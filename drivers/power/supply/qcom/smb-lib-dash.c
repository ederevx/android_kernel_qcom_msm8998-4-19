// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2026, Edrick Sinsuan <evcsinsuan@gmail.com>
 *
 * OnePlus DASH fast-charge integration for the SMB2 charger.
 *
 * Split out from smb-lib.c/smb-lib.h so the shared SMB2 charger driver
 * stays untouched: only a handful of CONFIG_ARCH_ONEPLUS_MSM8998-guarded
 * hooks in smb-lib.c call into op_dash_charger_init()/
 * op_dash_charger_deinit() and the few other functions declared in
 * smb-lib.h.
 */

#include <linux/kernel.h>
#include <linux/power_supply.h>
#include <linux/regmap.h>
#include <linux/pmic-voter.h>
#include <linux/moduleparam.h>
#if defined(CONFIG_FB)
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

#include "smb-lib.h"
#include "smb-reg.h"

#define SOFT_CHG_TERM_CURRENT 100 /* 100mA */
#define CHG_SOFT_OVP_MV 5800
#define CHG_SOFT_UVP_MV 4300

enum chg_protect_status_type {
	PROTECT_CHG_OVP = 1, /* 1: VCHG > 5.8V     */
	PROTECT_BATT_MISSING, /* 2: battery missing */
	PROTECT_CHG_OVERTIME, /* 3: charge overtime */
	PROTECT_BATT_OVP, /* 4: vbat >= 4.5     */
	PROTECT_BATT_TEMP_REGION__HOT,/* 5: 55 < t          */
	PROTECT_BATT_TEMP_REGION_COLD,/* 6:      t <= -3    */
	PROTECT_BATT_TEMP_REGION_LITTLE_COLD, /* 7: -3 < t <=  0  */
	PROTECT_BATT_TEMP_REGION_COOL,/* 8:  0 < t <=  5    */
	PROTECT_BATT_TEMP_REGION_WARM /* 9: 45 < t <= 55   */
};

struct smb_charger *g_chg;

/* SOC-backup register access via the primary PMIC PON regmap. */
static struct regmap *pm_pon_regmap;

static struct external_battery_gauge *fast_charger;
char dump_val[2048];

int op_usb_icl_set(struct smb_charger *chg, int icl_ua);
int smblib_set_prop_chg_voltage(struct smb_charger *chg, const union power_supply_propval *val);
int smblib_set_prop_batt_temp(struct smb_charger *chg, const union power_supply_propval *val);
int smblib_set_prop_chg_protect_status(struct smb_charger *chg, const union power_supply_propval *val);
int smblib_set_prop_charge_parameter_set(struct smb_charger *chg);
static inline void op_dump_reg(struct smb_charger *chip, u16 addr_start, u16 addr_end);
void op_dump_regs(struct smb_charger *chip);
void op_handle_usb_plugin(struct smb_charger *chg);
int op_rerun_apsd(struct smb_charger *chg);
int op_get_aicl_result(struct smb_charger *chg);
static int get_property_from_fg(struct smb_charger *chg, enum power_supply_property prop, int *val);
int set_property_on_fg(struct smb_charger *chg, enum power_supply_property prop, int val);
int op_charging_en(struct smb_charger *chg, bool en);
static bool is_usb_present(struct smb_charger *chg);
static bool op_get_fast_low_temp_full(struct smb_charger *chg);
static bool get_fastchg_firmware_updated_status(struct smb_charger *chg);
static bool get_prop_fast_switch_to_normal(struct smb_charger *chg);
bool is_fastchg_allowed(struct smb_charger *chg);
bool get_oem_charge_done_status(void);
void op_handle_usb_removal(struct smb_charger *chg);
static void dash_to_normal_workaround(struct smb_charger *chg);
int update_dash_unplug_status(void);
int op_set_collapse_fet(struct smb_charger *chg, bool on);
int op_handle_switcher_power_ok(void);
int op_contrl(int enable, bool check_power_ok);
bool get_prop_fast_chg_started(struct smb_charger *chg);
int get_prop_fast_adapter_update(struct smb_charger *chg);
static bool set_prop_fast_switch_to_normal_false(struct smb_charger *chg);
bool op_get_fastchg_ing(struct smb_charger *chg);
bool op_set_fast_chg_allow(struct smb_charger *chg, bool enable);
static bool op_get_fast_chg_allow(struct smb_charger *chg);
static bool op_is_usb_switch_on(struct smb_charger *chg);
static enum batt_status_type op_battery_status_get(struct smb_charger *chg);
enum temp_region_type op_battery_temp_region_get(struct smb_charger *chg);
int fuelgauge_battery_temp_region_get(void);
static void op_battery_status_set(struct smb_charger *chg, enum batt_status_type battery_status);
static void op_battery_temp_region_set(struct smb_charger *chg, enum temp_region_type batt_temp_region);
static void set_usb_switch(struct smb_charger *chg, bool enable);
static int op_dash_disable_role_switch(struct smb_charger *chg, bool disable);
static void op_dash_enable_role_switch(struct smb_charger *chg);
static void switch_fast_chg(struct smb_charger *chg);
static void op_re_kick_allowed_voltage(struct smb_charger *chg);
static void op_re_kick_work(struct work_struct *work);
static void retrigger_dash_work(struct work_struct *work);
static void op_chek_apsd_done_work(struct work_struct *work);
static void op_recovery_usb_suspend_work(struct work_struct *work);
static void op_check_allow_switch_dash_work(struct work_struct *work);
int check_allow_switch_dash(struct smb_charger *chg, const union power_supply_propval *val);
static int set_dash_charger_present(int status);
static void op_check_charge_timeout(struct smb_charger *chg);
static int get_prop_batt_present(struct smb_charger *chg);
int get_prop_batt_capacity(struct smb_charger *chg);
int get_prop_batt_temp(struct smb_charger *chg);
int get_prop_batt_current_now(struct smb_charger *chg);
int get_prop_batt_voltage_now(struct smb_charger *chg);
int get_prop_fg_capacity(struct smb_charger *chg);
int get_prop_fg_current_now(struct smb_charger *chg);
int get_prop_fg_voltage_now(struct smb_charger *chg);
int get_prop_batt_status(struct smb_charger *chg);
int get_charging_status(void);
void set_chg_ibat_vbat_max(struct smb_charger *chg, int ibat, int vfloat);
static int handle_batt_temp_cold(struct smb_charger *chg);
static int handle_batt_temp_little_cold(struct smb_charger *chg);
static int handle_batt_temp_cool(struct smb_charger *chg);
static int handle_batt_temp_little_cool(struct smb_charger *chg);
static int handle_batt_temp_prenormal(struct smb_charger *chg);
static int handle_batt_temp_normal(struct smb_charger *chg);
static int handle_batt_temp_warm(struct smb_charger *chg);
static int handle_batt_temp_hot(struct smb_charger *chg);
int op_check_battery_temp(struct smb_charger *chg);
void op_charge_info_init(struct smb_charger *chg);
static int op_handle_battery_uovp(struct smb_charger *chg);
static int op_handle_battery_restore_from_uovp(struct smb_charger *chg);
static void op_check_battery_uovp(struct smb_charger *chg);
int op_get_charg_en(struct smb_charger *chg, int *chg_enabled);
static void op_check_charger_collapse(struct smb_charger *chg);
static int fb_notifier_callback(struct notifier_block *self, unsigned long event, void *data);
void checkout_term_current(struct smb_charger *chg, int batt_temp);
static void check_non_standard_charger_work(struct work_struct *work);
static void smbchg_re_det_work(struct work_struct *work);
static void op_recovery_set_work(struct work_struct *work);
void aging_test_check_aicl(struct smb_charger *chg);
static void op_heartbeat_work(struct work_struct *work);
static int op_read(struct smb_charger *chg, u16 addr, u8 *val);
int op_read_backup_flag(struct smb_charger *chg);
int op_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val);
void op_write_backup_flag(struct smb_charger *chg, bool bk_flag);
static int load_data(struct smb_charger *chg);
int load_soc(void);
static void clear_backup_soc(struct smb_charger *chg);
void clean_backup_soc_ex(void);
static void backup_soc(struct smb_charger *chg, int soc);
void backup_soc_ex(int soc);
int get_prop_chg_protect_status(struct smb_charger *chg);
bool get_prop_fastchg_status(struct smb_charger *chg);
void fastcharge_information_register(struct external_battery_gauge *fast_chg);
void fastcharge_information_unregister(struct external_battery_gauge *fast_chg);
static int notify_usb_enumeration_function(int status);

int op_usb_icl_set(struct smb_charger *chg, int icl_ua)
{
	int rc = 0;
	bool override;
	pr_info("%s,icl_ua=%d\n", __func__, icl_ua);
	disable_irq_nosync(chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);
	rc = smblib_set_charge_param(chg, &chg->param.usb_icl,
			icl_ua);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set HC ICL rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* determine if override needs to be enforced */
	override = true;
	/* enforce override */
	rc = smblib_masked_write(chg, USBIN_ICL_OPTIONS_REG,
		USBIN_MODE_CHG_BIT, override ? USBIN_MODE_CHG_BIT : 0);

	rc = smblib_icl_override(chg, override);
	if (rc < 0) {
		smblib_err(chg, "Couldn't set ICL override rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

	/* unsuspend after configuring current and override */
	rc = smblib_set_usb_suspend(chg, false);
	if (rc < 0) {
		smblib_err(chg, "Couldn't resume input rc=%d\n", rc);
		goto enable_icl_changed_interrupt;
	}

enable_icl_changed_interrupt:
	enable_irq(chg->irq_info[USBIN_ICL_CHANGE_IRQ].irq);
	return rc;
}

int smblib_set_prop_chg_voltage(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_batt_temp(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	chg->fake_temp = val->intval;
	chg->use_fake_temp = true;
	power_supply_changed(chg->batt_psy);

	return 0;
}

int smblib_set_prop_chg_protect_status(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	chg->fake_protect_sts = val->intval;
	chg->use_fake_protect_sts = true;
	power_supply_changed(chg->batt_psy);
	return 0;
}

int smblib_set_prop_charge_parameter_set(struct smb_charger *chg)
{
	chg->is_power_changed = true;
	op_check_battery_temp(chg);
	return 0;
}


static inline void op_dump_reg(struct smb_charger *chip,
	u16 addr_start, u16 addr_end)
{
	u8 reg;
	u16 addr;
	char reg_val[19];

	memset(dump_val, 0, sizeof(dump_val));
	for (addr = addr_start; addr <= addr_end; addr++) {
		memset(reg_val, 0, sizeof(reg_val));
		smblib_read(chip, addr, &reg);
		scnprintf(reg_val,
			sizeof(reg_val), "%x=%0x;", addr, reg);
		strlcat(dump_val, reg_val, sizeof(dump_val));
	}
	pr_info("%s\n", dump_val);
}

void op_dump_regs(struct smb_charger *chip)
{
	u16 addr, count;

	count = 0x80;
	for (addr = 0x1000; addr <= 0x1700; addr += count)
	op_dump_reg(chip, addr, (addr + count));
}

void op_handle_usb_plugin(struct smb_charger *chg)
{
	mutex_lock(&chg->lock);
	smblib_usb_plugin_locked(chg);
	smblib_usb_typec_change(chg);
	mutex_unlock(&chg->lock);
}

int op_rerun_apsd(struct smb_charger *chg)
{
	union power_supply_propval val;
	int rc;

	rc = smblib_get_prop_usb_present(chg, &val);
	if (rc < 0) {
		smblib_err(chg, "Couldn't get usb present rc = %d\n", rc);
		return rc;
	}

	if (!val.intval)
		return 0;
		/* rerun APSD */
		pr_info("OP Reruning APSD type\n");
		rc = smblib_masked_write(chg, CMD_APSD_REG,
					APSD_RERUN_BIT,
					APSD_RERUN_BIT);
		if (rc < 0) {
			smblib_err(chg, "Couldn't rerun APSD rc = %d\n", rc);
			return rc;
		}
	return 0;
}

int op_get_aicl_result(struct smb_charger *chg)
{
	int icl_ma, rc;

	rc = smblib_get_charge_param(chg,
				&chg->param.icl_stat, &icl_ma);
	if (rc < 0) {
		pr_err("Couldn't get ICL status rc=%d\n", rc);
		return  -EINVAL;
	}

	pr_info("AICL result=%d\n", icl_ma);
	return icl_ma;
}

static int get_property_from_fg(struct smb_charger *chg,
		enum power_supply_property prop, int *val)
{
	int rc;
	union power_supply_propval ret = {0, };

	if (!chg->bms_psy)
		chg->bms_psy = power_supply_get_by_name("bms");

	if (chg->bms_psy) {
		rc = power_supply_get_property(chg->bms_psy, prop, &ret);
		if (rc) {
			pr_err("bms psy doesn't support reading prop %d rc = %d\n",
				prop, rc);
			return rc;
		}
		*val = ret.intval;
	} else {
		pr_err("no bms psy found\n");
		return -EINVAL;
	}

	return rc;
}

int set_property_on_fg(struct smb_charger *chg,
		enum power_supply_property prop, int val)
{
	int rc;
	union power_supply_propval ret = {0, };

	if (!chg->bms_psy)
		chg->bms_psy = power_supply_get_by_name("bms");

	if (chg->bms_psy) {
		ret.intval = val;
		rc = power_supply_set_property(chg->bms_psy, prop, &ret);
		if (rc)
			pr_err("bms psy does not allow updating prop %d rc = %d\n",
				prop, rc);
	} else {
		pr_err("no bms psy found\n");
		return -EINVAL;
	}

	return rc;
}

int op_charging_en(struct smb_charger *chg, bool en)
{
	int rc;

	if (en && !chg->chg_enabled) {
		pr_warn("charging disabled by userspace\n");
		return 0;
	}

	pr_err("enable=%d\n", en);
	rc = smblib_masked_write(chg, CHARGING_ENABLE_CMD_REG,
				 CHARGING_ENABLE_CMD_BIT,
				 en ? CHARGING_ENABLE_CMD_BIT : 0);
	if (rc < 0) {
		pr_err("Couldn't %s charging rc=%d\n",
			en ? "enable" : "disable", rc);
		return rc;
	}

	return 0;
}

static bool is_usb_present(struct smb_charger *chg)
{
	int rc = 0;
	u8 stat;

	rc = smblib_read(chg, USBIN_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		pr_err("Couldn't read TYPE_C_STATUS_4 rc=%d\n", rc);
		return rc;
	}
	pr_debug("TYPE_C_STATUS_4 = 0x%02x\n", stat);
	return (bool)(stat & USBIN_PLUGIN_RT_STS_BIT);
}

static bool op_get_fast_low_temp_full(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fast_low_temp_full)
		return fast_charger->get_fast_low_temp_full();
	pr_err("no fast_charger register found\n");
	return false;
}

static bool get_fastchg_firmware_updated_status(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fastchg_firmware_already_updated)
		return fast_charger->get_fastchg_firmware_already_updated();
	pr_err("no fast_charger register found\n");
	return false;
}

static bool get_prop_fast_switch_to_normal(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->fast_switch_to_normal)
		return fast_charger->fast_switch_to_normal();
	pr_err("no fast_charger register found\n");
	return false;
}

bool is_fastchg_allowed(struct smb_charger *chg)
{
	int temp;
	static int pre_temp;
	static bool pre_switch_to_normal;
	bool low_temp_full, switch_to_normal, fw_updated;

	temp = get_prop_batt_temp(chg);
	low_temp_full = op_get_fast_low_temp_full(chg);
	fw_updated = get_fastchg_firmware_updated_status(chg);

	if (!fw_updated)
		return false;
	if (chg->usb_enum_status)
		return false;
	if (chg->dash_handshake_failed)
		return false;
	if (!chg->chg_enabled) {
		pr_err("fast charging is not allowed by userspace\n");
		return false;
	}
	if (temp < 165 || temp > 430) {
		if (temp != pre_temp)
			pr_err("temp=%d is not allow to swith fastchg\n", temp);
		pre_temp = temp;
		return false;
	}

	switch_to_normal = get_prop_fast_switch_to_normal(chg);
	if (pre_switch_to_normal != switch_to_normal)
		pr_info("switch_to_normal =%d\n", switch_to_normal);
	if (switch_to_normal)
		return false;

	return true;
}

bool get_oem_charge_done_status(void)
{
#ifdef	CONFIG_OP_DEBUG_CHG
	return false;
#else
	if (g_chg->is_aging_test)
		return false;
#endif
	if (g_chg)
		return (g_chg->chg_done || g_chg->recharge_status);
	else
		return false;
}

void op_handle_usb_removal(struct smb_charger *chg)
{
	op_set_fast_chg_allow(chg, false);
	set_prop_fast_switch_to_normal_false(chg);
	set_usb_switch(chg, false);
	set_dash_charger_present(false);
	op_dash_enable_role_switch(chg);

	chg->chg_ovp = false;
	chg->dash_on = false;
	chg->chg_done = false;
	chg->time_out = false;
	chg->recharge_status = false;
	chg->usb_enum_status = false;
	chg->non_std_chg_present = false;
	chg->usb_type_redet_done = false;
	chg->boot_usb_present = false;
	chg->revert_boost_trigger = false;
	chg->non_stand_chg_current = 0;
	chg->non_stand_chg_count = 0;
	chg->redet_count = 0;
	chg->dump_count = 0;
	chg->op_apsd_done = 0;
	chg->ck_dash_count = 0;
	chg->re_trigr_dash_done = 0;
	chg->dash_handshake_failed = false;
	chg->recovery_boost_count = 0;
	op_battery_temp_region_set(chg, BATT_TEMP_INVALID);
}

static void dash_to_normal_workaround(struct smb_charger *chg)
{
	u8 cfg_mask;

	/* Return if we're just about to start dash */
	if (op_get_fast_chg_allow(chg))
		return;

	/* Return if we're still dash charging */
	if (is_fastchg_allowed(chg))
		return;

	pr_info("applying dash to normal workaround");

	/* Reset USBIN collapse and rerun AICL */
	cfg_mask = SUSPEND_ON_COLLAPSE_USBIN_BIT
			| USBIN_HV_COLLAPSE_RESPONSE_BIT
			| USBIN_LV_COLLAPSE_RESPONSE_BIT;
	smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
			cfg_mask, 0);

	smblib_rerun_apsd(chg);

	smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
			USBIN_AICL_RERUN_EN_BIT, USBIN_AICL_RERUN_EN_BIT);

	smblib_rerun_aicl(chg);

	smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
			cfg_mask, cfg_mask);
}

int update_dash_unplug_status(void)
{
	int rc;
	union power_supply_propval vbus_val;

	dash_to_normal_workaround(g_chg);

	rc = smblib_get_prop_usb_voltage_now(g_chg, &vbus_val);
	if (rc < 0)
		pr_err("failed to read usb_voltage rc=%d\n", rc);
	else if (vbus_val.intval / 1000 <= 2500) {
		op_handle_usb_plugin(g_chg);
		smblib_update_usb_type(g_chg);
		power_supply_changed(g_chg->usb_psy);
	}

	return 0;
}

int op_set_collapse_fet(struct smb_charger *chg, bool on)
{
	int rc = 0;
	u8 stat;

	rc = smblib_masked_write(chg, USBIN_5V_AICL_THRESHOLD_CFG_REG,
		BIT(0) | BIT(1), on ? 0 : BIT(0) | BIT(1));
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_5V_AICL_THRESHOLD_CFG_REG, rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_5V_AICL_THRESHOLD_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_5V_AICL_THRESHOLD_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_5V_AICL_THRESHOLD_CFG_REG(0x%x)=0x%x\n",
			USBIN_5V_AICL_THRESHOLD_CFG_REG, stat);

	rc = smblib_masked_write(chg, USBIN_CONT_AICL_THRESHOLD_CFG_REG,
		BIT(0) | BIT(1), on ? 0 : BIT(0) | BIT(1));
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_CONT_AICL_THRESHOLD_CFG_REG,
			rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_CONT_AICL_THRESHOLD_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_CONT_AICL_THRESHOLD_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_CONT_AICL_THRESHOLD_CFG_REG(0x%x)=0x%x\n",
			USBIN_CONT_AICL_THRESHOLD_CFG_REG, stat);

	rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
					SUSPEND_ON_COLLAPSE_USBIN_BIT
					| USBIN_HV_COLLAPSE_RESPONSE_BIT
					| USBIN_LV_COLLAPSE_RESPONSE_BIT
					| USBIN_AICL_RERUN_EN_BIT,
					on ? 0 : SUSPEND_ON_COLLAPSE_USBIN_BIT
					| USBIN_HV_COLLAPSE_RESPONSE_BIT
					| USBIN_LV_COLLAPSE_RESPONSE_BIT);
	if (rc < 0) {
		smblib_err(chg,
			"Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_AICL_OPTIONS_CFG_REG, rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_AICL_OPTIONS_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_AICL_OPTIONS_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_AICL_OPTIONS_CFG_REG(0x%x)=0x%x\n",
		USBIN_AICL_OPTIONS_CFG_REG, stat);

	rc = smblib_masked_write(chg, USBIN_LOAD_CFG_REG, BIT(0)
			| BIT(1), on ? 0 : BIT(0) | BIT(1));
	if (rc < 0) {
		smblib_err(chg, "Couldn't write %s to 0x%x rc=%d\n",
			on ? "on" : "off", USBIN_LOAD_CFG_REG, rc);
		return rc;
	}

	rc = smblib_read(chg, USBIN_LOAD_CFG_REG, &stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read 0x%x rc=%d\n",
			USBIN_LOAD_CFG_REG, rc);
		return rc;
	}
	pr_info("USBIN_LOAD_CFG_REG(0x%x)=0x%x\n",
			USBIN_LOAD_CFG_REG, stat);

	return rc;
}

int op_handle_switcher_power_ok(void)
{
	int rc;
	u8 stat;
	union power_supply_propval vbus_val;

	if (!g_chg)
		return 0;
	if (!(g_chg->wa_flags & BOOST_BACK_WA))
		return 0;
	rc = smblib_read(g_chg, POWER_PATH_STATUS_REG, &stat);
	if (rc < 0) {
		smblib_err(g_chg,
			"Couldn't read POWER_PATH_STATUS rc=%d\n", rc);
		return 0;
	}
	smblib_err(g_chg, "POWER_PATH_STATUS stat=0x%x\n", stat);

	if ((stat & USE_USBIN_BIT) &&
		get_effective_result(g_chg->usb_icl_votable)
		< USBIN_25MA)
		return 0;

	if (stat & USE_DCIN_BIT)
		return 0;
	usleep_range(50000, 50002);
	rc = smblib_get_prop_usb_voltage_now(g_chg, &vbus_val);
	if (rc < 0) {
		pr_err("fail to read usb_voltage rc=%d\n", rc);
	} else if (vbus_val.intval / 1000 >= 2500) {
		pr_err("vbus_val.intval=%d\n", vbus_val.intval);
		vote(g_chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
		schedule_delayed_work(&g_chg->recovery_suspend_work,
				msecs_to_jiffies(TIME_100MS));
		smblib_err(g_chg, "OP Reverse boost detected\n");
	}

	return 0;
}

int op_contrl(int enable, bool check_power_ok)
{
	pr_info("%s, en=%d\n", __func__, enable);
	if (!g_chg)
		return 0;
	if (enable) {
		if (check_power_ok)
			op_handle_switcher_power_ok();
	} else {
		op_set_collapse_fet(g_chg, enable);
	}

	return 0;
}

bool get_prop_fast_chg_started(struct smb_charger *chg)
{
	if (get_prop_fast_adapter_update(chg)
		== ADAPTER_FW_NEED_UPDATE)
	return true;
	if (fast_charger && fast_charger->fast_chg_started)
		return fast_charger->fast_chg_started();
	pr_err("no fast_charger register found\n");
	return false;
}

int get_prop_fast_adapter_update(struct smb_charger *chg)
{
	int update_status;

	if (fast_charger && fast_charger->get_adapter_update)
		update_status = fast_charger->get_adapter_update();
	else {
		pr_err("no fast_charger register found\n");
		update_status = ADAPTER_FW_UPDATE_NONE;
	}
	return update_status;
}

static bool set_prop_fast_switch_to_normal_false(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->set_switch_to_noraml_false)
		return fast_charger->set_switch_to_noraml_false();
	pr_err("no fast_charger register found\n");
	return false;
}

bool op_get_fastchg_ing(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fast_chg_ing)
		return fast_charger->get_fast_chg_ing();
	pr_err("no fast_charger register found\n");
	return false;
}

bool op_set_fast_chg_allow(struct smb_charger *chg, bool enable)
{
	if (fast_charger && fast_charger->set_fast_chg_allow)
		return fast_charger->set_fast_chg_allow(enable);
	pr_err("no fast_charger register found\n");
	return false;
}

static bool op_get_fast_chg_allow(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->get_fast_chg_allow)
		return fast_charger->get_fast_chg_allow();
	pr_err("no fast_charger register found\n");
	return false;
}

static bool op_is_usb_switch_on(struct smb_charger *chg)
{
	if (fast_charger && fast_charger->is_usb_switch_on)
		return fast_charger->is_usb_switch_on();

	pr_err("no fast_charger register found\n");
	return false;
}

static enum batt_status_type op_battery_status_get(struct smb_charger *chg)
{
	return chg->battery_status;
}

enum temp_region_type op_battery_temp_region_get(struct smb_charger *chg)
{
	return chg->mBattTempRegion;
}

int fuelgauge_battery_temp_region_get(void)
{
	if (!g_chg)
		return BATT_TEMP_NORMAL;

	return op_battery_temp_region_get(g_chg);
}

static void op_battery_status_set(struct smb_charger *chg,
		enum batt_status_type battery_status)
{
	chg->battery_status = battery_status;
}

static void op_battery_temp_region_set(struct smb_charger *chg,
		enum temp_region_type batt_temp_region)
{
	chg->mBattTempRegion = batt_temp_region;
	pr_err("set temp_region=%d\n", chg->mBattTempRegion);
}

static int op_dash_disable_role_switch(struct smb_charger *chg, bool disable)
{
	int rc;

	if (!chg->disable_power_role_switch)
		return -ENODEV;

	rc = vote(chg->disable_power_role_switch, DASH_VOTER, disable, 0);
	if (rc)
		smblib_err(chg, "Couldn't %s power role switch rc=%d\n",
				disable ? "disable" : "enable", rc);

	return rc;
}

static bool op_dash_is_sink_only(struct smb_charger *chg)
{
	u8 ctrl;
	int rc;

	rc = smblib_read(chg, TYPE_C_INTRPT_ENB_SOFTWARE_CTRL_REG, &ctrl);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read SOFTWARE_CTRL_REG rc=%d\n", rc);
		return false;
	}

	return (ctrl & TYPEC_POWER_ROLE_CMD_MASK) == UFP_EN_CMD_BIT;
}

static void op_dash_enable_role_switch(struct smb_charger *chg)
{
	int rc;

	if (!chg->disable_power_role_switch)
		return;

	/* vote() records even a failed write; verify, then re-elect. */
	op_dash_disable_role_switch(chg, false);

	if (get_effective_result(chg->disable_power_role_switch))
		return;

	if (!op_dash_is_sink_only(chg))
		return;

	smblib_err(chg, "DASH sink-only release not applied, re-electing\n");
	rc = rerun_election(chg->disable_power_role_switch);
	if (rc)
		smblib_err(chg, "Couldn't re-elect power role switch rc=%d\n", rc);
}
static void set_usb_switch(struct smb_charger *chg, bool enable)
{
	int retrger_time;

	if (!fast_charger) {
		pr_err("no fast_charger register found\n");
		return;
	}

	if (enable) {
		pr_err("switch on fastchg\n");
		if (chg->boot_usb_present && chg->re_trigr_dash_done) {
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER,
					true, 0);
			usleep_range(500000, 510000);
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER,
					true, DEFAULT_DCP_MA*1000);
		}
		set_mcu_en_gpio_value(1);
		usleep_range(10000, 10002);
		usb_sw_gpio_set(1);
		usleep_range(10000, 10002);
		mcu_en_gpio_set(0);
		if (chg->boot_usb_present)
			retrger_time = TIME_3S;
		else
			retrger_time = TIME_200MS;
		if (!chg->re_trigr_dash_done)
			schedule_delayed_work(&chg->rechk_sw_dsh_work,
					msecs_to_jiffies(retrger_time));
	} else {
		pr_err("switch off fastchg\n");
		usb_sw_gpio_set(0);
		mcu_en_gpio_set(1);
	}
}

static void switch_fast_chg(struct smb_charger *chg)
{
	bool fastchg_allowed, is_allowed;
	static bool pre_fastchg_allowed, pre_is_allowed;
	int rc;

	mutex_lock(&chg->sw_dash_lock);
	if (op_is_usb_switch_on(chg)) {
		mutex_unlock(&chg->sw_dash_lock);
		return;
	}
	if (!is_usb_present(chg)) {
		mutex_unlock(&chg->sw_dash_lock);
		return;
	}

	fastchg_allowed = op_get_fast_chg_allow(chg);
	if (pre_fastchg_allowed != fastchg_allowed) {
		pre_fastchg_allowed = fastchg_allowed;
		pr_info("fastchg_allowed = %d\n", fastchg_allowed);
	}
	if (!fastchg_allowed) {
		is_allowed = is_fastchg_allowed(chg);
		if (pre_is_allowed != is_allowed) {
			pre_is_allowed = is_allowed;
			pr_info("is_allowed = %d\n", is_allowed);
		}
		if (is_allowed) {
			rc = op_dash_disable_role_switch(chg, true);
			if (!rc && !op_dash_is_sink_only(chg)) {
				smblib_err(chg, "DASH sink-only not applied\n");
				rc = -EINVAL;
			}
			if (rc) {
				op_dash_enable_role_switch(chg);
			} else {
				set_usb_switch(chg, true);
				op_set_fast_chg_allow(chg, true);
			}
		}
	}
	mutex_unlock(&chg->sw_dash_lock);
}

static void op_re_kick_allowed_voltage(struct smb_charger  *chg)
{
	const struct apsd_result *apsd_result;

	if (!is_usb_present(chg))
		return;

	apsd_result = smblib_get_apsd_result(chg);
	if (apsd_result->bit == SDP_CHARGER_BIT)
		return;

	pr_info("re-kick allowed voltage\n");
	smblib_set_usb_pd_allowed_voltage(chg, MICRO_9V, MICRO_9V);
	msleep(500);
	smblib_set_usb_pd_allowed_voltage(chg, MICRO_5V, MICRO_5V);
}

static void op_re_kick_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			re_kick_work);

	if (chg->vbus_present) {
		op_re_kick_allowed_voltage(chg);
		schedule_delayed_work(&chg->check_switch_dash_work,
				msecs_to_jiffies(500));
	}
}

static void op_release_dash_path(struct smb_charger *chg)
{
	op_set_fast_chg_allow(chg, false);
	set_usb_switch(chg, false);
	op_dash_enable_role_switch(chg);
	chg->ck_dash_count = 0;
}

static void retrigger_dash_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			rechk_sw_dsh_work.work);
	mutex_lock(&chg->sw_dash_lock);
	pr_debug("chg->ck_dash_count=%d\n", chg->ck_dash_count);
	if (chg->usb_enum_status)
		goto out;
	if (chg->dash_present || !chg->vbus_present) {
		chg->ck_dash_count = 0;
		goto out;
	}
	if (chg->re_trigr_dash_done
			&& chg->ck_dash_count >= DASH_GIVEUP_COUNT) {
		chg->ck_dash_count = 0;
		/* Unplug holds chg->lock, which nests inside sw_dash_lock. */
		mutex_lock(&chg->lock);
		if (chg->vbus_present) {
			pr_info("dash handshake failed, restoring the port\n");
			op_release_dash_path(chg);
			chg->dash_handshake_failed = true;
		}
		mutex_unlock(&chg->lock);
		goto out;
	}
	if (!chg->re_trigr_dash_done
			&& chg->ck_dash_count >= DASH_CHECK_COUNT) {
		chg->ck_dash_count = 0;
		pr_info("retrger dash\n");
		chg->re_trigr_dash_done = true;
		set_usb_switch(chg, false);
		set_usb_switch(chg, true);
		schedule_delayed_work(&chg->rechk_sw_dsh_work,
				msecs_to_jiffies(TIME_200MS));
	} else {
		chg->ck_dash_count++;
		schedule_delayed_work(&chg->rechk_sw_dsh_work,
				msecs_to_jiffies(TIME_200MS));
	}
out:
	mutex_unlock(&chg->sw_dash_lock);
}

static void op_chek_apsd_done_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			op_check_apsd_work.work);
	union power_supply_propval vbus_val;
	int rc;
	const struct apsd_result *apsd_result;

	pr_debug("chg->ck_apsd_count=%d\n", chg->ck_apsd_count);
	if (chg->usb_enum_status || chg->op_apsd_done) {
		chg->ck_apsd_count = 0;
		return;
	}
	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		chg->ck_apsd_count = 0;
		pr_info("failed to read usb_voltage rc=%d\n", rc);
		return;
	}
	if (vbus_val.intval / 1000 < 2500) {
		pr_info("vbus less 2.5v\n");
		chg->ck_apsd_count = 0;
		return;
	}
	apsd_result = smblib_get_apsd_result(chg);
	if (apsd_result->bit) {
		chg->ck_apsd_count = 0;
		return;
	}

	if (chg->ck_apsd_count >= APSD_CHECK_COUTNT) {
		pr_info("apsd done error\n");
		chg->ck_apsd_count = 0;
		op_dump_regs(chg);
		op_rerun_apsd(chg);
	} else {
		chg->ck_apsd_count++;
		schedule_delayed_work(&chg->op_check_apsd_work,
				msecs_to_jiffies(TIME_1000MS));
	}
}

bool op_dash_active(struct smb_charger *chg)
{
	chg->dash_on = get_prop_fast_chg_started(chg);

	return chg->dash_on;
}

bool op_pd_disabled(struct smb_charger *chg)
{
	return chg->pd_disabled;
}

void op_charge_done_check(struct smb_charger *chg, int stat)
{
	if (stat == TERMINATE_CHARGE) {
		/* charge done, disable charging in software too */
		chg->chg_done = true;
		op_charging_en(chg, false);
	}
}

bool op_switcher_power_ok_skip(struct smb_charger *chg, const char *irq_name)
{
	smblib_dbg(chg, PR_INTERRUPT, "IRQ: %s\n", irq_name);
	chg->dash_on = get_prop_fast_chg_started(chg);

	return chg->dash_on;
}

bool op_reverse_boost_bounce(struct smb_charger *chg)
{
	union power_supply_propval vbus_val;
	int rc;

	smblib_err(chg, "Reverse boost detected\n");
	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0)
		pr_err("fail to read usb_voltage rc=%d\n", rc);
	else if (vbus_val.intval / 1000 >= 2500)
		pr_err("vbus_val.intval=%d\n", vbus_val.intval);
	chg->revert_boost_trigger = true;
	vote(chg->usb_icl_votable, BOOST_BACK_VOTER, true, 0);
	schedule_delayed_work(&chg->recovery_suspend_work,
			msecs_to_jiffies(TIME_100MS));

	return true;
}

void op_boot_usb_present_init(struct smb_charger *chg, bool usb_present)
{
	if (usb_present) {
		schedule_delayed_work(&chg->non_standard_charger_check_work,
				msecs_to_jiffies(TIME_1000MS));
		chg->boot_usb_present = true;
	} else if (chg->vbus_present) {
		op_handle_usb_plugin(chg);
	}
}

static void op_recovery_usb_suspend_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			recovery_suspend_work.work);
	int effect_result;

	if (chg->recovery_boost_count >= BOOST_BACK_COUNT) {
		pr_info("recovery revert boost\n");
		vote(chg->usb_icl_votable, BOOST_BACK_VOTER, false, 0);
		if (is_usb_present(chg)) {
			effect_result =
			get_effective_result(chg->usb_icl_votable);
			pr_info("effect_result=%d\n", effect_result);
			if (effect_result > DEFAULT_AGAING_CHG_MA*1000)
				vote(chg->usb_icl_votable, DCP_VOTER,
					true, (effect_result - USBIN_150MA));
		}
		msleep(1000);
		chg->revert_boost_trigger = false;
	} else {
		chg->recovery_boost_count++;
		schedule_delayed_work(&chg->recovery_suspend_work,
			msecs_to_jiffies(TIME_100MS));
	}
}

static void op_check_allow_switch_dash_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
			struct smb_charger, check_switch_dash_work);
	const struct apsd_result *apsd_result;

	if (!is_usb_present(chg))
		return;
	if (chg->usb_enum_status)
		return;

	apsd_result = smblib_get_apsd_result(chg);
	if (((apsd_result->bit != SDP_CHARGER_BIT
		&& apsd_result->bit != CDP_CHARGER_BIT)
		&& apsd_result->bit)
		|| chg->non_std_chg_present)
		switch_fast_chg(chg);
}

int check_allow_switch_dash(struct smb_charger *chg,
				const union power_supply_propval *val)
{
	if (val->intval < 0)
		return -EINVAL;

	schedule_delayed_work(&chg->check_switch_dash_work,
				msecs_to_jiffies(500));
	return 0;
}

static int set_dash_charger_present(int status)
{
	int charger_present;
	static bool pre_dash_present;

	if (g_chg) {
		pre_dash_present = g_chg->dash_present;
		charger_present = is_usb_present(g_chg);
		if (status && charger_present)
			mutex_lock(&g_chg->sw_dash_lock);
		g_chg->dash_present = status && charger_present;
		if (g_chg->dash_present && !pre_dash_present) {
			pr_err("set dash online\n");
			g_chg->dash_handshake_failed = false;
			g_chg->re_trigr_dash_done = false;
			g_chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
			vote(g_chg->usb_icl_votable, PD_VOTER, true,
					DEFAULT_WALL_CHG_MA * 1000);
		}
		if (status && charger_present)
			mutex_unlock(&g_chg->sw_dash_lock);
		power_supply_changed(g_chg->batt_psy);
		pr_info("dash_present = %d, charger_present = %d\n",
				g_chg->dash_present, charger_present);
	} else {
		pr_err("set_dash_charger_present error\n");
	}

	return 0;
}

static void op_check_charge_timeout(struct smb_charger *chg)
{
	static int batt_status, count;

	if (chg->chg_done || chg->is_aging_test)
		return;

	batt_status = get_prop_batt_status(chg);
	if (chg->vbus_present
			&& batt_status == POWER_SUPPLY_STATUS_CHARGING)
		count++;
	else
		count = 0;

	if (count > CHG_TIMEOUT_COUNT) {
		pr_err("chg timeout! stop chaging now\n");
		op_charging_en(chg, false);
		chg->time_out = true;
	}
}

static int get_prop_batt_present(struct smb_charger *chg)
{
	int rc;
	u8 stat;

	rc = smblib_read(chg, BATIF_BASE + INT_RT_STS_OFFSET, &stat);
	if (rc < 0) {
		pr_err("Couldn't read BATIF_INT_RT_STS rc=%d\n", rc);
		return rc;
	}

	return !(stat & (BAT_THERM_OR_ID_MISSING_RT_STS_BIT
					| BAT_TERMINAL_MISSING_RT_STS_BIT));
}

int get_prop_batt_capacity(struct smb_charger *chg)
{
	int capacity, rc;

	if (chg->fake_capacity >= 0)
		return chg->fake_capacity;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_CAPACITY, &capacity);
	if (rc) {
		pr_err("Couldn't get capacity rc=%d\n", rc);
		capacity = DEFAULT_BATT_CAPACITY;
	}

	return capacity;
}

int get_prop_batt_temp(struct smb_charger *chg)
{
	int temp, rc;

	if (chg->use_fake_temp)
		return chg->fake_temp;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_TEMP, &temp);
	if (rc) {
		pr_err("Couldn't get temperature rc=%d\n", rc);
		temp = DEFAULT_BATT_TEMP;
	}

	return temp;
}

int get_prop_batt_current_now(struct smb_charger *chg)
{
	int ua, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_CURRENT_NOW, &ua);
	if (rc) {
		pr_err("Couldn't get current rc=%d\n", rc);
		ua = DEFAULT_BATT_CURRENT_NOW;
	}

	return ua;
}

int get_prop_batt_voltage_now(struct smb_charger *chg)
{
	int uv, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_VOLTAGE_NOW, &uv);
	if (rc) {
		pr_err("Couldn't get voltage rc=%d\n", rc);
		uv = DEFAULT_BATT_VOLTAGE_NOW;
	}

	return uv;
}

int get_prop_fg_capacity(struct smb_charger *chg)
{
	int capacity, rc;

	if (chg->fake_capacity >= 0)
		return chg->fake_capacity;

	rc = get_property_from_fg(chg,
		POWER_SUPPLY_PROP_FG_CAPACITY, &capacity);
	if (rc) {
		pr_err("Couldn't get capacity rc=%d\n", rc);
		capacity = DEFAULT_BATT_CAPACITY;
	}

	return capacity;
}

int get_prop_fg_current_now(struct smb_charger *chg)
{
	int ua, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_FG_CURRENT_NOW, &ua);
	if (rc) {
		pr_err("Couldn't get current rc=%d\n", rc);
		ua = DEFAULT_BATT_CURRENT_NOW;
	}

	return ua;
}

int get_prop_fg_voltage_now(struct smb_charger *chg)
{
	int uv, rc;

	rc = get_property_from_fg(chg, POWER_SUPPLY_PROP_FG_VOLTAGE_NOW, &uv);
	if (rc) {
		pr_err("Couldn't get voltage rc=%d\n", rc);
		uv = DEFAULT_BATT_VOLTAGE_NOW;
	}

	return uv;
}

int get_prop_batt_status(struct smb_charger *chg)
{
	int capacity, batt_status, rc;
	enum temp_region_type temp_region;
	union power_supply_propval pval = {0, };

	temp_region = op_battery_temp_region_get(chg);
	capacity = get_prop_batt_capacity(chg);
	chg->dash_on = get_prop_fast_chg_started(chg);
	if ((chg->chg_done || chg->recharge_status)
			&& (temp_region == BATT_TEMP_COOL
			|| temp_region == BATT_TEMP_LITTLE_COOL
			|| temp_region == BATT_TEMP_PRE_NORMAL
			|| temp_region == BATT_TEMP_NORMAL)
			&& capacity > 90) {
		return POWER_SUPPLY_STATUS_FULL;
	} else if (chg->dash_on) {
		return POWER_SUPPLY_STATUS_CHARGING;
	}
	if (chg->revert_boost_trigger && chg->vbus_present)
		return POWER_SUPPLY_STATUS_CHARGING;

	rc = smblib_get_prop_batt_status(chg, &pval);
	if (rc)
		batt_status = 0;
	else
		batt_status = pval.intval;

	return batt_status;
}

int get_charging_status(void)
{
	int rc;
	union power_supply_propval pval = {0, };

	if (!g_chg)
		return POWER_SUPPLY_STATUS_DISCHARGING;

	rc = smblib_get_prop_batt_status(g_chg, &pval);
	if (rc)
		return POWER_SUPPLY_STATUS_UNKNOWN;

	return pval.intval;
}

void set_chg_ibat_vbat_max(
	struct smb_charger *chg, int ibat, int vfloat)
{
	pr_err("set ibatmax=%d and set vbatmax=%d\n",
			ibat, vfloat);

	vote(chg->fcc_votable,
		DEFAULT_VOTER, true, ibat * 1000);
	vote(chg->fv_votable,
		DEFAULT_VOTER, true, vfloat * 1000);

	/* set cc to cv 100mv lower than vfloat */
	set_property_on_fg(chg, POWER_SUPPLY_PROP_CC_TO_CV_POINT, vfloat - 100);
}

static int handle_batt_temp_cold(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_COLD || chg->is_power_changed) {
		pr_err("triggered\n");
		chg->is_power_changed = false;

		op_charging_en(chg, false);
		op_battery_temp_region_set(chg, BATT_TEMP_COLD);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0 + BATT_TEMP_HYST;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}

	return 0;
}

static int handle_batt_temp_little_cold(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_LITTLE_COLD
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);

		set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_LITTLE_COLD],
				chg->vbatmax[BATT_TEMP_LITTLE_COLD]);
		op_battery_temp_region_set(chg, BATT_TEMP_LITTLE_COLD);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1 + BATT_TEMP_HYST;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}

	return 0;
}

static int handle_batt_temp_cool(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_COOL
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);

		set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_COOL],
				chg->vbatmax[BATT_TEMP_COOL]);
		op_battery_temp_region_set(chg, BATT_TEMP_COOL);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2 + BATT_TEMP_HYST;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}

	return 0;
}

static int handle_batt_temp_little_cool(struct smb_charger *chg)
{
	int temp_region, vbat_mv;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_LITTLE_COOL
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);

		vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
		if (vbat_mv > 4180) {
			set_chg_ibat_vbat_max(chg, 450,
					chg->vbatmax[BATT_TEMP_LITTLE_COOL]);
			chg->temp_littel_cool_set_current_0_point_25c = false;
		} else {
			set_chg_ibat_vbat_max(chg,
					chg->ibatmax[BATT_TEMP_LITTLE_COOL],
					chg->vbatmax[BATT_TEMP_LITTLE_COOL]);
			chg->temp_littel_cool_set_current_0_point_25c = true;
		}
		op_battery_temp_region_set(chg, BATT_TEMP_LITTLE_COOL);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3 + BATT_TEMP_HYST;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}

	return 0;
}

static int handle_batt_temp_prenormal(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if (temp_region != BATT_TEMP_PRE_NORMAL
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);

		set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_PRE_NORMAL],
				chg->vbatmax[BATT_TEMP_PRE_NORMAL]);
		op_battery_temp_region_set(chg, BATT_TEMP_PRE_NORMAL);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4 + BATT_TEMP_HYST;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}

	return 0;
}

static int handle_batt_temp_normal(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if ((temp_region != BATT_TEMP_NORMAL)
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->recharge_pending = false;
		chg->is_power_changed = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);

		set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_NORMAL],
				chg->vbatmax[BATT_TEMP_NORMAL]);
		op_battery_temp_region_set(chg, BATT_TEMP_NORMAL);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}

	return 0;
}

static int handle_batt_temp_warm(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	if (chg->chg_ovp)
		return 0;

	temp_region = op_battery_temp_region_get(chg);
	if ((temp_region != BATT_TEMP_WARM)
			|| chg->is_power_changed || chg->recharge_pending) {
		pr_err("triggered\n");
		chg->is_power_changed = false;
		chg->recharge_pending = false;

		if (temp_region == BATT_TEMP_HOT ||
				temp_region == BATT_TEMP_COLD)
			op_charging_en(chg, true);

		set_chg_ibat_vbat_max(chg,
				chg->ibatmax[BATT_TEMP_WARM],
				chg->vbatmax[BATT_TEMP_WARM]);
		op_battery_temp_region_set(chg, BATT_TEMP_WARM);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5 - BATT_TEMP_HYST;
		chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	}

	return 0;
}

static int handle_batt_temp_hot(struct smb_charger *chg)
{
	enum temp_region_type temp_region;

	temp_region = op_battery_temp_region_get(chg);
	if ((temp_region != BATT_TEMP_HOT)
			|| chg->is_power_changed) {
		pr_err("triggered\n");
		chg->is_power_changed = false;
		op_charging_en(chg, false);
		op_battery_temp_region_set(chg, BATT_TEMP_HOT);

		/* Update the temperature boundaries */
		chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
		chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
		chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
		chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
		chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
		chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
		chg->mBattTempBoundT6 =
			chg->BATT_TEMP_T6 - BATT_TEMP_HYST;
		/* from hot to warm */
	}

	return 0;
}

int op_check_battery_temp(struct smb_charger *chg)
{
	int temp, rc = -1;

	if (!chg->vbus_present)
		return rc;

	temp = get_prop_batt_temp(chg);
	if (temp < chg->mBattTempBoundT0) /* COLD */
		rc = handle_batt_temp_cold(chg);
	else if (temp >=  chg->mBattTempBoundT0 &&
			temp < chg->mBattTempBoundT1) /* LITTLE_COLD */
		rc = handle_batt_temp_little_cold(chg);
	else if (temp >=  chg->mBattTempBoundT1 &&
			temp < chg->mBattTempBoundT2) /* COOL */
		rc = handle_batt_temp_cool(chg);
	else if (temp >= chg->mBattTempBoundT2 &&
			temp < chg->mBattTempBoundT3) /* LITTLE_COOL */
		rc = handle_batt_temp_little_cool(chg);
	else if (temp >= chg->mBattTempBoundT3 &&
			temp < chg->mBattTempBoundT4) /* PRE_NORMAL */
		rc = handle_batt_temp_prenormal(chg);
	else if (temp >= chg->mBattTempBoundT4 &&
			temp < chg->mBattTempBoundT5) /* NORMAL */
		rc = handle_batt_temp_normal(chg);
	else if (temp >= chg->mBattTempBoundT5 &&
			temp <=  chg->mBattTempBoundT6) /* WARM */
		rc = handle_batt_temp_warm(chg);
	else if (temp > chg->mBattTempBoundT6) /* HOT */
		rc = handle_batt_temp_hot(chg);

	return rc;
}

void op_charge_info_init(struct smb_charger *chg)
{
	op_battery_temp_region_set(chg, BATT_TEMP_NORMAL);

	chg->mBattTempBoundT0 = chg->BATT_TEMP_T0;
	chg->mBattTempBoundT1 = chg->BATT_TEMP_T1;
	chg->mBattTempBoundT2 = chg->BATT_TEMP_T2;
	chg->mBattTempBoundT3 = chg->BATT_TEMP_T3;
	chg->mBattTempBoundT4 = chg->BATT_TEMP_T4;
	chg->mBattTempBoundT5 = chg->BATT_TEMP_T5;
	chg->mBattTempBoundT6 = chg->BATT_TEMP_T6;
	chg->chg_ovp = false;
	chg->is_power_changed = false;
	chg->chg_done = false;
	chg->recharge_pending = false;
	chg->recharge_status = false;
	chg->temp_littel_cool_set_current_0_point_25c = false;
	chg->oem_lcd_is_on = false;
	chg->time_out = false;
	chg->battery_status = BATT_STATUS_GOOD;
	chg->disable_normal_chg_for_dash = false;
	chg->usb_enum_status = false;
	chg->non_std_chg_present = false;
}

static int op_handle_battery_uovp(struct smb_charger *chg)
{
	pr_err("vbat is over voltage, stop charging\n");
	op_charging_en(chg, false);

	return 0;
}

static int op_handle_battery_restore_from_uovp(struct smb_charger *chg)
{
	pr_err("vbat is back to normal, start charging\n");
	/* restore charging form battery ovp */
	op_charging_en(chg, true);

	return 0;
}

static void op_check_battery_uovp(struct smb_charger *chg)
{
	int vbat_mv = 0;
	enum batt_status_type battery_status_pre;

	if (!chg->vbus_present)
		return;

	battery_status_pre = op_battery_status_get(chg);
	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	pr_debug("bat vol:%d\n", vbat_mv);
	if (vbat_mv > BATT_SOFT_OVP_MV) {
		if (battery_status_pre == BATT_STATUS_GOOD) {
			pr_err("BATTERY_SOFT_OVP_VOLTAGE\n");
			op_battery_status_set(chg, BATT_STATUS_BAD);
			op_handle_battery_uovp(chg);
		}
	} else {
		if (battery_status_pre == BATT_STATUS_BAD) {
			pr_err("battery_restore_from_uovp\n");
			op_battery_status_set(chg, BATT_STATUS_GOOD);
			op_handle_battery_restore_from_uovp(chg);
		}
	}

}

int op_get_charg_en(struct smb_charger *chg, int *chg_enabled)
{
	int rc = 0;
	u8 temp;

	rc = smblib_read(chg, CHARGING_ENABLE_CMD_REG, &temp);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read chg en rc=%d\n", rc);
		return rc;
	}
	*chg_enabled = temp & CHARGING_ENABLE_CMD_BIT;

	return rc;
}

static void op_check_charger_uovp(struct smb_charger *chg, int vchg_mv)
{
	static int over_volt_count, not_over_volt_count;
	static bool uovp_satus, pre_uovp_satus;
	int detect_time = 3; /* 3 x 6s = 18s */

	if (!chg->vbus_present)
		return;

	pr_debug("charger_voltage=%d charger_ovp=%d\n",
		vchg_mv, chg->chg_ovp);

	if (!chg->chg_ovp) {
		if (vchg_mv > CHG_SOFT_OVP_MV ||
				vchg_mv <= CHG_SOFT_UVP_MV) {
			pr_err("charger is over voltage, count=%d\n",
				over_volt_count);
			uovp_satus = true;
			if (pre_uovp_satus)
				over_volt_count++;
			else
				over_volt_count = 0;

			if (detect_time <= over_volt_count) {
				op_charging_en(chg, false);
				chg->chg_ovp = true;
			}
		}
	} else {
		if (vchg_mv < CHG_SOFT_OVP_MV - 100 &&
				vchg_mv > CHG_SOFT_UVP_MV + 100) {
			uovp_satus = false;
			if (!pre_uovp_satus)
				not_over_volt_count++;
			else
				not_over_volt_count = 0;

			if (detect_time <= not_over_volt_count) {
				op_charging_en(chg, true);
				chg->chg_ovp = false;
				op_check_battery_temp(chg);
				smblib_rerun_aicl(chg);
			}
		}
	}

	pre_uovp_satus = uovp_satus;
}

static void op_check_charger_collapse(struct smb_charger *chg)
{
	int rc, is_usb_supend, curr, chg_en;
	u8 stat, chger_stat, pwer_source_stats;

	if (!chg->vbus_present)
		return;
	if (chg->dash_present)
		return;
	rc = smblib_read(chg, BATTERY_CHARGER_STATUS_1_REG, &chger_stat);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read POWER_PATH_STATUS rc=%d\n",
			rc);
	}
	rc = smblib_read(chg, POWER_PATH_STATUS_REG, &pwer_source_stats);
	if (rc < 0) {
		smblib_err(chg, "Couldn't read AICL_STATUS_REG rc=%d\n",
			rc);
	}
	smblib_get_usb_suspend(chg, &is_usb_supend);
	op_get_charg_en(chg, &chg_en);
	pr_debug("chger_stat=0x%x, aicl_stats =0x%x, chg_en =%d\n",
		chger_stat, pwer_source_stats, chg_en);
	curr = get_prop_batt_current_now(chg) / 1000;
	stat = !chg->chg_done
			&& !is_usb_supend
			&& (curr > 20)
			&& chg_en
			&& ((CC_SOFT_TERMINATE_BIT & chger_stat)
				|| (pwer_source_stats == 0x72));

	if (stat && !chg->charger_collpse) {
		rc = smblib_masked_write(chg, USBIN_AICL_OPTIONS_CFG_REG,
			SUSPEND_ON_COLLAPSE_USBIN_BIT
			| USBIN_AICL_START_AT_MAX_BIT
			| USBIN_AICL_ADC_EN_BIT
			| USBIN_AICL_RERUN_EN_BIT, USBIN_AICL_RERUN_EN_BIT);
			if (rc < 0)
				dev_err(chg->dev,
				"Couldn't configure AICL rc=%d\n", rc);
		smblib_rerun_aicl(chg);
		chg->charger_collpse = true;
		schedule_delayed_work(&chg->op_re_set_work,
				msecs_to_jiffies(TIME_1000MS));
		smblib_err(chg, "op_check_charger_collapse done\n");
	}
}

static int fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	int *blank;
	struct smb_charger *chip =
		container_of(self, struct smb_charger, fb_notif);

	if (evdata && evdata->data && chip) {
		if (event == FB_EVENT_BLANK) {
			blank = evdata->data;
			if (*blank == FB_BLANK_UNBLANK) {
				if (!chip->oem_lcd_is_on)
					set_property_on_fg(chip,
						POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF, 0);
				chip->oem_lcd_is_on = true;
			} else if (*blank == FB_BLANK_POWERDOWN) {
				if (chip->oem_lcd_is_on != false)
					set_property_on_fg(chip,
						POWER_SUPPLY_PROP_UPDATE_LCD_IS_OFF, 1);
				chip->oem_lcd_is_on = false;
			}
		}
	}

	return 0;
}

void checkout_term_current(struct smb_charger *chg, int batt_temp)
{
	static int term_current_reached;
	int current_ma, voltage_mv, temp_region, batt_status;

	batt_status = get_prop_batt_status(chg);
	if (batt_status != POWER_SUPPLY_STATUS_CHARGING)
		return;

	current_ma = get_prop_batt_current_now(chg) / 1000;
	if (!(current_ma >= -SOFT_CHG_TERM_CURRENT
			&& current_ma <= SOFT_CHG_TERM_CURRENT)) {
		/* soft charge term set to 100mA */
		term_current_reached = 0;
		return;
	}

	voltage_mv = get_prop_batt_voltage_now(chg) / 1000;
	temp_region = op_battery_temp_region_get(chg);
	if (voltage_mv >= chg->vbatmax[temp_region]) {
		term_current_reached++;
	} else {
		term_current_reached = 0;
		return;
	}

	if (term_current_reached >= 5) {
		chg->chg_done = true;
		term_current_reached = 0;
		pr_err("chg_done: CAP=%d (Q:%d), VBAT=%d (Q:%d), IBAT=%d (Q:%d), BAT_TEMP=%d\n",
				get_prop_batt_capacity(chg),
				get_prop_fg_capacity(chg),
				get_prop_batt_voltage_now(chg) / 1000,
				get_prop_fg_voltage_now(chg) / 1000,
				get_prop_batt_current_now(chg) / 1000,
				get_prop_fg_current_now(chg) / 1000,
				get_prop_batt_temp(chg));
		op_charging_en(chg, false);
	}
}

static void check_non_standard_charger_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
	struct smb_charger, non_standard_charger_check_work);

	bool charger_present;
	const struct apsd_result *apsd_result;
	int aicl_result, rc;

	pr_debug("chg->non_stand_chg_count=%d\n",
		chg->non_stand_chg_count);

	charger_present = is_usb_present(chg);
	if (!charger_present) {
		pr_info("chk_non_std_chger,charger_present\n");
		chg->non_stand_chg_count = 0;
		return;
	}
	if (chg->usb_enum_status) {
		pr_info("chk_non_std_chger,usb_enum_status\n");
		chg->non_stand_chg_count = 0;
		return;
	}
	if (chg->non_stand_chg_count
		>= NON_STANDARD_CHARGER_CHECK_S) {
		apsd_result = smblib_update_usb_type(chg);
		if (apsd_result->bit == DCP_CHARGER_BIT
			|| apsd_result->bit == OCP_CHARGER_BIT)
			return;
		rc = smblib_rerun_aicl(chg);
		if (rc < 0)
			smblib_err(chg, "Couldn't re-run AICL rc=%d\n", rc);
		msleep(500);
		aicl_result = op_get_aicl_result(chg);
		chg->non_stand_chg_current = aicl_result;
		chg->usb_psy_desc.type = POWER_SUPPLY_TYPE_USB_DCP;
		if (chg->is_aging_test)
			op_usb_icl_set(chg, DEFAULT_AGAING_CHG_MA*1000);
		else
			op_usb_icl_set(chg, DEFAULT_DCP_MA*1000);
		power_supply_changed(chg->batt_psy);
		chg->is_power_changed = true;
		chg->non_std_chg_present = true;
		pr_err("non-standard_charger detected,aicl_result=%d\n",
			aicl_result);
	} else {
		chg->non_stand_chg_count++;
		schedule_delayed_work(
			&chg->non_standard_charger_check_work,
			msecs_to_jiffies(TIME_1000MS));
	}
}

static void smbchg_re_det_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			re_det_work.work);

	pr_debug("chg->redet_count=%d\n", chg->redet_count);
	if (chg->usb_enum_status) {
		pr_info("re_det, usb_enum_status\n");
		chg->redet_count = 0;
		return;
	}
	if (!chg->vbus_present) {
		pr_info("re_det, vbus_no_present\n");
		chg->redet_count = 0;
		return;
	}

	if (chg->redet_count >= REDET_COUTNT) {
		op_rerun_apsd(chg);
		chg->usb_type_redet_done = true;
	} else {
		chg->redet_count++;
		schedule_delayed_work(&chg->re_det_work,
				msecs_to_jiffies(TIME_1000MS));
	}
}

static void op_recovery_set_work(struct work_struct *work)
{
	struct smb_charger *chg = container_of(work,
			struct smb_charger,
			op_re_set_work.work);
	int rc;

	pr_debug("chg->reset_count=%d\n", chg->reset_count);
	if (!chg->charger_collpse) {
		chg->reset_count = 0;
		return;
	}
	if (!chg->vbus_present) {
		chg->reset_count = 0;
		return;
	}

	if (chg->reset_count >= 13) {

		pr_err("op_set_collapse_fet\n");
		rc = smblib_write(chg, USBIN_AICL_OPTIONS_CFG_REG, 0xc7);
		if (rc < 0)
			smblib_err(chg,
			"Couldn't enable OTG regulator rc=%d\n", rc);
		chg->charger_collpse = false;
		chg->reset_count = 0;
	} else {
		chg->reset_count++;
		schedule_delayed_work(&chg->op_re_set_work,
				msecs_to_jiffies(TIME_1000MS));
	}
}

void aging_test_check_aicl(struct smb_charger *chg)
{
	int aicl_result, vbat;

	if (chg->usb_enum_status)
		return;
	vbat = get_prop_fg_voltage_now(chg) / 1000;
	aicl_result = op_get_aicl_result(chg);
	if (aicl_result < 800*1000) {
		if (vbat < 4000) {
			pr_info("set icl 900mA\n");
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER,
				true, 900*1000);
			vote(chg->usb_icl_votable, AICL_RERUN_VOTER, false, 0);
		}
	}
}

static void op_heartbeat_work(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct smb_charger *chg = container_of(dwork,
			struct smb_charger, heartbeat_work);
	enum temp_region_type temp_region;
	bool charger_present;
	bool fast_charging;
	static int batt_temp, vbat_mv;
	union power_supply_propval vbus_val;
	int rc;
#ifndef CONFIG_OP_DEBUG_CHG
	op_check_charge_timeout(chg);
#endif
	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		pr_err("failed to read usb_voltage rc=%d\n", rc);
		vbus_val.intval = CHG_VOLTAGE_NORMAL * 1000;
	}

	charger_present = is_usb_present(chg);
	if (!charger_present)
		goto out;

	/* charger present */
	power_supply_changed(chg->batt_psy);
	chg->dash_on = get_prop_fast_chg_started(chg);
	if (chg->dash_on) {
		switch_fast_chg(chg);
		pr_info("fast chg started, usb_switch=%d\n",
				op_is_usb_switch_on(chg));
		/* add for disable normal charge */
		fast_charging = op_get_fastchg_ing(chg);
		if (fast_charging) {
			if (!chg->disable_normal_chg_for_dash)
				op_charging_en(chg, false);
			chg->disable_normal_chg_for_dash = true;
		}
		goto out;
	} else {
		if (chg->disable_normal_chg_for_dash) {
			chg->disable_normal_chg_for_dash = false;
			op_charging_en(chg, true);
		}
		schedule_delayed_work(&chg->check_switch_dash_work,
							msecs_to_jiffies(100));
	}

	op_check_charger_uovp(chg, vbus_val.intval / 1000);
	op_check_battery_uovp(chg);
	if (vbus_val.intval / 1000 > 4500)
		op_check_charger_collapse(chg);

	vbat_mv = get_prop_batt_voltage_now(chg) / 1000;
	temp_region = op_battery_temp_region_get(chg);
	if (temp_region == BATT_TEMP_LITTLE_COOL) {
		if (vbat_mv > 4180 + 20
		&& chg->temp_littel_cool_set_current_0_point_25c) {
			chg->is_power_changed = true;
		} else if (vbat_mv < 4180 - 10
		&& !chg->temp_littel_cool_set_current_0_point_25c) {
			chg->is_power_changed = true;
		}
	}

	batt_temp = get_prop_batt_temp(chg);
	checkout_term_current(chg, batt_temp);
	if (!chg->chg_ovp && chg->chg_done
			&& temp_region > BATT_TEMP_COLD
			&& temp_region < BATT_TEMP_HOT
			&& chg->vbatdet[temp_region] >= vbat_mv) {
		chg->chg_done = false;
		chg->recharge_pending = true;
		chg->recharge_status = true;

		op_charging_en(chg, true);
		pr_debug("temp_region=%d, recharge_pending\n", temp_region);
	}

	if (!chg->chg_ovp && chg->battery_status == BATT_STATUS_GOOD
			&& !chg->time_out) {
		op_check_battery_temp(chg);
	}
#ifdef	CONFIG_OP_DEBUG_CHG
	chg->dump_count++;
	if (chg->dump_count == 600) {
		chg->dump_count = 0;
		if ((get_prop_batt_current_now(chg) / 1000) > 0) {
			op_dump_regs(chg);
			aging_test_check_aicl(chg);
		}
	}
#else
	if (chg->is_aging_test) {
		chg->dump_count++;
		if (chg->dump_count == 600) {
			chg->dump_count = 0;
			if ((get_prop_batt_current_now(chg) / 1000) > 0) {
				op_dump_regs(chg);
				aging_test_check_aicl(chg);
			}
		}
	}

#endif
out:

	/*update time 6s*/
	schedule_delayed_work(&chg->heartbeat_work,
			round_jiffies_relative(msecs_to_jiffies
				(HEARTBEAT_INTERVAL_MS)));
}

static int op_read(struct smb_charger *chg, u16 addr, u8 *val)
{
	unsigned int temp;
	int rc = 0;

	if (pm_pon_regmap) {
		rc = regmap_read(pm_pon_regmap, addr, &temp);
		if (rc >= 0)
			*val = (u8)temp;
	}
	return rc;
}

int op_read_backup_flag(struct smb_charger *chg)
{
	u8 flag;
	int rc = 0;

	rc = op_read(chg, SOC_DATA_REG_0, &flag);
	if (rc) {
		pr_err("failed to read PM addr[0x%x], rc=%d\n",
			SOC_DATA_REG_0, rc);
		return 0;
	}
	flag = flag & BIT(7);
	return flag;
}

int op_masked_write(struct smb_charger *chg, u16 addr, u8 mask, u8 val)
{
	int rc = 0;

	if (pm_pon_regmap) {
		mutex_lock(&chg->write_lock);
		rc = regmap_update_bits(pm_pon_regmap, addr, mask, val);
		mutex_unlock(&chg->write_lock);
	} else {
		pr_err("PMIC PON regmap is NULL\n");
	}
	return rc;
}

void op_write_backup_flag(struct smb_charger *chg, bool bk_flag)
{
	int rc = 0;

	rc = op_masked_write(chg, SOC_DATA_REG_0,
		BIT(7), bk_flag ? BIT(7):0);
	if (rc) {
		pr_err("failed to clean PM addr[0x%x], rc=%d\n",
			SOC_DATA_REG_0, rc);
	}
}

static int load_data(struct smb_charger *chg)
{
	u8 stored_soc = 0, flag;
	int rc = 0, shutdown_soc = 0;

	if (!chg) {
		pr_err("chg is NULL !\n");
		return SOC_INVALID;
	}
	flag = op_read_backup_flag(chg);
	if (!flag)
		return SOC_INVALID;

	rc = smblib_read(chg, SOC_DATA_REG_0, &stored_soc);
	if (rc) {
		pr_err("failed to read addr[0x%x], rc=%d\n",
			SOC_DATA_REG_0, rc);
		return SOC_INVALID;
	}

	/* the fist time connect battery, the PM 0x88d bit7 is 0,*/
	/* we do not need load this data.*/
	if (flag)
		shutdown_soc = (stored_soc >> 1);
	/* get data from bit1~bit7 */
	else
		shutdown_soc = SOC_INVALID;

	pr_info("stored_soc[0x%x], shutdown_soc[%d]\n",
		stored_soc, shutdown_soc);
	return shutdown_soc;
}

int load_soc(void)
{
	int soc = 0;

	soc = load_data(g_chg);
	if (soc == SOC_INVALID || soc < 0 || soc > 100)
		return -EINVAL;
	return soc;
}

static void clear_backup_soc(struct smb_charger *chg)
{
	int rc = 0;
	u8 invalid_soc = SOC_INVALID;

	op_write_backup_flag(chg, false);
	rc = smblib_masked_write(chg, SOC_DATA_REG_0,
		BIT(7) | BIT(6) | BIT(5) | BIT(4)
		| BIT(3) | BIT(2) | BIT(1),
		(BIT(7) & invalid_soc) | (BIT(6) & invalid_soc)
		| (BIT(5) & invalid_soc) |
		(BIT(4) & invalid_soc) | (BIT(3) & invalid_soc)
		| (BIT(2) & invalid_soc) |
		(BIT(1) & invalid_soc));
	if (rc)
		pr_err("failed to write addr[0x%x], rc=%d\n",
						SOC_DATA_REG_0, rc);
	if (rc) {
		pr_err("failed to clean PM addr[0x%x], rc=%d\n",
			SOC_DATA_REG_0, rc);
	}
}

void clean_backup_soc_ex(void)
{
	if (g_chg)
		clear_backup_soc(g_chg);
}

static void backup_soc(struct smb_charger *chg, int soc)
{
	int rc = 0;

	if (!chg || soc < 0 || soc > 100) {
		pr_err("chg or soc invalid, store an invalid soc\n");
		if (chg) {
			rc = smblib_masked_write(chg, SOC_DATA_REG_0,
				BIT(7) | BIT(6) | BIT(5)
				| BIT(4) | BIT(3) | BIT(2) | BIT(1),
				BIT(7) | BIT(6) | BIT(5)
				| BIT(4) | BIT(3) | BIT(2) | BIT(1));
			if (rc)
				pr_err("failed to write addr[0x%x], rc=%d\n",
						SOC_DATA_REG_0, rc);
			op_write_backup_flag(chg, false);
		}
		return;
	}

	pr_err("backup_soc[%d]\n", soc);
	soc = soc*2;
	rc = smblib_masked_write(chg, SOC_DATA_REG_0,
	BIT(7) | BIT(6) | BIT(5)|
	BIT(4) | BIT(3) | BIT(2) | BIT(1),
	(BIT(7) & soc) | (BIT(6) & soc) | (BIT(5) & soc) |
	(BIT(4) & soc) | (BIT(3) & soc) | (BIT(2) & soc) |
	(BIT(1) & soc));
	if (rc)
		pr_err("failed to write addr[0x%x], rc=%d\n",
				SOC_DATA_REG_0, rc);
	op_write_backup_flag(chg, true);
}

void backup_soc_ex(int soc)
{
	if (g_chg)
		backup_soc(g_chg, soc);
}

int get_prop_chg_protect_status(struct smb_charger *chg)
{
	int temp, rc;
	bool batt_present;
	enum temp_region_type temp_region;
	union power_supply_propval vbus_val;

	if (chg->use_fake_protect_sts)
		return chg->fake_protect_sts;

	if (!is_usb_present(chg))
		return 0;

	rc = smblib_get_prop_usb_voltage_now(chg, &vbus_val);
	if (rc < 0) {
		pr_err("failed to read usb_voltage rc=%d\n", rc);
		vbus_val.intval = CHG_VOLTAGE_NORMAL * 1000;
	}

	temp = get_prop_batt_temp(chg);
	batt_present = get_prop_batt_present(chg);
	temp_region = op_battery_temp_region_get(chg);
	if (chg->chg_ovp &&
			vbus_val.intval / 1000 >= CHG_SOFT_OVP_MV - 100)
		return PROTECT_CHG_OVP;
	else if (!batt_present || BATT_REMOVE_TEMP > temp)
		return  PROTECT_BATT_MISSING;
	else if (chg->battery_status == BATT_STATUS_BAD)
		return PROTECT_BATT_OVP;
	else if (true == chg->time_out)
		return PROTECT_CHG_OVERTIME;
	else if (temp_region == BATT_TEMP_HOT)
		return PROTECT_BATT_TEMP_REGION__HOT;
	else if (temp_region == BATT_TEMP_COLD)
		return PROTECT_BATT_TEMP_REGION_COLD;
	else if (temp_region == BATT_TEMP_LITTLE_COLD
			&& (chg->chg_done || chg->recharge_status))
		return PROTECT_BATT_TEMP_REGION_LITTLE_COLD;
	else if (temp_region == BATT_TEMP_WARM
			&& (chg->chg_done || chg->recharge_status))
		return PROTECT_BATT_TEMP_REGION_WARM;
	else
		return 0;
}

bool get_prop_fastchg_status(struct smb_charger *chg)
{
	return chg->dash_present;
}

void fastcharge_information_register(struct external_battery_gauge *fast_chg)
{
	if (fast_charger) {
		fast_charger = fast_chg;
		pr_err("multiple battery gauge called\n");
	} else {
		fast_charger = fast_chg;
	}
}

void op_pm8998_regmap_register(struct regmap *regmap)
{
	if (pm_pon_regmap)
		pr_err("multiple PMIC PON regmaps registered\n");

	pm_pon_regmap = regmap;
}

void fastcharge_information_unregister(struct external_battery_gauge *fast_chg)
{
	fast_charger = NULL;
}

static int notify_usb_enumeration_function(int status)
{
	pr_info("status=%d\n", status);
	g_chg->usb_enum_status = status;

	return g_chg->usb_enum_status;
}

static struct notify_dash_event notify_unplug_event = {
	.notify_event	= update_dash_unplug_status,
	.op_contrl	= op_contrl,
	.notify_dash_charger_present
		= set_dash_charger_present,
};

static struct notify_usb_enumeration_status usb_enumeration = {
	.notify_usb_enumeration	= notify_usb_enumeration_function,
};

void op_dash_charger_init(struct smb_charger *chg)
{
	mutex_init(&chg->sw_dash_lock);

	INIT_WORK(&chg->re_kick_work, op_re_kick_work);
	INIT_DELAYED_WORK(&chg->rechk_sw_dsh_work, retrigger_dash_work);
	INIT_DELAYED_WORK(&chg->op_check_apsd_work, op_chek_apsd_done_work);
	INIT_DELAYED_WORK(&chg->recovery_suspend_work,
			op_recovery_usb_suspend_work);
	INIT_DELAYED_WORK(&chg->check_switch_dash_work,
			op_check_allow_switch_dash_work);
	INIT_DELAYED_WORK(&chg->heartbeat_work, op_heartbeat_work);
	INIT_DELAYED_WORK(&chg->non_standard_charger_check_work,
			check_non_standard_charger_work);
	INIT_DELAYED_WORK(&chg->re_det_work, smbchg_re_det_work);
	INIT_DELAYED_WORK(&chg->op_re_set_work, op_recovery_set_work);
	schedule_delayed_work(&chg->heartbeat_work,
			msecs_to_jiffies(HEARTBEAT_INTERVAL_MS));

	notify_dash_unplug_register(&notify_unplug_event);
	chg->chg_wake_lock.name = "chg_wake_lock";
	wakeup_source_add(&chg->chg_wake_lock);
	g_chg = chg;

	regsister_notify_usb_enumeration_status(&usb_enumeration);
#if defined(CONFIG_FB)
	chg->fb_notif.notifier_call = fb_notifier_callback;
	if (fb_register_client(&chg->fb_notif))
		pr_err("op_dash_charger_init: unable to register fb_notifier\n");
#endif
	op_set_collapse_fet(chg, false);
}

void op_dash_charger_deinit(struct smb_charger *chg)
{
	notify_dash_unplug_unregister(&notify_unplug_event);
}
