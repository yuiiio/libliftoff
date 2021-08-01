/* Minimalistic example: create a few layers and display as many of them as
 * possible. Layers that don't make it into a plane won't be displayed. */

#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <fcntl.h>
#include <libliftoff.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
#include "common.h"

#define LAYERS_LEN 4

/* ARGB 8:8:8:8 */
static const uint32_t colors[] = {
	0xFFFF0000, /* red */
	0xFF00FF00, /* green */
	0xFF0000FF, /* blue */
	0xFFFFFF00, /* yellow */
};

static struct liftoff_layer *add_layer(int drm_fd, struct liftoff_output *output,
				       int x, int y, int width, int height,
				       bool with_alpha)
{
	static bool first = true;
	static size_t color_idx = 0;
	struct dumb_fb fb = {0};
	uint32_t color;
	struct liftoff_layer *layer;

	uint32_t format = with_alpha ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888;
	if (!dumb_fb_init(&fb, drm_fd, format, width, height)) {
		fprintf(stderr, "failed to create framebuffer\n");
		return NULL;
	}
	printf("Created FB %d with size %dx%d\n", fb.id, width, height);

	if (first) {
		color = 0xFFFFFFFF;
		first = false;
	} else {
		color = colors[color_idx];
		color_idx = (color_idx + 1) % (sizeof(colors) / sizeof(colors[0]));
	}

	dumb_fb_fill(&fb, drm_fd, color);

	layer = liftoff_layer_create(output);
	liftoff_layer_set_property(layer, "FB_ID", fb.id);
	liftoff_layer_set_property(layer, "CRTC_X", x);
	liftoff_layer_set_property(layer, "CRTC_Y", y);
	liftoff_layer_set_property(layer, "CRTC_W", width);
	liftoff_layer_set_property(layer, "CRTC_H", height);
	liftoff_layer_set_property(layer, "SRC_X", 0);
	liftoff_layer_set_property(layer, "SRC_Y", 0);
	liftoff_layer_set_property(layer, "SRC_W", width << 16);
	liftoff_layer_set_property(layer, "SRC_H", height << 16);

	return layer;
}

struct modeset_dev {
	struct modeset_dev *next;

	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t size;
	uint32_t handle;
	uint8_t *map;

	drmModeModeInfo mode;
	uint32_t fb;
	uint32_t conn;
	uint32_t crtc;
	drmModeCrtc *saved_crtc;
};

static struct modeset_dev *modeset_list = NULL;

static int modeset_find_crtc(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	drmModeEncoder *enc;
	int i, j;
	uint32_t crtc;
	struct modeset_dev *iter;
	bool check;

	/* first try the currently conected encoder+crtc */
	if (conn->encoder_id)
		enc = drmModeGetEncoder(fd, conn->encoder_id);
	else
		enc = NULL;

	if (enc) {
		if (enc->crtc_id) {
			check = false;
			crtc = enc->crtc_id;
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == crtc) {
					//crtc = -1;
					check = true;
					break;
				}
			}

			if (!check) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	/* If the connector is not currently bound to an encoder or if the
	 * encoder+crtc is already used by another connector (actually unlikely
	 * but lets be safe), iterate all other available encoders to find a
	 * matching CRTC. */
	for (i = 0; i < conn->count_encoders; ++i) {
		enc = drmModeGetEncoder(fd, conn->encoders[i]);
		if (!enc) {
			fprintf(stderr, "cannot retrieve encoder %u:%u\n",
				i, conn->encoders[i]);
			continue;
		}

		/* iterate all global CRTCs */
		for (j = 0; j < res->count_crtcs; ++j) {
			/* check whether this CRTC works with the encoder */
			if (!(enc->possible_crtcs & (1 << j)))
				continue;

			check = false;
			/* check that no other device already uses this CRTC */
			crtc = res->crtcs[j];
			for (iter = modeset_list; iter; iter = iter->next) {
				if (iter->crtc == crtc) {
					//crtc = -1;
					check = true;
					break;
				}
			}

			/* we have found a CRTC, so save it and return */
			if (!check) {
				drmModeFreeEncoder(enc);
				dev->crtc = crtc;
				return 0;
			}
		}

		drmModeFreeEncoder(enc);
	}

	fprintf(stderr, "cannot find suitable CRTC for connector %u\n",
		conn->connector_id);
	return -ENOENT;
}

static int modeset_create_fb(int fd, struct modeset_dev *dev)
{
	struct drm_mode_create_dumb creq;
	struct drm_mode_destroy_dumb dreq;
	struct drm_mode_map_dumb mreq;
	int ret;

	/* create dumb buffer */
	memset(&creq, 0, sizeof(creq));
	creq.width = dev->width;
	creq.height = dev->height;
	creq.bpp = 32;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq);
	if (ret < 0) {
		fprintf(stderr, "cannot create dumb buffer\n");
		return -errno;
	}
	dev->stride = creq.pitch;
	dev->size = creq.size;
	dev->handle = creq.handle;

	/* create framebuffer object for the dumb-buffer */
	ret = drmModeAddFB(fd, dev->width, dev->height, 24, 32, dev->stride,
			dev->handle, &dev->fb);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer\n");
		ret = -errno;
		goto err_destroy;
	}

	/* prepare buffer for memory mapping */
	memset(&mreq, 0, sizeof(mreq));
	mreq.handle = dev->handle;
	ret = drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq);
	if (ret) {
		fprintf(stderr, "cannot map dumb buffer\n");
		ret = -errno;
		goto err_fb;
	}


	/* perform actual memory mapping */
	dev->map = mmap(0, dev->size, PROT_READ | PROT_WRITE, MAP_SHARED,
			fd, mreq.offset);
	//fprintf(stderr, "dev->map = %p\n", dev->map);
	if (dev->map == MAP_FAILED) {
		fprintf(stderr, "cannot mmap dumb buffer\n");
		ret = -errno;
		goto err_fb;
	}

	/* clear the framebuffer to 0 */
	memset(dev->map, 0x71, dev->size);

	return 0;

err_fb:
	drmModeRmFB(fd, dev->fb);
err_destroy:
	memset(&dreq, 0, sizeof(dreq));
	dreq.handle = dev->handle;
	drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
	return ret;
}

static int modeset_setup_dev(int fd, drmModeRes *res, drmModeConnector *conn,
			     struct modeset_dev *dev)
{
	int ret;

	/* check if a monitor is connected */
	if (conn->connection != DRM_MODE_CONNECTED) {
		fprintf(stderr, "ignoring unused connector %u\n",
			conn->connector_id);
		return -ENOENT;
	}

	/* check if there is at least one valid mode */
	if (conn->count_modes == 0) {
		fprintf(stderr, "no valid mode for connector %u\n",
			conn->connector_id);
		return -EFAULT;
	}

	/* copy the mode information into our device structure */
	memcpy(&dev->mode, &conn->modes[0], sizeof(dev->mode));
	dev->width = conn->modes[0].hdisplay;
	dev->height = conn->modes[0].vdisplay;
	fprintf(stderr, "mode for connector %u is %ux%u\n",
		conn->connector_id, dev->width, dev->height);

	/* find a crtc for this connector */
	ret = modeset_find_crtc(fd, res, conn, dev);
	if (ret) {
		fprintf(stderr, "no valid crtc for connector %u\n",
			conn->connector_id);
		return ret;
	}

	/* create a framebuffer for this CRTC */
	ret = modeset_create_fb(fd, dev);
	if (ret) {
		fprintf(stderr, "cannot create framebuffer for connector %u\n",
			conn->connector_id);
		return ret;
	}


	return 0;
}

static int modeset_prepare(int fd, drmModeRes *res)
{
	drmModeConnector *conn;
	int i;
	struct modeset_dev *dev;
	int ret;

	/* iterate all connectors */
	for (i = 0; i < res->count_fbs; ++i) {
		fprintf(stderr, "FB #%d\n", i);
	}

	for (i = 0; i < res->count_crtcs; ++i) {
		drmModeCrtc *crtc;
		fprintf(stderr, "CRTC #%d\n", i);
		fprintf(stderr, "    id = %d\n", res->crtcs[i]);
		crtc = drmModeGetCrtc(fd, res->crtcs[i]);
		fprintf(stderr, "    %dx%d\n", crtc->width, crtc->height);
		drmModeFreeCrtc(crtc);
	}

	for (i = 0; i < res->count_encoders; ++i) {
		fprintf(stderr, "Encoder #%d\n", i);
	}

	/* iterate all connectors */
	for (i = 0; i < res->count_connectors; ++i) {
		fprintf(stderr, "Connector #%d\n", i);
		/* get information for each connector */
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (!conn) {
			fprintf(stderr, "cannot retrieve DRM connector %u:%u:\n",
					i, res->connectors[i]);
			continue;
		}

		/* create a device structure */
		dev = malloc(sizeof(*dev));
		memset(dev, 0, sizeof(*dev));
		dev->conn = conn->connector_id;

		/* call helper function to prepare this connector */
		ret = modeset_setup_dev(fd, res, conn, dev);
		if (ret) {
			if (ret != -ENOENT) {
				errno = -ret;
				fprintf(stderr, "cannot setup device for connector %u:%u\n",
						i, res->connectors[i]);
			}
			free(dev);
			drmModeFreeConnector(conn);
			continue;
		}

		/* free connector data and link device into global list */
		drmModeFreeConnector(conn);
		dev->next = modeset_list;
		modeset_list = dev;
	}

	return 0;
}

int main(int argc, char *argv[])
{
	int drm_fd;
	struct liftoff_device *device;
	drmModeRes *drm_res;
	drmModeCrtc *crtc;
	struct liftoff_output *output;
	struct liftoff_layer *layers[LAYERS_LEN];
	struct liftoff_plane *plane;
	drmModeAtomicReq *req;
	int ret;
	size_t i;
	uint32_t flags;

	const char *card;

	if (argc > 1)
		card = argv[1];
	else
		card = "/dev/dri/card0";

	fprintf(stderr, "using card '%s'\n", card);


	drm_fd = open(card, O_RDWR | O_CLOEXEC);
	if (drm_fd < 0) {
		perror("open");
		return 1;
	}

	if (drmSetClientCap(drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) < 0) {
		perror("drmSetClientCap(ATOMIC)");
		return 1;
	}

	device = liftoff_device_create(drm_fd);
	if (device == NULL) {
		perror("liftoff_device_create");
		return 1;
	}

	liftoff_device_register_all_planes(device);

	drm_res = drmModeGetResources(drm_fd);
	ret = modeset_prepare(drm_fd, drm_res);
	if (ret)
	{
		close(drm_fd);
		return ret;
	}

	struct modeset_dev *iter;
	/* perform actual modesetting on each found connector+CRTC */
	for (iter = modeset_list; iter; iter = iter->next) {
		iter->saved_crtc = drmModeGetCrtc(drm_fd, iter->crtc);
		ret = drmModeSetCrtc(drm_fd, iter->crtc, iter->fb, 0, 0,
				     &iter->conn, 1, &iter->mode);
		if (ret)
			fprintf(stderr, "cannot set CRTC for connector %u\n",
				iter->conn);
	}

	crtc = modeset_list->saved_crtc; //use first one

	if (crtc == NULL || !crtc->mode_valid) {
		fprintf(stderr, "no CRTC found\n");
		return 1;
	}

	disable_all_crtcs_except(drm_fd, drm_res, crtc->crtc_id);
	output = liftoff_output_create(device, crtc->crtc_id);
	drmModeFreeResources(drm_res);


	layers[0] = add_layer(drm_fd, output, 0, 0, crtc->mode.hdisplay,
			      crtc->mode.vdisplay, false);
	for (i = 1; i < LAYERS_LEN; i++) {
		layers[i] = add_layer(drm_fd, output, 100 * i, 100 * i,
				      256, 256, i % 2);
	}

	for (i = 0; i < LAYERS_LEN; i++) {
		liftoff_layer_set_property(layers[i], "zpos", i);
	}

	flags = DRM_MODE_ATOMIC_NONBLOCK;
	req = drmModeAtomicAlloc();
	ret = liftoff_output_apply(output, req, flags);
	if (ret != 0) {
		perror("liftoff_output_apply");
		return 1;
	}

	ret = drmModeAtomicCommit(drm_fd, req, flags, NULL);
	if (ret < 0) {
		perror("drmModeAtomicCommit");
		return 1;
	}

	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		plane = liftoff_layer_get_plane(layers[i]);
		if (plane != NULL) {
			printf("Layer %zu got assigned to plane %u\n", i,
			       liftoff_plane_get_id(plane));
		} else {
			printf("Layer %zu has no plane assigned\n", i);
		}
	}

	sleep(1);

	drmModeAtomicFree(req);
	for (i = 0; i < sizeof(layers) / sizeof(layers[0]); i++) {
		liftoff_layer_destroy(layers[i]);
	}
	liftoff_output_destroy(output);
	drmModeFreeCrtc(crtc);
	liftoff_device_destroy(device);
	close(drm_fd);
	return 0;

}
