// SPDX-License-Identifier: GPL-2.0
/* Static boot splash drawn by a drm_client (fbcon logos are unavailable to module
 * framebuffers); it stays until HWC commits. */

#include <linux/container_of.h>
#include <linux/err.h>
#include <linux/iosys-map.h>
#include <linux/jiffies.h>
#include <linux/minmax.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#include <drm/drm_client.h>
#include <drm/drm_device.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_mode.h>
#include <drm/drm_print.h>
#include <drm/drm_rect.h>

#include "spacemit_drm.h"

/* Raw XRGB8888 image embedded by spacemit_bootsplash_data.S. */
extern const u8 spacemit_bootsplash_data[];
extern const u8 spacemit_bootsplash_data_end[];

#define SPACEMIT_BOOTSPLASH_WIDTH  1920
#define SPACEMIT_BOOTSPLASH_HEIGHT 1080
#define SPACEMIT_BOOTSPLASH_BPP    4

/* Draw from a workqueue: a modeset right at probe raced dpu_init() and underran. */
#define SPACEMIT_BOOTSPLASH_DELAY_MS 800

/* No output may be connected yet: retry on hotplug and on a timer until drawn. */
#define SPACEMIT_BOOTSPLASH_RETRY_MS	250
#define SPACEMIT_BOOTSPLASH_MAX_TRIES	80	/* ~20 s */
#define SPACEMIT_BOOTSPLASH_HOTPLUG_MS	200	/* let the link settle after a hotplug */

struct spacemit_bootsplash {
	struct drm_client_dev client;
	struct delayed_work work;
	struct drm_client_buffer *buffers[4];	/* drawn but not (yet) on screen, freed on retry */
	unsigned int num_buffers;
	unsigned int tries;
	bool registered;
	bool done;	/* shown, or given up (someone else owns the display) */
};

static void spacemit_bootsplash_client_free(struct drm_client_dev *client)
{
	struct spacemit_bootsplash *splash =
		container_of(client, struct spacemit_bootsplash, client);

	kfree(splash);
}

static int spacemit_bootsplash_client_hotplug(struct drm_client_dev *client)
{
	struct spacemit_bootsplash *splash =
		container_of(client, struct spacemit_bootsplash, client);

	/* Only registered once the first attempt ran, so this never overtakes the settle delay. */
	if (!READ_ONCE(splash->done))
		mod_delayed_work(system_wq, &splash->work,
				 msecs_to_jiffies(SPACEMIT_BOOTSPLASH_HOTPLUG_MS));
	return 0;
}

static const struct drm_client_funcs spacemit_bootsplash_client_funcs = {
	.owner  = THIS_MODULE,
	.free   = spacemit_bootsplash_client_free,
	.hotplug = spacemit_bootsplash_client_hotplug,
};

static void spacemit_bootsplash_draw(struct drm_client_buffer *buffer)
{
	struct drm_framebuffer *fb = buffer->fb;
	struct iosys_map map;
	u32 copy_w = min_t(u32, fb->width, SPACEMIT_BOOTSPLASH_WIDTH);
	u32 copy_h = min_t(u32, fb->height, SPACEMIT_BOOTSPLASH_HEIGHT);
	struct drm_rect r = DRM_RECT_INIT(0, 0, fb->width, fb->height);
	u32 y;

	if (drm_client_buffer_vmap_local(buffer, &map))
		return;

	if (copy_w < fb->width || copy_h < fb->height)
		iosys_map_memset(&map, 0, 0, (size_t)fb->pitches[0] * fb->height);

	for (y = 0; y < copy_h; y++) {
		const u8 *src = spacemit_bootsplash_data +
				(size_t)y * SPACEMIT_BOOTSPLASH_WIDTH * SPACEMIT_BOOTSPLASH_BPP;

		iosys_map_memcpy_to(&map, (size_t)y * fb->pitches[0], src,
				    (size_t)copy_w * SPACEMIT_BOOTSPLASH_BPP);
	}

	drm_client_buffer_vunmap_local(buffer);
	drm_client_buffer_flush(buffer, &r);
}

static void spacemit_bootsplash_work_fn(struct work_struct *w)
{
	struct spacemit_bootsplash *splash =
		container_of(w, struct spacemit_bootsplash, work.work);
	struct drm_client_dev *client = &splash->client;
	struct drm_device *drm = client->dev;
	struct drm_mode_set *mode_set;
	bool drawn = false;
	int ret;

	if (splash->done)
		return;
	splash->tries++;

	/* Nothing from an earlier attempt reached the screen (see below), drop it. */
	while (splash->num_buffers)
		drm_client_buffer_delete(splash->buffers[--splash->num_buffers]);

	ret = drm_client_modeset_probe(client, SPACEMIT_BOOTSPLASH_WIDTH,
				       SPACEMIT_BOOTSPLASH_HEIGHT);
	if (ret)
		drm_warn(drm, "spacemit_bootsplash: modeset_probe failed: %d\n", ret);

	mutex_lock(&client->modeset_mutex);
	drm_client_for_each_modeset(mode_set, client) {
		struct drm_client_buffer *buffer;
		u32 width, height;

		if (!mode_set->mode)
			continue;
		if (splash->num_buffers >= ARRAY_SIZE(splash->buffers))
			break;

		width = mode_set->mode->hdisplay;
		height = mode_set->mode->vdisplay;

		buffer = drm_client_buffer_create_dumb(client, width, height,
						       DRM_FORMAT_XRGB8888);
		if (IS_ERR(buffer)) {
			drm_warn(drm,
				"spacemit_bootsplash: can't create %ux%u XRGB8888 buffer: %ld\n",
				width, height, PTR_ERR(buffer));
			continue;
		}

		spacemit_bootsplash_draw(buffer);
		mode_set->fb = buffer->fb;
		splash->buffers[splash->num_buffers++] = buffer;
		drawn = true;
	}
	mutex_unlock(&client->modeset_mutex);

	if (drawn) {
		ret = drm_client_modeset_commit(client);
		if (ret == -EBUSY) {
			/* userspace (SurfaceFlinger) is already the DRM master */
			drm_info(drm, "spacemit_bootsplash: display already taken over\n");
			WRITE_ONCE(splash->done, true);
		} else if (ret) {
			drm_warn(drm, "spacemit_bootsplash: modeset_commit failed: %d\n", ret);
		} else {
			drm_info(drm, "spacemit_bootsplash: splash displayed (attempt %u)\n",
				 splash->tries);
			/* on screen now: keep the buffers, they are scanned out */
			splash->num_buffers = 0;
			WRITE_ONCE(splash->done, true);
		}
	}

	/* Stay registered so a later hotplug/restore keeps the splash. */
	if (!splash->registered) {
		drm_client_register(client);
		splash->registered = true;
	}

	if (!READ_ONCE(splash->done)) {
		if (splash->tries >= SPACEMIT_BOOTSPLASH_MAX_TRIES) {
			drm_warn(drm, "spacemit_bootsplash: no output to draw on, giving up\n");
			WRITE_ONCE(splash->done, true);
		} else {
			schedule_delayed_work(&splash->work,
					      msecs_to_jiffies(SPACEMIT_BOOTSPLASH_RETRY_MS));
		}
	}
}

/**
 * spacemit_bootsplash_show - schedule drawing the compiled-in splash (best effort)
 * @drm: registered DRM device
 */
void spacemit_bootsplash_show(struct drm_device *drm)
{
	struct spacemit_bootsplash *splash;
	int ret;

	splash = kzalloc(sizeof(*splash), GFP_KERNEL);
	if (!splash) {
		drm_warn(drm, "spacemit_bootsplash: no memory\n");
		return;
	}

	ret = drm_client_init(drm, &splash->client, "spacemit-bootsplash",
			      &spacemit_bootsplash_client_funcs);
	if (ret) {
		drm_warn(drm, "spacemit_bootsplash: drm_client_init failed: %d\n", ret);
		kfree(splash);
		return;
	}

	INIT_DELAYED_WORK(&splash->work, spacemit_bootsplash_work_fn);
	schedule_delayed_work(&splash->work,
			      msecs_to_jiffies(SPACEMIT_BOOTSPLASH_DELAY_MS));
}
