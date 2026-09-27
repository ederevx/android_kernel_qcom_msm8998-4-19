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

#ifndef _FLICKER_FREE_H
#define _FLICKER_FREE_H

struct device_node;
struct msm_fb_data_type;

/* Use this function to remap the physical backlight level */
uint32_t mdss_panel_calc_backlight(uint32_t bl_lvl);

/* Use this function to read the panel's ELVSS threshold from DT */
void mdss_panel_parse_elvss_off_thresh(struct device_node *np);

/* Use this function to track the framebuffer the display thread runs on */
void mdss_fb_update_flicker_free_mfd(struct msm_fb_data_type *mfd);

/* Expose the flicker free state to the LiveDisplay HAL */
int mdss_fb_create_flicker_free_sysfs(struct msm_fb_data_type *mfd);

#endif  /* _FLICKER_FREE_H */
