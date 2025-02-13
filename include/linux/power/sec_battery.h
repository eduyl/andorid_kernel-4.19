/*
 * sec_battery.h
 * Samsung Mobile Battery Header
 *
 *
 * Copyright (C) 2012 Samsung Electronics, Inc.
 *
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#ifndef __SEC_BATTERY_H
#define __SEC_BATTERY_H __FILE__

#include <linux/power/sec_charging_common.h>
#include <linux/of_gpio.h>
#if defined(ANDROID_ALARM_ACTIVATED)
#include <linux/android_alarm.h>
#else
#include <linux/alarmtimer.h>
#endif
#include <linux/workqueue.h>
#include <linux/proc_fs.h>
#include <linux/jiffies.h>

#if defined(CONFIG_MUIC_NOTIFIER)
#include <linux/muic/muic.h>
#include <linux/muic/muic_notifier.h>
#endif /* CONFIG_MUIC_NOTIFIER */

#include <linux/sec_batt.h>

#if defined(CONFIG_BATTERY_SWELLING)
#define BATT_SWELLING_HIGH_TEMP_BLOCK			500
#define BATT_SWELLING_HIGH_TEMP_RECOV			450
#define BATT_SWELLING_LOW_TEMP_BLOCK			50
#define BATT_SWELLING_LOW_TEMP_RECOV			100
#define BATT_SWELLING_CHG_CURRENT				1400
#define BATT_SWELLING_NORMAL_VOLTAGE			4400
#define BATT_SWELLING_HIGH_RECHG_VOLTAGE		4150
#define BATT_SWELLING_LOW_RECHG_VOLTAGE			4050
#define BATT_SWELLING_DROP_VOLTAGE				4200
#define BATT_SWELLING_BLOCK_TIME		10 * 60 /* 10 min */
#endif

#if defined(CONFIG_CHARGING_VZWCONCEPT)
#define STORE_MODE_CHARGING_MAX 35
#define STORE_MODE_CHARGING_MIN 30
#else
#define STORE_MODE_CHARGING_MAX 70
#define STORE_MODE_CHARGING_MIN 60
#endif

#define ADC_CH_COUNT		10
#define ADC_SAMPLE_COUNT	10

struct adc_sample_info {
	unsigned int cnt;
	int total_adc;
	int average_adc;
	int adc_arr[ADC_SAMPLE_COUNT];
	int index;
};

struct sec_battery_info {
	struct device *dev;
	sec_battery_platform_data_t *pdata;
	/* power supply used in Android */
	struct power_supply *psy_bat;
	struct power_supply *psy_usb;
	struct power_supply *psy_ac;
	struct power_supply *psy_wireless;
	struct power_supply *psy_ps;
	unsigned int irq;

	struct notifier_block batt_nb;

	int status;
	int health;
	bool present;

	int voltage_now;		/* cell voltage (mV) */
	int voltage_avg;		/* average voltage (mV) */
	int voltage_ocv;		/* open circuit voltage (mV) */
	int current_now;		/* current (mA) */
	int inbat_adc;                  /* inbat adc */
	int current_avg;		/* average current (mA) */
	int current_max;		/* input current limit (mA) */
	int current_adc;

	unsigned int capacity;			/* SOC (%) */

	struct mutex adclock;
	struct adc_sample_info	adc_sample[ADC_CH_COUNT];

	/* keep awake until monitor is done */
	struct wakeup_source *monitor_wake_lock;
	struct workqueue_struct *monitor_wqueue;
	struct delayed_work monitor_work;
#ifdef CONFIG_SAMSUNG_BATTERY_FACTORY
	struct wakeup_source *lpm_wake_lock;
#endif
	unsigned int polling_count;
	unsigned int polling_time;
	bool polling_in_sleep;
	bool polling_short;

	struct delayed_work polling_work;
	struct alarm polling_alarm;
	ktime_t last_poll_time;

	/* event set */
	unsigned int event;
	unsigned int event_wait;

	struct alarm event_termination_alarm;

	ktime_t	last_event_time;

	/* battery check */
	unsigned int check_count;
	/* ADC check */
	unsigned int check_adc_count;
	unsigned int check_adc_value;

	/* time check */
	unsigned long charging_start_time;
	unsigned long charging_passed_time;
	unsigned long charging_next_time;
	unsigned long charging_fullcharged_time;

	/* temperature check */
	int temperature;	/* battery temperature */
	int temper_amb;		/* target temperature */
	int chg_temp;
	int pre_chg_temp;

	int temp_adc;
	int temp_ambient_adc;
	int chg_temp_adc;
	int chg_limit;

	int temp_highlimit_threshold;
	int temp_highlimit_recovery;
	int temp_high_threshold;
	int temp_high_recovery;
	int temp_low_threshold;
	int temp_low_recovery;

	unsigned int temp_highlimit_cnt;
	unsigned int temp_high_cnt;
	unsigned int temp_low_cnt;
	unsigned int temp_recover_cnt;

	/* charging */
	unsigned int charging_mode;
	bool is_recharging;
	bool is_jig_on;
	int cable_type;
	int muic_cable_type;
	int extended_cable_type;
	struct wakeup_source *cable_wake_lock;
	struct work_struct cable_work;
	struct wakeup_source *vbus_wake_lock;
	unsigned int full_check_cnt;
	unsigned int recharge_check_cnt;

	/* wireless charging enable*/
	int wc_enable;
	int wc_status;

	int wire_status;

	/* wearable charging */
	int ps_enable;
	int ps_status;
	int ps_changed;

	/* test mode */
	int test_mode;
	bool factory_mode;
	bool store_mode;
	bool ignore_store_mode;
	bool slate_mode;

	int siop_level;
#if defined(CONFIG_SAMSUNG_BATTERY_ENG_TEST)
	int stability_test;
	int eng_not_full_status;
#endif

	bool charging_block;
#if defined(CONFIG_BATTERY_SWELLING)
	bool swelling_mode;
	unsigned long swelling_block_start;
	unsigned long swelling_block_passed;
	int swelling_full_check_cnt;
#endif
#if defined(CONFIG_AFC_CHARGER_MODE)
	char *hv_chg_name;
#endif
#if defined(CONFIG_BATTERY_SWELLING_SELF_DISCHARGING)
	bool factory_self_discharging_mode_on;
	bool force_discharging;
	bool self_discharging;
	int self_discharging_adc;
#endif
	int cycle;
#if defined(CONFIG_BATTERY_AGE_FORECAST)
	int batt_cycle;
#endif
};

ssize_t sec_bat_show_attrs(struct device *dev,
				struct device_attribute *attr, char *buf);

ssize_t sec_bat_store_attrs(struct device *dev,
				struct device_attribute *attr,
				const char *buf, size_t count);

#define SEC_BATTERY_ATTR(_name)						\
{									\
	.attr = {.name = #_name, .mode = 0664},	\
	.show = sec_bat_show_attrs,					\
	.store = sec_bat_store_attrs,					\
}

/* event check */
#define EVENT_NONE				(0)
#define EVENT_2G_CALL			(0x1 << 0)
#define EVENT_3G_CALL			(0x1 << 1)
#define EVENT_MUSIC				(0x1 << 2)
#define EVENT_VIDEO				(0x1 << 3)
#define EVENT_BROWSER			(0x1 << 4)
#define EVENT_HOTSPOT			(0x1 << 5)
#define EVENT_CAMERA			(0x1 << 6)
#define EVENT_CAMCORDER			(0x1 << 7)
#define EVENT_DATA_CALL			(0x1 << 8)
#define EVENT_WIFI				(0x1 << 9)
#define EVENT_WIBRO				(0x1 << 10)
#define EVENT_LTE				(0x1 << 11)
#define EVENT_LCD			(0x1 << 12)
#define EVENT_GPS			(0x1 << 13)

enum {
	BATT_RESET_SOC = 0,
	BATT_READ_RAW_SOC,
	BATT_READ_ADJ_SOC,
	BATT_TYPE,
	BATT_VFOCV,
	BATT_VOL_ADC,
	BATT_VOL_ADC_CAL,
	BATT_VOL_AVER,
	BATT_VOL_ADC_AVER,

	BATT_CURRENT_UA_NOW,
	BATT_CURRENT_UA_AVG,

	BATT_TEMP,
	BATT_TEMP_ADC,
	BATT_TEMP_AVER,
	BATT_TEMP_ADC_AVER,
	CHG_TEMP,
	CHG_TEMP_ADC,
	BATT_VF_ADC,
	BATT_SLATE_MODE,

	BATT_LP_CHARGING,
	SIOP_ACTIVATED,
	SIOP_LEVEL,
	BATT_CHARGING_SOURCE,
	FG_REG_DUMP,
	FG_RESET_CAP,
	FG_CAPACITY,
	FG_ASOC,
	AUTH,
	CHG_CURRENT_ADC,
	WC_ADC,
	WC_STATUS,
	WC_ENABLE,
	FACTORY_MODE,
	STORE_MODE,
	UPDATE,
	TEST_MODE,

	BATT_EVENT_CALL,
	BATT_EVENT_2G_CALL,
	BATT_EVENT_TALK_GSM,
	BATT_EVENT_3G_CALL,
	BATT_EVENT_TALK_WCDMA,
	BATT_EVENT_MUSIC,
	BATT_EVENT_VIDEO,
	BATT_EVENT_BROWSER,
	BATT_EVENT_HOTSPOT,
	BATT_EVENT_CAMERA,
	BATT_EVENT_CAMCORDER,
	BATT_EVENT_DATA_CALL,
	BATT_EVENT_WIFI,
	BATT_EVENT_WIBRO,
	BATT_EVENT_LTE,
	BATT_EVENT_LCD,
	BATT_EVENT_GPS,
	BATT_EVENT,
	BATT_TEMP_TABLE,
	HV_CHARGER_STATUS,
	HV_CHARGER_SET,
	HV_CHARGER_SUPPORT,
#if defined(CONFIG_SAMSUNG_BATTERY_ENG_TEST)
	BATT_TEST_CHARGE_CURRENT,
	BATT_STABILITY_TEST,
#endif
	BATT_CAPACITY_MAX,
	BATT_INBAT_VOLTAGE,
#if defined(CONFIG_BATTERY_SWELLING_SELF_DISCHARGING)
	BATT_DISCHARGING_CHECK,
	BATT_DISCHARGING_CHECK_ADC,
	BATT_SELF_DISCHARGING_CONTROL,
#endif
#if defined(CONFIG_BATTERY_AGE_FORECAST)
	FG_CYCLE,
	FG_FULL_VOLTAGE,
	FG_FULLCAPNOM,
	BATTERY_CYCLE,
#endif
};

#ifdef CONFIG_OF
extern int adc_read(struct sec_battery_info *battery, int channel);
extern void adc_init(struct platform_device *pdev, struct sec_battery_info *battery);
extern void adc_exit(struct sec_battery_info *battery);
#endif

#endif /* __SEC_BATTERY_H */
