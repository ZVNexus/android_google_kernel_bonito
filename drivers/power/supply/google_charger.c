/*
 * Copyright 2018 Google, Inc
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/printk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/pm_wakeup.h>

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#endif

#define CHG_TEMP_NB_LIMITS_MAX 10
#define CHG_VOLT_NB_LIMITS_MAX 5
#define CHG_DELAY_INIT_MS 250

#define DEFAULT_CHARGE_STOP_LEVEL 100
#define DEFAULT_CHARGE_START_LEVEL 0

struct chg_profile {
	u32 update_interval;
	u32 battery_capacity;
	u32 temp_nb_limits;
	s32 temp_limits[CHG_TEMP_NB_LIMITS_MAX];
	u32 volt_nb_limits;
	s32 volt_limits[CHG_VOLT_NB_LIMITS_MAX];
	/* Array of constant current limits */
	s32 *cccm_limits;
	u32 cv_hw_resolution;
	u32 cc_hw_resolution;
	u32 cv_range_accuracy;
	u32 cv_debounce_cnt;
	u32 cv_update_interval;
	u32 chg_cc_tolerance;
	u32 cv_tier_ov_cnt;
};

struct chg_drv {
	struct device *device;
	struct power_supply *chg_psy;
	const char *chg_psy_name;
	struct power_supply *usb_psy;
	struct power_supply *wlc_psy;
	const char *wlc_psy_name;
	struct power_supply *bat_psy;
	const char *bat_psy_name;
	struct chg_profile chg_profile;
	struct notifier_block psy_nb;

	struct delayed_work init_work;
	struct delayed_work chg_work;
	struct wakeup_source chg_ws;

	bool stop_charging;
	int temp_idx;
	int vbatt_idx;
	int checked_cv_cnt;
	int checked_ov_cnt;

	int chg_mode;
	int fv_uv;
	int disable_charging;
	int disable_pwrsrc;
	bool lowerdb_reached;
	int charge_stop_level;
	int charge_start_level;
};

/* Used as left operand also */
#define CCCM_LIMITS(profile, ti, vi) \
	profile->cccm_limits[(ti * profile->volt_nb_limits) + vi]

static int psy_changed(struct notifier_block *nb,
		       unsigned long action, void *data)
{
	struct power_supply *psy = data;
	struct chg_drv *chg_drv = container_of(nb, struct chg_drv, psy_nb);

	pr_debug("name=%s evt=%lu\n", psy->desc->name, action);

	if ((action != PSY_EVENT_PROP_CHANGED) ||
	    (psy == NULL) || (psy->desc == NULL) || (psy->desc->name == NULL))
		return NOTIFY_OK;

	if (action == PSY_EVENT_PROP_CHANGED &&
	    (!strcmp(psy->desc->name, chg_drv->chg_psy_name) ||
	     !strcmp(psy->desc->name, chg_drv->bat_psy_name) ||
	     !strcmp(psy->desc->name, "usb") ||
	     (chg_drv->wlc_psy_name &&
	      !strcmp(psy->desc->name, chg_drv->wlc_psy_name)))) {
		cancel_delayed_work(&chg_drv->chg_work);
		schedule_delayed_work(&chg_drv->chg_work, 0);
	}
	return NOTIFY_OK;
}

static char *psy_chgt_str[] = {
	"Unknown", "N/A", "Trickle", "Fast", "Taper"
};

static char *psy_usb_type_str[] = {
	"Unknown", "Battery", "UPS", "Mains", "USB", "USB_DCP",
	"USB_CDP", "USB_ACA", "USB_HVDCP", "USB_HVDCP_3", "USB_PD",
	"Wireless", "USB_FLOAT", "BMS", "Parallel", "Main", "Wipower",
	"TYPEC", "TYPEC_UFP", "TYPEC_DFP"
};

#define PSY_GET_PROP(psy, psp) psy_get_prop(psy, psp, #psp)
static inline int psy_get_prop(struct power_supply *psy,
			       enum power_supply_property psp, char *prop_name)
{
	union power_supply_propval val;
	int ret = 0;

	if (!psy)
		return -EINVAL;
	ret = power_supply_get_property(psy, psp, &val);
	if (ret < 0) {
		pr_err("failed to get %s from '%s', ret=%d\n",
		       prop_name, psy->desc->name, ret);
		return -EINVAL;
	}
	pr_debug("get %s for '%s' => %d\n",
		 prop_name, psy->desc->name, val.intval);
	return val.intval;
}

#define PSY_SET_PROP(psy, psp, val) psy_set_prop(psy, psp, val, #psp)
static inline int psy_set_prop(struct power_supply *psy,
			       enum power_supply_property psp,
			       int intval, char *prop_name)
{
	union power_supply_propval val;
	int ret = 0;

	if (!psy)
		return -EINVAL;
	val.intval = intval;
	pr_debug("set %s for '%s' to %d\n", prop_name, psy->desc->name, intval);
	ret = power_supply_set_property(psy, psp, &val);
	if (ret < 0) {
		pr_err("failed to set %s for '%s', ret=%d\n",
		       prop_name, psy->desc->name, ret);
		return -EINVAL;
	}
	return 0;
}

static inline void reset_chg_drv_state(struct chg_drv *chg_drv)
{
	chg_drv->temp_idx = -1;
	chg_drv->vbatt_idx = -1;
	chg_drv->fv_uv = -1;
	chg_drv->checked_cv_cnt = 0;
	chg_drv->checked_ov_cnt = 0;
	chg_drv->disable_charging = 0;
	chg_drv->disable_pwrsrc = 0;
	chg_drv->lowerdb_reached = true;
	PSY_SET_PROP(chg_drv->chg_psy,
		     POWER_SUPPLY_PROP_TAPER_CONTROL_ENABLED, 0);
}
static void pr_info_states(struct power_supply *chg_psy,
			   struct power_supply *usb_psy,
			   struct power_supply *wlc_psy,
			   int temp, int ibatt, int vbatt, int vchrg,
			   int chg_type, int fv_uv,
			   int soc, int usb_present, int wlc_online)
{
	int usb_type = PSY_GET_PROP(usb_psy, POWER_SUPPLY_PROP_REAL_TYPE);

	pr_info("l=%d vb=%d vc=%d c=%d fv=%d t=%d s=%s usb=%d wlc=%d\n",
		soc, vbatt / 1000, vchrg / 1000, ibatt / 1000,
		fv_uv, temp, psy_chgt_str[chg_type],
		usb_present, wlc_online);

	if (usb_present)
		pr_info("usbchg=%s usbv=%d usbc=%d usbMv=%d usbMc=%d\n",
			psy_usb_type_str[usb_type],
			PSY_GET_PROP(usb_psy,
				     POWER_SUPPLY_PROP_VOLTAGE_NOW) / 1000,
			PSY_GET_PROP(usb_psy,
				     POWER_SUPPLY_PROP_INPUT_CURRENT_NOW)/1000,
			PSY_GET_PROP(usb_psy,
				     POWER_SUPPLY_PROP_VOLTAGE_MAX) / 1000,
			PSY_GET_PROP(usb_psy,
				     POWER_SUPPLY_PROP_CURRENT_MAX) / 1000);

	if (wlc_online)
		pr_info("wlcv=%d wlcc=%d wlcMv=%d wlcMc=%d wlct=%d\n",
			PSY_GET_PROP(wlc_psy,
				     POWER_SUPPLY_PROP_VOLTAGE_NOW) / 1000,
			PSY_GET_PROP(wlc_psy,
				     POWER_SUPPLY_PROP_CURRENT_NOW) / 1000,
			PSY_GET_PROP(wlc_psy,
				     POWER_SUPPLY_PROP_VOLTAGE_MAX) / 1000,
			PSY_GET_PROP(wlc_psy,
				     POWER_SUPPLY_PROP_CURRENT_MAX) / 1000,
			PSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_TEMP));
}

/* returns 1 if charging should be disabled given the current battery capacity
 * given in percent, return 0 if charging should happen
 */
static int is_charging_disabled(struct chg_drv *chg_drv, int capacity)
{
	int disable_charging = 0;
	int upperbd = chg_drv->charge_stop_level;
	int lowerbd = chg_drv->charge_start_level;

	if ((upperbd == DEFAULT_CHARGE_STOP_LEVEL) &&
	    (lowerbd == DEFAULT_CHARGE_START_LEVEL))
		return 0;

	if ((upperbd > lowerbd) &&
	    (upperbd <= DEFAULT_CHARGE_STOP_LEVEL) &&
	    (lowerbd >= DEFAULT_CHARGE_START_LEVEL)) {
		if (chg_drv->lowerdb_reached && upperbd <= capacity) {
			pr_info("%s: lowerbd=%d, upperbd=%d, capacity=%d, lowerdb_reached=1->0, charging off\n",
				__func__, lowerbd, upperbd, capacity);
			disable_charging = 1;
			chg_drv->lowerdb_reached = false;
		} else if (!chg_drv->lowerdb_reached && lowerbd < capacity) {
			pr_info("%s: lowerbd=%d, upperbd=%d, capacity=%d, charging off\n",
				__func__, lowerbd, upperbd, capacity);
			disable_charging = 1;
		} else if (!chg_drv->lowerdb_reached && capacity <= lowerbd) {
			pr_info("%s: lowerbd=%d, upperbd=%d, capacity=%d, lowerdb_reached=0->1, charging on\n",
				__func__, lowerbd, upperbd, capacity);
			chg_drv->lowerdb_reached = true;
		} else {
			pr_info("%s: lowerbd=%d, upperbd=%d, capacity=%d, charging on\n",
				__func__, lowerbd, upperbd, capacity);
		}
	}

	return disable_charging;
}


/* 1. charge profile idx based on the battery temperature */
int msc_temp_idx(struct chg_drv *chg_drv, int temp)
{
	int temp_idx = 0;

	while (temp_idx < chg_drv->chg_profile.temp_nb_limits - 1 &&
	       temp >= chg_drv->chg_profile.temp_limits[temp_idx + 1])
		temp_idx++;

	return temp_idx;
}

/* 2. compute the step index given the battery voltage  */
int msc_voltage_idx(struct chg_drv *chg_drv, int vbatt)
{
	int vbatt_idx = 0;

	while (vbatt_idx < chg_drv->chg_profile.volt_nb_limits - 1 &&
	       vbatt > chg_drv->chg_profile.volt_limits[vbatt_idx])
		vbatt_idx++;

	return vbatt_idx;
}

#define CHG_WORK_ERROR_RETRY_MS 1000
static void chg_work(struct work_struct *work)
{
	struct chg_drv *chg_drv =
	    container_of(work, struct chg_drv, chg_work.work);
	struct chg_profile *profile = &chg_drv->chg_profile;
	union power_supply_propval val;
	struct power_supply *chg_psy = chg_drv->chg_psy;
	struct power_supply *usb_psy = chg_drv->usb_psy;
	struct power_supply *wlc_psy = chg_drv->wlc_psy;
	struct power_supply *bat_psy = chg_drv->bat_psy;
	int temp, ibatt, vbatt, vchrg, soc, chg_type;
	int usb_present, wlc_online = 0;
	int vbatt_idx = chg_drv->vbatt_idx, fv_uv = chg_drv->fv_uv, temp_idx;
	int update_interval = chg_drv->chg_profile.update_interval;
	int batt_status;
	bool rerun_work = false;
	int disable_charging;
	int disable_pwrsrc;

	__pm_stay_awake(&chg_drv->chg_ws);
	pr_debug("battery charging work item\n");

	/* debug option  */
	if (chg_drv->chg_mode)
		bat_psy = chg_psy;

	temp = PSY_GET_PROP(bat_psy, POWER_SUPPLY_PROP_TEMP);
	ibatt = PSY_GET_PROP(bat_psy, POWER_SUPPLY_PROP_CURRENT_NOW);
	vbatt = PSY_GET_PROP(bat_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW);
	soc = PSY_GET_PROP(bat_psy, POWER_SUPPLY_PROP_CAPACITY);
	vchrg = PSY_GET_PROP(chg_psy, POWER_SUPPLY_PROP_VOLTAGE_NOW);
	chg_type = PSY_GET_PROP(chg_psy, POWER_SUPPLY_PROP_CHARGE_TYPE);

	usb_present = PSY_GET_PROP(usb_psy, POWER_SUPPLY_PROP_PRESENT);
	if (wlc_psy)
		wlc_online = PSY_GET_PROP(wlc_psy, POWER_SUPPLY_PROP_ONLINE);

	if (temp == -EINVAL || ibatt == -EINVAL || vbatt == -EINVAL ||
	    usb_present == -EINVAL || wlc_online == -EINVAL)
		goto error_rerun;

	pr_info_states(chg_psy, usb_psy, wlc_psy,
		       temp, ibatt, vbatt, vchrg,
		       chg_type, chg_drv->fv_uv,
		       soc, usb_present, wlc_online);

	if (!usb_present && !wlc_online)
		goto check_rerun;

	if (temp < profile->temp_limits[0] ||
	    temp > profile->temp_limits[profile->temp_nb_limits - 1]) {
		if (!chg_drv->stop_charging) {
			pr_info("batt. temp. off limits, disabling charging\n");
			PSY_SET_PROP(chg_psy,
				     POWER_SUPPLY_PROP_CHARGE_DISABLE, 1);
			chg_drv->stop_charging = true;
			reset_chg_drv_state(chg_drv);
		}
		/* status will be discharging when disabled but we want to keep
		 * monitoring temperature to re-enable charging
		 */
		rerun_work = true;
		goto handle_rerun;
	} else if (chg_drv->stop_charging) {
		pr_info("batt. temp. ok, enabling charging\n");
		PSY_SET_PROP(chg_psy, POWER_SUPPLY_PROP_CHARGE_DISABLE, 0);
		chg_drv->stop_charging = false;
	}

	disable_charging = is_charging_disabled(chg_drv, soc);
	if (disable_charging && soc > chg_drv->charge_stop_level)
		disable_pwrsrc = 1;
	else
		disable_pwrsrc = 0;

	if (disable_charging != chg_drv->disable_charging) {
		pr_info("set disable_charging(%d)", disable_charging);
		PSY_SET_PROP(chg_psy, POWER_SUPPLY_PROP_CHARGE_DISABLE,
			     disable_charging);
	}
	chg_drv->disable_charging = disable_charging;

	if (disable_pwrsrc != chg_drv->disable_pwrsrc) {
		pr_info("set disable_pwrsrc(%d)", disable_pwrsrc);
		PSY_SET_PROP(chg_psy, POWER_SUPPLY_PROP_INPUT_SUSPEND,
			     disable_pwrsrc);
	}
	chg_drv->disable_pwrsrc = disable_pwrsrc;

	/* no need to reschedule, battery psy event will reschedule work item */
	if (chg_drv->disable_charging || chg_drv->disable_pwrsrc) {
		rerun_work = false;
		goto exit_chg_work;
	}

	/* Multi Step Chargings with compensation of IRDROP
	 * vbatt_idx = chg_drv->vbatt_idx, fv_uv = chg_drv->fv_uv
	 */
	temp_idx = msc_temp_idx(chg_drv, temp);
	if (temp_idx != chg_drv->temp_idx || chg_drv->fv_uv == -1 ||
		chg_drv->vbatt_idx == -1) {
		/* temperature changed or startup, (re)seed... */
		vbatt_idx = msc_voltage_idx(chg_drv, vbatt);
		/* voltage tier desn't go back when temperature changes */
		if (vbatt_idx < chg_drv->vbatt_idx)
			vbatt_idx = chg_drv->vbatt_idx;
		/* keep voltage steady (keep corrections) when in same tier */
		if (vbatt_idx != chg_drv->vbatt_idx)
			fv_uv = profile->volt_limits[vbatt_idx];

		pr_info("MSC_SEED temp_idx:%d->%d, vbatt_idx:%d->%d, fv=%d->%d\n",
			chg_drv->temp_idx, temp_idx, chg_drv->vbatt_idx,
			vbatt_idx, chg_drv->fv_uv, fv_uv);

		/* TODO: should I debounce taper and raise? */
		chg_drv->checked_cv_cnt = 0;
		chg_drv->checked_ov_cnt = 0;

	} else if (ibatt > 0) {
		/* discharging: check_rerun will reset chg_drv state() */
		pr_info("MSC_DISCHARGE ibatt=%d\n", ibatt);
		goto check_rerun;
	} else if (chg_drv->vbatt_idx == profile->volt_nb_limits - 1) {
		/* Last tier: taper control is set on entrance.
		 * NOTE: this might not be the "real" last tier since I have
		 * tiers with max charge current == 0
		 * TODO: add check on busting vMAX?
		 */
		pr_info("MSC_LAST vbatt=%d fv_uv=%d\n", vbatt, fv_uv);
	} else {
		const int vtier = profile->volt_limits[vbatt_idx];
		const int cc_next_max = CCCM_LIMITS(profile, temp_idx,
						    vbatt_idx + 1);

		if (vbatt > vtier) {
		/* OVER: when over tier vlim, pullback, set fast poll, reset
		 * taper debounce (so we check if we are at next tier right
		 * away), set penalty on raising the voltage limit again.
		 * TODO: figure out if there is something really wrong
		 * EX: vbatt more than cv_hw_resolution over tier
		 */
			fv_uv -= profile->cv_hw_resolution;
			update_interval = profile->cv_update_interval;
			chg_drv->checked_cv_cnt = 0;

			chg_drv->checked_ov_cnt = profile->cv_tier_ov_cnt;
			/* NOTE: might get here after windup (was current
			 * limited in taper due to load) because algo will
			 * consider the reduction in voltage caused from load
			 * as IRDROP. Not an issue if current limited clear
			 * taper condition.
			 */

			pr_info("MSC_PULLBACK vbatt=%d vtier=%d fv_uv=%d->%d\n",
				vbatt, vtier, chg_drv->fv_uv, fv_uv);
		} else if (chg_type == POWER_SUPPLY_CHARGE_TYPE_FAST) {
		/* FAST: usual compensation (vchrg is vqcom) */
			fv_uv = vtier + (vchrg - vbatt)
				+ profile->cv_hw_resolution - 1;
			fv_uv = (fv_uv  / profile->cv_hw_resolution)
				* profile->cv_hw_resolution;

			/* NOTE: setting checked_cv_cnt>0 here will debounce
			 * taper entry. Might really need it at cold or with
			 * age but likely not...
			 */
			chg_drv->checked_cv_cnt = profile->cv_debounce_cnt;

			pr_info("MSC_FAST vtier=%d vbatt=%d vchrg=%d fv_uv=%d->%d\n",
				vtier, vbatt, vchrg, chg_drv->fv_uv, fv_uv);

			/* make sure we skip checking tier switch */
		} else if (chg_type != POWER_SUPPLY_CHARGE_TYPE_TAPER) {
		/* TODO: UNKNOWN, NONE or TRICKLE should reset checked? */
			pr_info("MSC_TYPE chg_type=%d vbatt=%d vtier=%d fv_uv=%d\n",
				chg_type, vbatt, vtier, fv_uv);
			goto check_rerun;
		} else if (chg_drv->checked_cv_cnt + chg_drv->checked_ov_cnt) {
		/* TAPER_COUNTDOWN: countdown to raise fv_uv and/or check
		 * for tier switch.
		 */
			update_interval = profile->cv_update_interval;
			if (chg_drv->checked_cv_cnt)
				chg_drv->checked_cv_cnt -= 1;
			if (chg_drv->checked_ov_cnt)
				chg_drv->checked_ov_cnt -= 1;

		} else if ((vtier - vbatt) < profile->cv_range_accuracy) {
		/* TAPER_STEADY: close enough to tier, don't need to adjust */
			pr_info("MSC_STEADY vb=%d vt=%d margin=%d fv_uv=%d\n",
				vbatt, vtier, profile->cv_range_accuracy,
				fv_uv);
		} else {
		/* TAPER_RAISE: when under tier vlim, raise one click & debounce
		 * taper (see above handling of "close enough")
		 */
			fv_uv += profile->cv_hw_resolution;
			update_interval = profile->cv_update_interval;

			/* debounce next taper voltage adjustment */
			chg_drv->checked_cv_cnt = profile->cv_debounce_cnt;

			pr_info("MSC_RAISE vbatt=%d vtier=%d fv_uv=%d->%d\n",
				vbatt, vtier, chg_drv->fv_uv, fv_uv);
		}

		if (chg_drv->checked_cv_cnt > 0) {
			pr_info("MSC_WAIT vbatt=%d vtier=%d fv_uv=%d cv_cnt=%d ov_cnt=%d\n",
				vbatt, vtier, chg_drv->fv_uv,
				chg_drv->checked_cv_cnt,
				chg_drv->checked_ov_cnt);
		} else if (-ibatt <= cc_next_max) {
		/* NEXT_TIER: switch to next when current is below next tier */
			vbatt_idx = chg_drv->vbatt_idx + 1;
			fv_uv = profile->volt_limits[vbatt_idx];
			chg_drv->checked_ov_cnt = 0;

			pr_info("MSC_SWITCH tier vbatt=%d vbatt_idx=%d->%d fv_uv=%d->%d\n",
				vbatt, chg_drv->vbatt_idx, vbatt_idx,
				chg_drv->fv_uv, fv_uv);
		} else {
			/* TODO: add taper counter? */
			pr_info("MSC_NYET ibatt=%d cc_next_max=%d\n",
				ibatt, cc_next_max);
		}
	}

	/* update fv or cc will change in last tier... */
	if ((vbatt_idx != chg_drv->vbatt_idx) || (temp_idx != chg_drv->temp_idx)
		|| (fv_uv != chg_drv->fv_uv)) {
		const int cc_max = CCCM_LIMITS(profile, temp_idx, vbatt_idx);
		int rc;

		/* taper control on last tier with nonzero charge current */
		if (vbatt_idx == (profile->volt_nb_limits - 1) ||
			CCCM_LIMITS(profile, temp_idx, vbatt_idx + 1) == 0) {
			PSY_SET_PROP(chg_drv->chg_psy,
				POWER_SUPPLY_PROP_TAPER_CONTROL_ENABLED,
				1);
		}

		rc = PSY_SET_PROP(chg_psy,
			POWER_SUPPLY_PROP_CONSTANT_CHARGE_CURRENT_MAX, cc_max);
		if (rc == 0)
			rc = PSY_SET_PROP(chg_psy,
					POWER_SUPPLY_PROP_VOLTAGE_MAX, fv_uv);

		pr_info("MSC_SET cv_cnt=%d ov_cnt=%d temp_idx:%d->%d, vbatt_idx:%d->%d, fv=%d->%d, cc_max=%d, rc=%d\n",
			chg_drv->checked_cv_cnt, chg_drv->checked_ov_cnt,
			chg_drv->temp_idx, temp_idx, chg_drv->vbatt_idx,
			vbatt_idx, chg_drv->fv_uv, fv_uv, cc_max, rc);
		if (rc != 0)
			goto error_rerun;

		chg_drv->vbatt_idx = vbatt_idx;
		chg_drv->temp_idx = temp_idx;
		chg_drv->fv_uv = fv_uv;
	}

check_rerun:
	batt_status = PSY_GET_PROP(chg_psy, POWER_SUPPLY_PROP_STATUS);

	switch (batt_status) {
	case POWER_SUPPLY_STATUS_DISCHARGING:
		rerun_work = false;
		break;
	case POWER_SUPPLY_STATUS_CHARGING:
	case POWER_SUPPLY_STATUS_FULL:
	case POWER_SUPPLY_STATUS_NOT_CHARGING:
		rerun_work = true;
		break;
	case POWER_SUPPLY_STATUS_UNKNOWN:
		pr_err("chg_work charging status UNKNOWN\n");
		goto error_rerun;
	default:
		pr_err("chg_work invalid charging status %d\n", val.intval);
		goto error_rerun;
	}

handle_rerun:
	if (rerun_work) {
		pr_debug("rerun battery charging work in %d ms\n",
			 update_interval);
		schedule_delayed_work(&chg_drv->chg_work,
				      msecs_to_jiffies(update_interval));
	} else {
		pr_info("stop battery charging work: batt_status=%d\n",
			batt_status);
		reset_chg_drv_state(chg_drv);
	}
	goto exit_chg_work;

error_rerun:
	pr_err("error occurred, rerun battery charging work in %d ms\n",
	       CHG_WORK_ERROR_RETRY_MS);
	schedule_delayed_work(&chg_drv->chg_work,
			      msecs_to_jiffies(CHG_WORK_ERROR_RETRY_MS));

exit_chg_work:
	__pm_relax(&chg_drv->chg_ws);
}

static void dump_profile(struct chg_profile *profile)
{
	char buff[256];
	int ti, vi, count, len = sizeof(buff);

	pr_info("Profile constant charge limits:\n");
	count = 0;
	for (vi = 0; vi < profile->volt_nb_limits; vi++) {
		count += scnprintf(buff + count, len - count, "  %4d",
				   profile->volt_limits[vi] / 1000);
	}
	pr_info("|T \\ V%s\n", buff);

	for (ti = 0; ti < profile->temp_nb_limits - 1; ti++) {
		count = 0;
		count += scnprintf(buff + count, len - count, "|%2d:%2d",
				   profile->temp_limits[ti] / 10,
				   profile->temp_limits[ti + 1] / 10);
		for (vi = 0; vi < profile->volt_nb_limits; vi++) {
			count += scnprintf(buff + count, len - count, "  %4d",
					   CCCM_LIMITS(profile, ti, vi) / 1000);
		}
		pr_info("%s\n", buff);
	}
}

static int chg_init_chg_profile(struct chg_drv *chg_drv)
{
	struct device *dev = chg_drv->device;
	struct device_node *node = dev->of_node;
	struct chg_profile *profile = &chg_drv->chg_profile;
	u32 cccm_array_size, ccm;
	int ret = 0, vi, ti;

	ret = of_property_read_u32(node, "google,chg-update-interval",
				   &profile->update_interval);
	if (ret < 0) {
		pr_err("cannot read chg-update-interval, ret=%d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "google,chg-battery-capacity",
				   &profile->battery_capacity);
	if (ret < 0) {
		pr_err("cannot read chg-battery-capacity, ret=%d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "google,cv-hw-resolution",
				   &profile->cv_hw_resolution);
	if (ret < 0)
		profile->cv_hw_resolution = 25000;

	ret = of_property_read_u32(node, "google,cc-hw-resolution",
				   &profile->cc_hw_resolution);
	if (ret < 0)
		profile->cc_hw_resolution = 25000;

	ret = of_property_read_u32(node, "google,cv-range-accuracy",
				   &profile->cv_range_accuracy);
	if (ret < 0)
		profile->cv_range_accuracy = profile->cv_hw_resolution / 2;

	ret = of_property_read_u32(node, "google,cv-debounce-cnt",
				   &profile->cv_debounce_cnt);
	if (ret < 0)
		profile->cv_debounce_cnt = 3;

	ret = of_property_read_u32(node, "google,cv-update-interval",
				   &profile->cv_update_interval);
	if (ret < 0)
		profile->cv_update_interval = 2000;

	ret = of_property_read_u32(node, "google,cv-tier-ov-cnt",
				   &profile->cv_tier_ov_cnt);
	if (ret < 0)
		profile->cv_tier_ov_cnt = 10;

	profile->temp_nb_limits =
	    of_property_count_elems_of_size(node, "google,chg-temp-limits",
					    sizeof(u32));
	if (profile->temp_nb_limits < 0) {
		pr_err("cannot read chg-temp-limits, ret=%d\n", ret);
		return ret;
	}
	if (profile->temp_nb_limits > CHG_TEMP_NB_LIMITS_MAX) {
		pr_err("chg-temp-nb-limits exceeds driver max: %d\n",
		       CHG_TEMP_NB_LIMITS_MAX);
		return -EINVAL;
	}
	ret = of_property_read_u32_array(node, "google,chg-temp-limits",
					 profile->temp_limits,
					 profile->temp_nb_limits);
	if (ret < 0) {
		pr_err("cannot read chg-temp-limits table, ret=%d\n", ret);
		return ret;
	}

	profile->volt_nb_limits =
	    of_property_count_elems_of_size(node, "google,chg-cv-limits",
					    sizeof(u32));
	if (profile->volt_nb_limits < 0) {
		pr_err("cannot read chg-cv-limits, ret=%d\n", ret);
		return ret;
	}
	if (profile->volt_nb_limits > CHG_VOLT_NB_LIMITS_MAX) {
		pr_err("chg-cv-nb-limits exceeds driver max: %d\n",
		       CHG_VOLT_NB_LIMITS_MAX);
		return -EINVAL;
	}
	ret = of_property_read_u32_array(node, "google,chg-cv-limits",
					 profile->volt_limits,
					 profile->volt_nb_limits);
	if (ret < 0) {
		pr_err("cannot read chg-cv-limits table, ret=%d\n", ret);
		return ret;
	}
	for (vi = 0; vi < profile->volt_nb_limits; vi++)
		profile->volt_limits[vi] = profile->volt_limits[vi] /
		    profile->cv_hw_resolution * profile->cv_hw_resolution;

	cccm_array_size =
	    (profile->temp_nb_limits - 1) * profile->volt_nb_limits;
	profile->cccm_limits = devm_kzalloc(dev,
					    sizeof(s32) * cccm_array_size,
					    GFP_KERNEL);

	ret = of_property_read_u32_array(node, "google,chg-cc-limits",
					 profile->cccm_limits, cccm_array_size);
	if (ret < 0) {
		pr_err("cannot read chg-cc-limits table, ret=%d\n", ret);
		return ret;
	}

	ret = of_property_read_u32(node, "google,chg-cc-tolerance",
				   &profile->chg_cc_tolerance);
	if (ret < 0)
		profile->chg_cc_tolerance = 0;
	else if (profile->chg_cc_tolerance > 250)
		profile->chg_cc_tolerance = 250;

	/* chg-battery-capacity is in mAh, chg-cc-limits relative to 100 */
	for (ti = 0; ti < profile->temp_nb_limits - 1; ti++) {
		for (vi = 0; vi < profile->volt_nb_limits; vi++) {
			ccm = CCCM_LIMITS(profile, ti, vi);
			ccm *= profile->battery_capacity * 10;
			ccm = ccm * (1000 - profile->chg_cc_tolerance) / 1000;
			// round to the nearest resolution the PMIC can handle
			ccm = DIV_ROUND_CLOSEST(ccm, profile->cc_hw_resolution)
					* profile->cc_hw_resolution;
			CCCM_LIMITS(profile, ti, vi) = ccm;
		}
	}

	pr_info("successfully read charging profile:\n");
	dump_profile(profile);

	return 0;
}

static ssize_t show_tier_ovc(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
		chg_drv->chg_profile.cv_tier_ov_cnt);
}

static ssize_t set_tier_ovc(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);
	int ret = 0, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (val < 0)
		return count;

	chg_drv->chg_profile.cv_tier_ov_cnt = val;
	return count;
}

static DEVICE_ATTR(tier_ovc, 0660, show_tier_ovc, set_tier_ovc);

static ssize_t show_cv_update_interval(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
		chg_drv->chg_profile.cv_update_interval);
}

static ssize_t set_cv_update_interval(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);
	int ret = 0, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if (val < 1500)
		return count;

	chg_drv->chg_profile.cv_update_interval = val;
	return count;
}

static DEVICE_ATTR(cv_update_interval, 0660,
		   show_cv_update_interval,
		   set_cv_update_interval);

static ssize_t show_cv_range_accuracy(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n",
		chg_drv->chg_profile.cv_range_accuracy);
}

static ssize_t set_cv_range_accuracy(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);
	int ret = 0, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	chg_drv->chg_profile.cv_range_accuracy = val;
	return count;
}

static DEVICE_ATTR(cv_range_accuracy, 0660,
		   show_cv_range_accuracy,
		   set_cv_range_accuracy);

static ssize_t show_charge_stop_level(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", chg_drv->charge_stop_level);
}

static ssize_t set_charge_stop_level(struct device *dev,
				     struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);
	int ret = 0, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if ((val == chg_drv->charge_stop_level) ||
	    (val <= chg_drv->charge_start_level) ||
	    (val < 0))
		return count;

	chg_drv->charge_stop_level = val;
	power_supply_changed(chg_drv->bat_psy);
	return count;
}

static DEVICE_ATTR(charge_stop_level, 0660,
		   show_charge_stop_level, set_charge_stop_level);

static ssize_t
show_charge_start_level(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);

	return scnprintf(buf, PAGE_SIZE, "%d\n", chg_drv->charge_start_level);
}

static ssize_t set_charge_start_level(struct device *dev,
					struct device_attribute *attr,
					const char *buf, size_t count)
{
	struct chg_drv *chg_drv = dev_get_drvdata(dev);
	int ret = 0, val;

	ret = kstrtoint(buf, 0, &val);
	if (ret < 0)
		return ret;

	if ((val == chg_drv->charge_start_level) ||
	    (val >= chg_drv->charge_stop_level) ||
	    (val < 0))
		return count;

	chg_drv->charge_start_level = val;
	power_supply_changed(chg_drv->bat_psy);
	return count;
}

static DEVICE_ATTR(charge_start_level, 0660,
		   show_charge_start_level, set_charge_start_level);

#ifdef CONFIG_DEBUG_FS
/* experimental / debug code */
static int get_chg_mode(void *data, u64 *val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	*val = chg_drv->chg_mode;
	return 0;
}

static int set_chg_mode(void *data, u64 val)
{
	struct chg_drv *chg_drv = (struct chg_drv *)data;

	chg_drv->chg_mode = val;
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(chg_mode_fops, get_chg_mode, set_chg_mode, "%llu\n");

static int init_debugfs(struct chg_drv *chg_drv)
{
	struct dentry *de;

	de = debugfs_create_dir("google_charger", 0);
	if (de) {
		debugfs_create_file("chg_mode", 0644, de,
				   chg_drv, &chg_mode_fops);
	}

	return 0;

}
#else
static int init_debugfs(struct chg_drv *chg_drv) { }
#endif

static void google_charger_init_work(struct work_struct *work)
{
	struct chg_drv *chg_drv = container_of(work, struct chg_drv,
					       init_work.work);
	struct power_supply *chg_psy, *usb_psy, *wlc_psy = NULL, *bat_psy;
	int ret = 0;

	chg_psy = power_supply_get_by_name(chg_drv->chg_psy_name);
	if (!chg_psy) {
		pr_info("failed to get \"%s\" power supply\n",
			chg_drv->chg_psy_name);
		goto retry_init_work;
	}

	bat_psy = power_supply_get_by_name(chg_drv->bat_psy_name);
	if (!bat_psy) {
		pr_info("failed to get \"%s\" power supply\n",
			chg_drv->bat_psy_name);
		power_supply_put(chg_psy);
		goto retry_init_work;
	}

	usb_psy = power_supply_get_by_name("usb");
	if (!usb_psy) {
		pr_info("failed to get \"usb\" power supply\n");
		power_supply_put(chg_psy);
		power_supply_put(bat_psy);
		goto retry_init_work;
	}

	if (chg_drv->wlc_psy_name) {
		wlc_psy = power_supply_get_by_name(chg_drv->wlc_psy_name);
		if (!wlc_psy) {
			pr_info("failed to get \"%s\" power supply\n",
				chg_drv->wlc_psy_name);
			power_supply_put(chg_psy);
			power_supply_put(bat_psy);
			power_supply_put(usb_psy);
			goto retry_init_work;
		}
	}

	chg_drv->chg_psy = chg_psy;
	chg_drv->wlc_psy = wlc_psy;
	chg_drv->usb_psy = usb_psy;
	chg_drv->bat_psy = bat_psy;

	chg_drv->charge_stop_level = DEFAULT_CHARGE_STOP_LEVEL;
	chg_drv->charge_start_level = DEFAULT_CHARGE_START_LEVEL;

	reset_chg_drv_state(chg_drv);

	chg_drv->psy_nb.notifier_call = psy_changed;
	ret = power_supply_reg_notifier(&chg_drv->psy_nb);
	if (ret < 0)
		pr_err("Cannot register power supply notifer, ret=%d\n", ret);

	wakeup_source_init(&chg_drv->chg_ws, "google-charger");
	pr_info("google_charger_init_work done\n");
	return;

retry_init_work:
	schedule_delayed_work(&chg_drv->init_work,
			      msecs_to_jiffies(CHG_DELAY_INIT_MS));
}

static int google_charger_probe(struct platform_device *pdev)
{
	const char *chg_psy_name, *bat_psy_name, *wlc_psy_name = NULL;
	struct chg_drv *chg_drv;
	int ret;

	chg_drv = devm_kzalloc(&pdev->dev, sizeof(*chg_drv), GFP_KERNEL);
	if (!chg_drv)
		return -ENOMEM;

	chg_drv->device = &pdev->dev;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,chg-power-supply",
				      &chg_psy_name);
	if (ret != 0) {
		pr_err("cannot read google,chg-power-supply, ret=%d\n", ret);
		return -EINVAL;
	}
	chg_drv->chg_psy_name =
	    devm_kstrdup(&pdev->dev, chg_psy_name, GFP_KERNEL);
	if (!chg_drv->chg_psy_name)
		return -ENOMEM;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,bat-power-supply",
				      &bat_psy_name);
	if (ret != 0) {
		pr_err("cannot read google,bat-power-supply, ret=%d\n", ret);
		return -EINVAL;
	}
	chg_drv->bat_psy_name =
	    devm_kstrdup(&pdev->dev, bat_psy_name, GFP_KERNEL);
	if (!chg_drv->bat_psy_name)
		return -ENOMEM;

	ret = of_property_read_string(pdev->dev.of_node,
				      "google,wlc-power-supply",
				      &wlc_psy_name);
	if (ret != 0)
		pr_warn("google,wlc-power-supply not defined\n");
	if (wlc_psy_name) {
		chg_drv->wlc_psy_name =
		    devm_kstrdup(&pdev->dev, wlc_psy_name, GFP_KERNEL);
		if (!chg_drv->wlc_psy_name)
			return -ENOMEM;
	}

	ret = chg_init_chg_profile(chg_drv);
	if (ret < 0) {
		pr_err("cannot read charging profile from dt, ret=%d\n", ret);
		return ret;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_charge_stop_level);
	if (ret != 0) {
		pr_err("Failed to create charge_stop_level files, ret=%d\n",
		       ret);
		return ret;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_charge_start_level);
	if (ret != 0) {
		pr_err("Failed to create charge_start_level files, ret=%d\n",
		       ret);
		return ret;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_cv_range_accuracy);
	if (ret != 0) {
		pr_err("Failed to create cv_range_accuracy files, ret=%d\n",
		       ret);
		return ret;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_cv_update_interval);
	if (ret != 0) {
		pr_err("Failed to create cv_update_interval files, ret=%d\n",
			ret);
		return ret;
	}

	ret = device_create_file(&pdev->dev, &dev_attr_tier_ovc);
	if (ret != 0) {
		pr_err("Failed to create tier_ovc files, ret=%d\n", ret);
		return ret;
	}

	/* debug */
	init_debugfs(chg_drv);

	INIT_DELAYED_WORK(&chg_drv->init_work, google_charger_init_work);
	INIT_DELAYED_WORK(&chg_drv->chg_work, chg_work);
	platform_set_drvdata(pdev, chg_drv);

	schedule_delayed_work(&chg_drv->init_work,
			      msecs_to_jiffies(CHG_DELAY_INIT_MS));

	return 0;
}

static int google_charger_remove(struct platform_device *pdev)
{
	struct chg_drv *chg_drv = platform_get_drvdata(pdev);

	if (chg_drv) {
		power_supply_put(chg_drv->chg_psy);
		power_supply_put(chg_drv->bat_psy);
		power_supply_put(chg_drv->usb_psy);
		if (chg_drv->wlc_psy)
			power_supply_put(chg_drv->wlc_psy);
		wakeup_source_trash(&chg_drv->chg_ws);
	}

	return 0;
}

static const struct of_device_id match_table[] = {
	{.compatible = "google,charger"},
	{},
};

static struct platform_driver charger_driver = {
	.driver = {
		   .name = "google,charger",
		   .owner = THIS_MODULE,
		   .of_match_table = match_table,
		   },
	.probe = google_charger_probe,
	.remove = google_charger_remove,
};

static int __init google_charger_init(void)
{
	int ret;

	ret = platform_driver_register(&charger_driver);
	if (ret < 0) {
		pr_err("device registration failed: %d\n", ret);
		return ret;
	}
	return 0;
}

static void __init google_charger_exit(void)
{
	platform_driver_unregister(&charger_driver);
	pr_info("unregistered platform driver\n");
}

module_init(google_charger_init);
module_exit(google_charger_exit);
MODULE_DESCRIPTION("Multi-step battery charger driver");
MODULE_AUTHOR("Thierry Strudel <tstrudel@google.com>");
MODULE_LICENSE("GPL");
