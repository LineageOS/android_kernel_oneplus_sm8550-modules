// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015-2019, The Linux Foundation. All rights reserved.
 * Copyright (C) 2017-2020, Pixelworks, Inc.
 *
 * These files contain modifications made by Pixelworks, Inc., in 2019-2020.
 */
#include <drm/drm_mipi_dsi.h>
#include <video/mipi_display.h>
#include <dsi_drm.h>
#include <sde_encoder_phys.h>
#include <sde_trace.h>
#include "dsi_parser.h"
#include "dsi_iris_api.h"
#include "dsi_iris_lightup.h"
#include "dsi_iris_lightup_ocp.h"
#include "dsi_iris_lp.h"
#include "dsi_iris_pq.h"
#include "dsi_iris_ioctl.h"
#include "dsi_iris_lut.h"
#include "dsi_iris_loop_back.h"
#include "dsi_iris_gpio.h"
#include "dsi_iris_timing_switch.h"
#include "dsi_iris_log.h"
#include "dsi_iris_memc.h"
#include "dsi_iris_i3c.h"
#include "dsi_iris_cmpt.h"
#include "dsi_iris_dts_fw.h"
#include "dsi_iris_emv_i7.h"


int iris_dbgfs_status_init(struct dsi_display *display);

void iris_init_i7(struct dsi_display *display, struct dsi_panel *panel)
{
	struct iris_cfg *pcfg = iris_get_cfg();

	IRIS_LOGI("%s(), for dispaly: %s, panel: %s",
			__func__,
			display->display_type, panel->name);

	if (iris_virtual_display(display)) {
		pcfg->display2 = display;
		pcfg->panel2 = panel;
		return;
	}

	pcfg->display = display;
	pcfg->panel = panel;
	pcfg->iris_i2c_read = iris_ioctl_i2c_read;
	pcfg->iris_i2c_write = iris_ioctl_i2c_write;
	pcfg->iris_i2c_burst_write = iris_ioctl_i2c_burst_write;
	pcfg->aod = false;
	pcfg->fod = false;
	pcfg->fod_pending = false;
	pcfg->platform_type = 1; //need to use ASIC
	pcfg->abyp_ctrl.abypass_mode = ANALOG_BYPASS_MODE; //default abyp

	pcfg->pt_sr_enable = false;
	pcfg->pt_sr_enable_restore = false;
	pcfg->n2m_ratio = 1;
	pcfg->dtg_ctrl_pt = 0;
	pcfg->iris_mipi1_power_st = false;
	pcfg->ap_mipi1_power_st = false;
	pcfg->iris_pwil_blend_st = false;
	pcfg->iris_mipi1_power_on_pending = false;
	pcfg->iris_pwil_mode_state = 2;

	pcfg->frc_pq_guided_level = 1;
	pcfg->frc_pq_dejaggy_level = 1;
	pcfg->frc_pq_peaking_level = 1;
	pcfg->frc_pq_DLTI_level = 1;

	pcfg->frcgame_pq_guided_level = 0;
	pcfg->frcgame_pq_dejaggy_level = 0;
	pcfg->frcgame_pq_peaking_level = 0;
	pcfg->frcgame_pq_DLTI_level = 0;

	pcfg->osd_label = 0;
	pcfg->frc_label = 0;
	pcfg->frc_demo_window = 0;
	pcfg->ocp_read_by_i2c = 1;

	atomic_set(&pcfg->fod_cnt, 0);
	atomic_set(&pcfg->iris_esd_flag, 0);

	iris_init_memc();
	iris_init_timing_switch();
	iris_lp_init();

#ifdef IRIS_EXT_CLK // skip ext clk
	pcfg->ext_clk = devm_clk_get(&display->pdev->dev, "div_clk");
#endif

	if (!iris_virtual_display(display)) {
		iris_dbgfs_lp_init(display);
		iris_dbgfs_pq_init(display);
		iris_dbgfs_cont_splash_init(display);
		iris_dbgfs_memc_init(display);
		iris_dbgfs_loop_back_init(display);
		iris_dbgfs_adb_type_init(display);
		iris_dbgfs_fw_calibrate_status_init();
		iris_dbgfs_status_init(display);
		iris_dbg_gpio_init();
	}
	iris_get_vreg();
	mutex_init(&pcfg->gs_mutex);
	mutex_init(&pcfg->ioctl_mutex);
	mutex_init(&pcfg->i2c_read_mutex);
	iris_driver_register();
}


void iris_mult_addr_pad_i7(uint8_t **p, uint32_t *poff, uint32_t left_len)
{

	switch (left_len) {
	case 4:
		iris_set_ocp_type(*p, 0x00000405);
		*p += 4;
		*poff += 4;
		break;
	case 8:
		iris_set_ocp_type(*p, 0x00000805);
		iris_set_ocp_base_addr(*p, 0x00000000);
		*p += 8;
		*poff += 8;
		break;
	case 12:
		iris_set_ocp_type(*p, 0x00000c05);
		iris_set_ocp_base_addr(*p, 0x00000000);
		iris_set_ocp_first_val(*p, 0x00000000);
		*p += 12;
		*poff += 12;
		break;
	case 0:
		break;
	default:
		IRIS_LOGE("%s()%d, left len not aligh to 4.", __func__, __LINE__);
		break;
	}
}

int iris_lightoff_i7(struct dsi_panel *panel, bool dead,
		struct dsi_panel_cmd_set *off_cmds)
{
	struct iris_cfg *pcfg = iris_get_cfg();
	int lightup_opt = iris_lightup_opt_get();

	if (pcfg->valid < PARAM_PREPARED) {
		if (panel && !panel->is_secondary && off_cmds)
			iris_abyp_send_panel_cmd(panel, off_cmds);
		return 0;
	}

	pcfg->mipi2_pwr_st = false;
	pcfg->metadata = 0; // clean metadata
	pcfg->dtg_ctrl_pt = 0;

	if (!panel || panel->is_secondary) {
		IRIS_LOGD("no need to light off for 2nd panel.");
		return 0;
	}

	if ((lightup_opt & 0x10) == 0)
		pcfg->abyp_ctrl.abypass_mode = ANALOG_BYPASS_MODE; //clear to ABYP mode

	IRIS_LOGI("%s(%d), panel %s, mode: %s(%d) ---", __func__, __LINE__,
			dead ? "dead" : "alive",
			pcfg->abyp_ctrl.abypass_mode == PASS_THROUGH_MODE ? "PT" : "ABYP",
			pcfg->abyp_ctrl.abypass_mode);
	if (off_cmds && (!dead)) {
		if (pcfg->abyp_ctrl.abypass_mode == PASS_THROUGH_MODE)
			iris_pt_send_panel_cmd(panel, off_cmds);
		else
			iris_abyp_send_panel_cmd(panel, off_cmds);
	}
	irisSetDisableDsppPq_i7(false);
	iris_lightoff_memc();
	iris_quality_setting_off();
	iris_lp_setting_off();
	iris_pt_sr_reset();
	iris_dtg_update_reset();
	iris_clear_aod_state();
	pcfg->panel_pending = 0;
	iris_set_pinctrl_state(false);

	IRIS_LOGI("%s(%d) ---", __func__, __LINE__);

	return 0;
}

//module_platform_driver(iris_driver);
