/*
 * A flicker free driver based on Qcom MDSS for OLED devices
 *
 * Copyright (C) 2012-2014, The Linux Foundation. All rights reserved.
 * Copyright (C) Sony Mobile Communications Inc. All rights reserved.
 * Copyright (C) 2014-2018, AngeloGioacchino Del Regno <kholk11@gmail.com>
 * Copyright (C) 2018, Devries <therkduan@gmail.com>
 * Copyright (C) 2019-2020, Tanish <tanish2k09.dev@gmail.com>
 * Copyright (C) 2020, shxyke <shxyke@gmail.com>
 * Copyright (C) 2022, ederekun <sedrickvince@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/device.h>
#include <linux/fb.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sysfs.h>

#include "mdss_fb.h"
#include "mdss_mdp.h"

#include "flicker_free.h"

/* Maximum value of RGB possible */
#define FF_MAX_SCALE 32768

/* Minimum value of RGB recommended */
#define FF_MIN_SCALE 2560

/* Number of backlight entries */
#define BACKLIGHT_INDEX 66

/* Minimum backlight value that does not flicker */
static int elvss_off_threshold __read_mostly = 66;

/* Display configuration data */
static const int bkl_to_pcc[BACKLIGHT_INDEX] = {
	42, 56, 67, 75, 84, 91, 98, 104, 109, 114, 119, 124, 128, 133, 136,
	140, 143, 146, 150, 152, 156, 159, 162, 165, 168, 172, 176, 178, 181,
	184, 187, 189, 192, 194, 196, 199, 202, 204, 206, 209, 211, 213, 215,
	217, 220, 222, 224, 226, 228, 230, 233, 236, 237, 239, 241, 241, 243,
	245, 246, 249, 249, 250, 252, 254, 255, 256
};

static const uint32_t pcc_depth[9] = {
	128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768
};

static struct mdp_pcc_cfg_data pcc_config;
static struct mdp_dither_cfg_data dither_config;
static struct mdp_dither_data_v1_7 *dither_payload;
static struct mdp_pcc_data_v1_7 *payload;
static struct msm_fb_data_type *ff_mfd;

static uint32_t dither_copyback = 0;
static uint32_t copyback = 0;

/* State booleans */
static bool pcc_enabled = false;
static bool mdss_backlight_enable = false;

static inline int flicker_free_push(struct msm_fb_data_type *mfd, int bl_lvl,
					bool enable)
{
	uint32_t backlight, temp, depth;

	/* Override bl_lvl if we're disabling flicker free */
	if (!enable)
		bl_lvl = elvss_off_threshold;

	backlight = clamp_t(int, (((bl_lvl - 1) * (BACKLIGHT_INDEX - 1)) /
					(elvss_off_threshold - 1)) + 1, 1, BACKLIGHT_INDEX);
	temp = clamp_t(int, 0x80 * bkl_to_pcc[backlight - 1], FF_MIN_SCALE,
					FF_MAX_SCALE);

	for (depth = 8; depth >= 1; depth--) {
		if (temp >= pcc_depth[depth])
			break;
	}

	/* Configure dither values */
	dither_config.flags = MDP_PP_OPS_WRITE;
	if (mdss_backlight_enable)
		dither_config.flags |= MDP_PP_OPS_ENABLE;
	else
		dither_config.flags |= MDP_PP_OPS_DISABLE;

	dither_payload->r_cr_depth = dither_config.r_cr_depth = depth;
	dither_payload->g_y_depth = dither_config.g_y_depth = depth;
	dither_payload->b_cb_depth = dither_config.b_cb_depth = depth;

	dither_payload->len = dither_payload->temporal_en = 0;

	dither_config.cfg_payload = dither_payload;

	/* Configure pcc values */
	pcc_config.ops = MDP_PP_OPS_WRITE;
	if (enable)
		pcc_config.ops |= MDP_PP_OPS_ENABLE;
	else
		pcc_config.ops |= MDP_PP_OPS_DISABLE;

	payload->r.r = pcc_config.r.r = temp;
	payload->g.g = pcc_config.g.g = temp;
	payload->b.b = pcc_config.b.b = temp;

	pcc_config.cfg_payload = payload;

	/* Push values consecutively */
	return mdss_mdp_dither_config(mfd, &dither_config, &dither_copyback, 1) ||
				mdss_mdp_kernel_pcc_config(mfd, &pcc_config, &copyback);
}

uint32_t mdss_panel_calc_backlight(uint32_t bl_lvl)
{
	struct msm_fb_data_type *mfd = ff_mfd;
	bool target_pcc_en = mdss_backlight_enable;

	if (!mfd || !bl_lvl)
		return bl_lvl;

	/* We don't need flicker free above elvss */
	if (bl_lvl >= elvss_off_threshold)
		target_pcc_en = false;

	if (target_pcc_en || pcc_enabled) {
		/* Only configure target pcc_enabled if we're successful */
		if (!flicker_free_push(mfd, bl_lvl, target_pcc_en))
			pcc_enabled = target_pcc_en;

		if (pcc_enabled)
			return elvss_off_threshold;
	}

	return bl_lvl;
}

/*
 * LiveDisplay anti-flicker interface
 *
 * IAntiFlicker is a dual state mode: it reads an integer and treats any
 * value greater than zero as enabled, and writes "0" or "1". Expose the
 * flicker free state as a plain sysfs attribute on the framebuffer device
 * so that the HAL can drive it without a sysfs path of its own.
 */
static ssize_t flicker_free_get_dc(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	/* Check ff_mfd, without mfd we cannot use flicker free */
	return scnprintf(buf, PAGE_SIZE, "%d\n",
			(mdss_backlight_enable && ff_mfd != NULL));
}

static ssize_t flicker_free_set_dc(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count)
{
	struct msm_fb_data_type *mfd = ff_mfd;
	int value;
	bool state;

	if (kstrtoint(buf, 0, &value))
		return -EINVAL;

	if (!payload || !dither_payload)
		return -ENODEV;

	if (!mfd)
		return count;

	state = (value != 0);

	if (mdss_backlight_enable != state) {
		/* Reset backlight */
		mutex_lock(&mfd->bl_lock);
		mfd->unset_bl_level = mfd->bl_level_usr;
		mfd->allow_bl_update = false;
		mutex_unlock(&mfd->bl_lock);

		mdss_backlight_enable = state;
		mdss_fb_update_backlight(mfd);
	}

	return count;
}

static DEVICE_ATTR(dc, S_IRUGO | S_IWUSR | S_IWGRP,
		flicker_free_get_dc, flicker_free_set_dc);

int mdss_fb_create_flicker_free_sysfs(struct msm_fb_data_type *mfd)
{
	int rc;

	rc = sysfs_create_file(&mfd->fbi->dev->kobj, &dev_attr_dc.attr);
	if (rc)
		pr_err("mdss_fb: flicker free sysfs creation failed, rc=%d\n",
				rc);

	return rc;
}

void mdss_fb_update_flicker_free_mfd(struct msm_fb_data_type *mfd)
{
	ff_mfd = mfd;
}

static void mdss_panel_set_elvss_off_threshold(int val)
{
	elvss_off_threshold = val;
}

/*
 * Flicker free only applies below the panel's ELVSS threshold, and the
 * panels this driver supports do not share one value.
 */
void mdss_panel_parse_elvss_off_thresh(struct device_node *np)
{
	u32 thresh;

	if (!of_property_read_u32(np, "qcom,mdss-dsi-panel-elvss-off-thresh",
					&thresh))
		mdss_panel_set_elvss_off_threshold(thresh);
}

static int __init mdss_fb_flicker_free_init(void)
{
	/* Display config init */
	memset(&pcc_config, 0, sizeof(struct mdp_pcc_cfg_data));

	pcc_config.version = mdp_pcc_v1_7;
	pcc_config.block = MDP_LOGICAL_BLOCK_DISP_0;
	payload = kzalloc(sizeof(struct mdp_pcc_data_v1_7), GFP_USER);

	memset(&dither_config, 0, sizeof(struct mdp_dither_cfg_data));

	dither_config.version = mdp_dither_v1_7;
	dither_config.block = MDP_LOGICAL_BLOCK_DISP_0;
	dither_payload = kzalloc(sizeof(struct mdp_dither_data_v1_7), GFP_USER);

	if (!payload || !dither_payload) {
		kfree(payload);
		kfree(dither_payload);

		pr_err("mdss_fb: Failed to initialize flicker free driver\n");

		return -ENOMEM;
	}

	return 0;
}
late_initcall(mdss_fb_flicker_free_init);
