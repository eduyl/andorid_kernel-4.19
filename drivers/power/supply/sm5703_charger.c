/* drivers/battery/sm5703_charger.c
 * SM5703 Charger Driver
 *
 * Copyright (C) 2013 Siliconmitus Technology Corp.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */
//Converting wakelocks https://www.linaro.org/blog/converting-code-implementing-suspend-blockers/
#include <linux/power/sm5703_charger.h>

#ifdef CONFIG_SM5703_MUIC
#include <linux/i2c/sm5703-muic.h>
#endif

#include <linux/mfd/sm5703.h>

#ifdef CONFIG_FLED_SM5703
#include <linux/leds/sm5703_fled.h>
#include <linux/leds/smfled.h>
#endif

#include <linux/version.h>
#include <linux/debugfs.h>
#include <linux/seq_file.h>
#include <linux/of_gpio.h>

#ifdef CONFIG_USB_HOST_NOTIFY
#include <linux/usb_notify.h>
#endif

#define EN_NOBAT_IRQ		0
#define EN_DONE_IRQ			1
#define EN_TOPOFF_IRQ		1
#define EN_CHGON_IRQ		0
#define EN_OTGFAIL_IRQ		1
#define EN_VBUSLIMIT_IRQ	0
#define EN_AICL_IRQ			1

#define MINVAL(a, b) ((a <= b) ? a : b)

#ifndef EN_TEST_READ
#define EN_TEST_READ 1
#endif

static int sm5703_reg_map[] = {
	SM5703_INTMSK1,
	SM5703_INTMSK2,
	SM5703_INTMSK3,
	SM5703_INTMSK4,
	SM5703_STATUS1,
	SM5703_STATUS2,
	SM5703_STATUS3,
	SM5703_STATUS4,
	SM5703_CNTL,		
	SM5703_VBUSCNTL,
	SM5703_CHGCNTL1,
	SM5703_CHGCNTL2,
	SM5703_CHGCNTL3,
	SM5703_CHGCNTL4,
	SM5703_CHGCNTL5,
	SM5703_CHGCNTL6,
	SM5703_OTGCURRENTCNTL,
	SM5703_Q3LIMITCNTL,
	SM5703_STATUS5,
};

typedef struct sm5703_charger_data {
	struct i2c_client	*client;
	sm5703_mfd_chip_t	*sm5703;
	struct power_supply	psy_chg;
	struct power_supply	psy_otg;
	sm5703_charger_platform_data_t *pdata;
	int charging_current;
	struct wakeup_source *vbuslimit_wake_lock;
	struct delayed_work vbuslimit_work;
	int	current_max;
	bool is_current_reduced;
	int siop_level;
	int cable_type;
	bool is_charging;
	struct mutex io_lock;
	/* register programming */
	int reg_addr;
	int reg_data;
	int nchgen;

	bool full_charged;
	bool ovp;
	bool is_mdock;
	struct workqueue_struct *wq;
#if EN_OTGFAIL_IRQ	
	struct wakeup_source *otg_fail_wake_lock;
	struct work_struct otg_fail_work;
#endif
	int status;
#ifdef CONFIG_FLED_SM5703
	struct sm_fled_info *fled_info;
#endif
} sm5703_charger_data_t;

static enum power_supply_property sec_charger_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_CHARGE_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_CURRENT_AVG,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL,
	POWER_SUPPLY_PROP_CHARGING_ENABLED,
};
static enum power_supply_property sm5703_otg_props[] = {
	POWER_SUPPLY_PROP_ONLINE,
};

int otg_enable_flag;

static int sm5703_get_charging_health(
		struct sm5703_charger_data *charger);
static int sm5703_get_charging_status(struct sm5703_charger_data *charger);
static void sm5703_read_regs(struct i2c_client *i2c, char *str)
{
	u8 data = 0;
	int i = 0;
	for (i = SM5703_INTMSK1; i < ARRAY_SIZE(sm5703_reg_map); i++) {
		data = sm5703_reg_read(i2c, sm5703_reg_map[i]);
		sprintf(str+strlen(str), "0x%02x, ", data);
	}
}

static void sm5703_test_read(struct i2c_client *i2c)
{
	int data;
	char str[1000] = {0,};
	int i;

	/* SM5703 REG: 0x04 ~ 0x13 */
	for (i = SM5703_INTMSK1; i <= SM5703_CHGCNTL6; i++) {
		data = sm5703_reg_read(i2c, i);
		sprintf(str+strlen(str), "0x%0x = 0x%02x, ", i, data);
	}

	sprintf(str+strlen(str), "0x%0x = 0x%02x, ",SM5703_OTGCURRENTCNTL,
		sm5703_reg_read(i2c, SM5703_OTGCURRENTCNTL));
	sprintf(str+strlen(str), "0x%0x = 0x%02x, ", SM5703_STATUS5,
		sm5703_reg_read(i2c, SM5703_STATUS5));
	sprintf(str+strlen(str), "0x%0x = 0x%02x, ", SM5703_Q3LIMITCNTL,
		sm5703_reg_read(i2c, SM5703_Q3LIMITCNTL));
	pr_info("%s: %s\n", __func__, str);
}

#define SM5703_FLEDCNTL6			0x19
static void sm5703_charger_otg_control(struct sm5703_charger_data *charger,
		bool enable)
{
	pr_info("%s: called charger otg control : %s\n", __func__,
			enable ? "on" : "off");

	otg_enable_flag = enable;

	if (!enable) {
		sm5703_assign_bits(charger->sm5703->i2c_client,
			SM5703_FLEDCNTL6, SM5703_BSTOUT_MASK,
			SM5703_BSTOUT_4P5);
#ifdef CONFIG_FLED_SM5703
		/* turn off OTG */
		if (charger->fled_info == NULL)
			charger->fled_info = sm_fled_get_info_by_name(NULL);
		if (charger->fled_info)
			sm5703_boost_notification(charger->fled_info, 0);
#else
		sm5703_assign_bits(charger->sm5703->i2c_client,
			SM5703_CNTL, SM5703_OPERATION_MODE_MASK,
			SM5703_OPERATION_MODE_CHARGING_ON);
#endif
	} else {
		sm5703_assign_bits(charger->sm5703->i2c_client,
			SM5703_FLEDCNTL6, SM5703_BSTOUT_MASK,
			SM5703_BSTOUT_5P0);
#ifdef CONFIG_FLED_SM5703
		if (charger->fled_info == NULL)
			charger->fled_info = sm_fled_get_info_by_name(NULL);
		if (charger->fled_info)
			sm5703_boost_notification(charger->fled_info, 1);
#else
		sm5703_assign_bits(charger->sm5703->i2c_client,
			SM5703_CNTL, SM5703_OPERATION_MODE_MASK,
			SM5703_OPERATION_MODE_USB_OTG_MODE);
#endif
		charger->cable_type = POWER_SUPPLY_TYPE_OTG;
	}
}


static void sm5703_enable_charger_switch(struct sm5703_charger_data *charger,
		int onoff)
{
	bool prev_charging_status = charger->is_charging;
	
	charger->is_charging = onoff ? true : false;
	if (onoff > 0 && (prev_charging_status == false)) {
		pr_info("%s: turn on charger\n", __func__);
#ifdef CONFIG_FLED_SM5703
		if (charger->fled_info == NULL)
			charger->fled_info = sm_fled_get_info_by_name(NULL);
		if (charger->fled_info)
			sm5703_charger_notification(charger->fled_info,1);
#endif

#ifndef CONFIG_FLED_SM5703
		sm5703_assign_bits(charger->sm5703->i2c_client,
			SM5703_CNTL, SM5703_OPERATION_MODE_MASK,
			SM5703_OPERATION_MODE_CHARGING_ON);
#endif

		charger->nchgen = false;
		gpio_direction_output(charger->pdata->chgen_gpio,
			charger->nchgen); //nCHG enable


		pr_info("%s : STATUS OF CHARGER ON(0)/OFF(1): %d\n",
			__func__, charger->nchgen);
	} else if(onoff == 0){
		charger->full_charged = false;
		pr_info("%s: turn off charger\n", __func__);

		charger->nchgen = true;
#ifdef CONFIG_FLED_SM5703
		if (charger->fled_info == NULL)
			charger->fled_info = sm_fled_get_info_by_name(NULL);
		if (charger->fled_info)
			sm5703_charger_notification(charger->fled_info,0);
#endif
		gpio_direction_output(charger->pdata->chgen_gpio,
			charger->nchgen); //nCHG disable
		pr_info("%s : STATUS OF CHARGER ON(0)/OFF(1): %d\n",
			__func__, charger->nchgen);
	}
	else
		pr_info("%s: repeated to set charger switch(%d), prev stat = %d\n",
             __func__, onoff, prev_charging_status ? 1 : 0);
}

static void sm5703_enable_autostop(struct sm5703_charger_data *charger,
		int onoff)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;

	pr_info("%s:[BATT] Autostop set(%d)\n", __func__, onoff);

	mutex_lock(&charger->io_lock);

	if (onoff)
		sm5703_set_bits(i2c, SM5703_CHGCNTL4, SM5703_AUTOSTOP_MASK);
	else
		sm5703_clr_bits(i2c, SM5703_CHGCNTL4, SM5703_AUTOSTOP_MASK);

	mutex_unlock(&charger->io_lock);    
}

static int sm5703_set_topoff_timer(struct sm5703_charger_data *charger,
				unsigned int topoff_timer)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	sm5703_assign_bits(i2c,
		SM5703_CHGCNTL5, SM5703_TOPOFF_TIMER_MASK,
		((topoff_timer & SM5703_TOPOFF_TIMER) << SM5703_TOPOFF_TIMER_SHIFT));
	pr_info("%s : TOPOFF timer set (timer=0x%x)\n",
		__func__, topoff_timer);

	return 0;
}

static void sm5703_enable_autoset(struct sm5703_charger_data *charger,
		int onoff)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;

	pr_info("%s:[BATT] Autoset set(%d)\n", __func__, onoff);

	mutex_lock(&charger->io_lock);

	if (onoff)
		sm5703_set_bits(i2c, SM5703_CNTL, SM5703_AUTOSET_MASK);
	else
		sm5703_clr_bits(i2c, SM5703_CNTL, SM5703_AUTOSET_MASK);

	mutex_unlock(&charger->io_lock);
}

static void sm5703_enable_aiclen(struct sm5703_charger_data *charger,
		int onoff)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;

	pr_info("%s:[BATT] AICLEN set(%d)\n", __func__, onoff);

	mutex_lock(&charger->io_lock);

	if (onoff)
		sm5703_set_bits(i2c, SM5703_CHGCNTL5, SM5703_AICLEN_MASK);
	else
		sm5703_clr_bits(i2c, SM5703_CHGCNTL5, SM5703_AICLEN_MASK);

	mutex_unlock(&charger->io_lock);    
}

static void sm5703_set_aiclth(struct sm5703_charger_data *charger,
		int aiclth)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	int data = 0, temp = 0;

	mutex_lock(&charger->io_lock);
	data = sm5703_reg_read(i2c, SM5703_CHGCNTL5);
	data &= ~SM5703_AICLTH;

	if (aiclth >= 4900)
		aiclth = 4900;

	if(aiclth >= 4300) {
		temp = (aiclth - 4300)/100;
		data |= temp;
	}

	sm5703_reg_write(i2c, SM5703_CHGCNTL5, data);

	data = sm5703_reg_read(i2c, SM5703_CHGCNTL5);
	pr_info("%s : SM5703_CHGCNTL5 (AICHTH) : 0x%02x\n",
			__func__, data);    
	mutex_unlock(&charger->io_lock);
}

#if EN_AICL_IRQ
static void sm5703_set_aicl_irq(struct sm5703_charger_data *charger,
			int mask)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	int data = 0;

	mutex_lock(&charger->io_lock);
	data = sm5703_reg_read(i2c, SM5703_INTMSK1);
	data &= 0xFE;

	if (mask)
		data |= 0x01;

	sm5703_reg_write(i2c, SM5703_INTMSK1, data);

	data = sm5703_reg_read(i2c, SM5703_INTMSK1);
	pr_info("%s : SM5703_INTMSK1 (AICH-MASK) : 0x%02x, mask : %d\n",
			__func__, data, mask);
	mutex_unlock(&charger->io_lock);
}
#endif

static void sm5703_set_freqsel(struct sm5703_charger_data *charger,
		int freqsel_hz)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	int data = 0;

	mutex_lock(&charger->io_lock);
	data = sm5703_reg_read(i2c, SM5703_CHGCNTL6);
	data &= ~SM5703_FREQSEL_MASK;
	data |= (freqsel_hz << SM5703_FREQSEL_SHIFT);

	sm5703_reg_write(i2c, SM5703_CHGCNTL6, data);

	data = sm5703_reg_read(i2c, SM5703_CHGCNTL6);
	pr_info("%s : SM5703_CHGCNTL6 (FREQSEL) : 0x%02x\n",
			__func__, data);    
	mutex_unlock(&charger->io_lock);
}

static void sm5703_set_input_current_limit(struct sm5703_charger_data *charger,
		int current_limit)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	int data = 0, temp = 0;

	mutex_lock(&charger->io_lock);
	data = sm5703_reg_read(i2c, SM5703_VBUSCNTL);
	data &= ~SM5703_VBUSLIMIT;

	if (charger->siop_level < 100 && current_limit >= SIOP_INPUT_LIMIT_CURRENT)
		current_limit = SIOP_INPUT_LIMIT_CURRENT;

	if (current_limit >= 2100)
		current_limit = 2100;

	if (charger->current_max < current_limit && charger->is_current_reduced) {
		pr_info("%s: skip set input current limit(%d <--> %d)\n",
			__func__, charger->current_max, current_limit);
	} else {
		if (current_limit > 100) {
			temp = ((current_limit - 100) / 50) | data;
			sm5703_reg_write(i2c, SM5703_VBUSCNTL, temp);
		}

		data = sm5703_reg_read(i2c, SM5703_VBUSCNTL);
		pr_info("%s : SM5703_VBUSCNTL (Input current limit) : 0x%02x\n",
				__func__, data);

		if (charger->pdata->chg_vbuslimit
#if EN_AICL_IRQ
			/* check aicl state */
			&& (sm5703_reg_read(i2c, SM5703_STATUS1) & 0x01)
#endif
			) {
			/* start vbuslimit work */
			__pm_stay_awake(charger->vbuslimit_wake_lock);
			queue_delayed_work_on(0, charger->wq,
				&charger->vbuslimit_work, msecs_to_jiffies(START_VBUSLIMIT_DELAY));
		}
	}

	mutex_unlock(&charger->io_lock);
}

static int sm5703_get_input_current_limit(struct i2c_client *i2c)
{
	int ret, current_limit = 0;
	ret = sm5703_reg_read(i2c, SM5703_VBUSCNTL);
	if (ret < 0)
		return ret;
	ret&=SM5703_VBUSLIMIT_MASK;

	current_limit = (100 + (ret*50));

	return current_limit;
}

static void sm5703_set_regulation_voltage(struct sm5703_charger_data *charger,
		int float_voltage)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	int data = 0;

	data = sm5703_reg_read(i2c, SM5703_CHGCNTL3);

	data &= ~SM5703_BATREG_MASK;

	if ((float_voltage) <= 4120)
		data = 0x00;
	else if ((float_voltage) >= 4430)
		data = 0x1f;
	else
		data = ((float_voltage - 4120) / 10);

	mutex_lock(&charger->io_lock);
	sm5703_reg_write(i2c, SM5703_CHGCNTL3, data);
	data = sm5703_reg_read(i2c, SM5703_CHGCNTL3);
	pr_info("%s : SM5703_CHGCNTL3 (Battery regulation voltage) : 0x%02x\n",
			__func__, data);
	mutex_unlock(&charger->io_lock);
}

#if defined(CONFIG_BATTERY_SWELLING) || defined(CONFIG_BATTERY_SWELLING_SELF_DISCHARGING)
static int sm5703_get_regulation_voltage(struct sm5703_charger_data *charger)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	int data = 0;

	data = sm5703_reg_read(i2c, SM5703_CHGCNTL3);
	data &= SM5703_BATREG_MASK;

	return (4120 + (data * 10));
}
#endif

static void __sm5703_set_fast_charging_current(struct i2c_client *i2c,
		int charging_current)
{
	int data = 0;

	if(charging_current <= 100)
		charging_current = 100;
	else if (charging_current >= 2500)
		charging_current = 2500;

	data = (charging_current - 100) / 50;

	sm5703_reg_write(i2c, SM5703_CHGCNTL2, data);

	data = sm5703_reg_read(i2c, SM5703_CHGCNTL2);
	pr_info("%s : SM5703_CHGCNTL2 (fastchg current) : 0x%02x\n",
			__func__, data);

}

static int sm5703_get_fast_charging_current(struct i2c_client *i2c)
{
	int data = sm5703_reg_read(i2c, SM5703_CHGCNTL2);
	int charging_current = 0;

	if (data < 0)
		return data;

	data &= SM5703_FASTCHG_MASK;

	charging_current = (100 + (data*50));

	return charging_current;
}

static int sm5703_get_current_topoff_setting(struct sm5703_charger_data *charger)
{
	int ret, data = 0, topoff_current = 0;
	mutex_lock(&charger->io_lock);
	ret = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_CHGCNTL4);
	mutex_unlock(&charger->io_lock);
	if (ret < 0) {
		pr_info("%s: warning --> fail to read i2c register(%d)\n", __func__, ret);
		return ret;
	}

	data = ((ret & SM5703_TOPOFF_MASK) >> SM5703_TOPOFF_SHIFT);  

	topoff_current = (100 + (data*25));

	return topoff_current;
}

static void __sm5703_set_termination_current_limit(struct i2c_client *i2c,
		int current_limit)
{
	int data = 0, temp = 0;

	pr_info("%s : Set Termination\n", __func__);

	data = sm5703_reg_read(i2c, SM5703_CHGCNTL4);

	data &= ~SM5703_TOPOFF_MASK;

	if(current_limit <= 100)
		current_limit = 100;
	else if (current_limit >= 475)
		current_limit = 475;

	temp = (current_limit - 100) / 25;
	data |= (temp << SM5703_TOPOFF_SHIFT);

	sm5703_reg_write(i2c, SM5703_CHGCNTL4, data);

	data = sm5703_reg_read(i2c, SM5703_CHGCNTL4);
	pr_info("%s : SM5703_CHGCNTL4 (Top-off current threshold) : 0x%02x\n",
			__func__, data);    
}

static void sm5703_set_charging_current(struct sm5703_charger_data *charger,
		int topoff, int reset_topoff)
{
	int adj_current = 0;

	adj_current = charger->charging_current * charger->siop_level / 100;
#if 0
#if CONFIG_SIOP_CHARGING_LIMIT_CURRENT
	if(charger->siop_level < 100 && adj_current > CONFIG_SIOP_CHARGING_LIMIT_CURRENT)
		adj_current = CONFIG_SIOP_CHARGING_LIMIT_CURRENT;
#endif
#endif
	pr_info("%s adj_current = %dmA charger->siop_level = %d\n",__func__, adj_current,charger->siop_level);
	mutex_lock(&charger->io_lock);
	__sm5703_set_fast_charging_current(charger->sm5703->i2c_client,
			adj_current);
	if(reset_topoff)
		__sm5703_set_termination_current_limit(
				charger->sm5703->i2c_client, topoff);
	mutex_unlock(&charger->io_lock);
}

static void sm5703_set_otgcurrent(struct sm5703_charger_data *charger,
		int otg_current)
{
	struct i2c_client *i2c = charger->sm5703->i2c_client;
	int data = 0;

	data = sm5703_reg_read(i2c, SM5703_OTGCURRENTCNTL);

	data &= ~SM5703_OTGCURRENT_MASK;

	if (otg_current <= 500)
		data = 0x00;
	else if (otg_current <= 700)
		data = 0x01;
	else if (otg_current <= 900)
		data = 0x02;    
	else
		data = 0x3;

	mutex_lock(&charger->io_lock);
	sm5703_reg_write(i2c, SM5703_OTGCURRENTCNTL, data);
	data = sm5703_reg_read(i2c, SM5703_OTGCURRENTCNTL);
	pr_info("%s : SM5703_OTGCURRENTCNTL (OTG current) : 0x%02x\n",
			__func__, data);
	mutex_unlock(&charger->io_lock);
}

static void sm5703_set_bst_iq3limit(struct sm5703_charger_data *charger,
		int iq3limit)
{
	int data = 0;
	int iq3limit_data = 0;

	if(iq3limit == 0)
		iq3limit_data = SM5703_BST_IQ3LIMIT_0P7X;
	else if(iq3limit == 1)
		iq3limit_data = SM5703_BST_IQ3LIMIT_1X;
	else{
		pr_info("%s : Unknown iq3limit! - %d\n", __func__, iq3limit);
	}

	mutex_lock(&charger->io_lock);
	data = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_Q3LIMITCNTL);
	data &= ~SM5703_BST_IQ3LIMIT_MASK;
	data |= (iq3limit_data << SM5703_BST_IQ3LIMIT_SHIFT);

	sm5703_reg_write(charger->sm5703->i2c_client, SM5703_Q3LIMITCNTL, data);

	data = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_Q3LIMITCNTL);
	pr_info("%s : SM5703_Q3LIMITCNTL (BST_IQ3LIMIT) : 0x%02x\n",
			__func__, data);
	mutex_unlock(&charger->io_lock);
}

static void sm5703_configure_charger(struct sm5703_charger_data *charger)
{

	int topoff;
	union power_supply_propval val, chg_now, swelling_state;
	int full_check_type;

	pr_info("%s : Set config charging\n", __func__);
	if (charger->charging_current < 0) {
		pr_info("%s : OTG is activated. Ignore command!\n",
				__func__);
		return;
	}

	psy_do_property("battery", get,
			POWER_SUPPLY_PROP_CHARGE_NOW, val);

	/* Input current limit */
	pr_info("%s : input current (%dmA)\n",
			__func__, charger->pdata->charging_current_table
			[charger->cable_type].input_current_limit);
	sm5703_set_input_current_limit(charger,
			charger->pdata->charging_current_table
			[charger->cable_type].input_current_limit);

	/* Float voltage */
	pr_info("%s : float voltage (%dmV)\n",
			__func__, charger->pdata->chg_float_voltage);
	sm5703_set_regulation_voltage(charger,
			charger->pdata->chg_float_voltage);

	/* Fast charge and Termination current */
	topoff = charger->pdata->charging_current_table
		[charger->cable_type].full_check_current_1st;

	psy_do_property("battery", get,
			POWER_SUPPLY_PROP_CHARGE_NOW, chg_now);

	if (chg_now.intval == SEC_BATTERY_CHARGING_1ST)
		full_check_type = charger->pdata->full_check_type;
	else
		full_check_type = charger->pdata->full_check_type_2nd;

	switch (full_check_type) {
		case SEC_BATTERY_FULLCHARGED_CHGPSY:
		case SEC_BATTERY_FULLCHARGED_FG_CURRENT:
#if defined(CONFIG_BATTERY_SWELLING)
			psy_do_property("battery", get,
					POWER_SUPPLY_PROP_CHARGE_CONTROL_LIMIT, swelling_state);
#else
			swelling_state.intval = 0;
#endif
			if (chg_now.intval == SEC_BATTERY_CHARGING_1ST && (!swelling_state.intval)) {
				pr_info("%s : termination current (%dmA)\n",
						__func__, charger->pdata->charging_current_table[
						charger->cable_type].full_check_current_1st);

				/** Setting 1st termination current as charger termination current*/
				topoff = charger->pdata->charging_current_table
					[charger->cable_type].full_check_current_1st;
			} else {
				pr_info("%s : termination current (%dmA)\n",
						__func__, charger->pdata->charging_current_table[
						charger->cable_type].full_check_current_2nd);

				if (sm5703_get_charging_status(charger) == POWER_SUPPLY_STATUS_FULL) {
					sm5703_enable_charger_switch(charger, 0);
					charger->charging_current = charger->pdata->charging_current_table
						[charger->cable_type].fast_charging_current;
				}
				/** Setting 2nd termination current as new charger termination current*/
				topoff = charger->pdata->charging_current_table
					[charger->cable_type].full_check_current_2nd;
			}
			break;
	}
	pr_info("%s : fast charging current (%dmA), topoff current (%dmA)\n",
			__func__, charger->charging_current, topoff);
	if(swelling_state.intval)
		sm5703_set_charging_current(charger, topoff, 0);
	else
		sm5703_set_charging_current(charger, topoff, 1);

	/* Charging Enable/Disable. */
	/* sm5703_enable_charger_switch(charger, 1); */
}

/* here is set init charger data */
static bool sm5703_chg_init(struct sm5703_charger_data *charger)
{
	sm5703_mfd_chip_t *chip = i2c_get_clientdata(charger->sm5703->i2c_client);
	int ret;
	chip->charger = charger;
	charger->full_charged = false;

	/* AUTOSTOP */
	sm5703_enable_autostop(chip->charger, (int)charger->pdata->chg_autostop);
	/* AUTOSET */
	sm5703_enable_autoset(chip->charger, (int)charger->pdata->chg_autoset);
	/* AICLEN */
	sm5703_enable_aiclen(chip->charger, (int)charger->pdata->chg_aiclen);
	/* AICLTH */
	sm5703_set_aiclth(chip->charger, (int)charger->pdata->chg_aiclth);
	/* FREQSEL */
	sm5703_set_freqsel(chip->charger, SM5703_FREQSEL_1P5MHZ);

	/* TOP-OFF Timer */
	if(charger->pdata->chg_autostop)
		sm5703_set_topoff_timer(charger, SM5703_TOPOFF_TIMER_30m);

	/* MUST set correct regulation voltage first
	 * Before MUIC pass cable type information to charger
	 * charger would be already enabled (default setting)
	 * it might cause EOC event by incorrect regulation voltage */
	sm5703_set_regulation_voltage(charger,
			charger->pdata->chg_float_voltage);

	sm5703_set_otgcurrent(charger, charger->pdata->otg_current); /* OTGCURRENT : 1.2A(Default) and 0.9A(Special Case) */

	sm5703_set_bst_iq3limit(charger, (int)charger->pdata->bst_iq3limit);

	ret = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_STATUS3);
	if (ret < 0)
		pr_info("Error : can't get charging status (%d)\n", ret);

	if (ret & SM5703_STATUS3_TOPOFF) {
		pr_info("%s: W/A Charger already topoff state. Charger Off\n", 
			__func__);
		sm5703_enable_charger_switch(charger, 0);
		msleep(100);
		sm5703_enable_charger_switch(charger, 1);
	}

	sm5703_test_read(charger->sm5703->i2c_client);

	return true;
}


static int sm5703_get_charging_status(struct sm5703_charger_data *charger)
{
	int status = POWER_SUPPLY_STATUS_UNKNOWN;
	int chg_status3,chg_status5;
	int ret;
	int nCHG = 0;

	chg_status3 = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_STATUS3);
	if (chg_status3<0) {
		pr_info("Error : SM5703_STATUS3 can't get charging status (%d)\n", chg_status3);
	}
	pr_info("%s chg_status3 = %d \n",__func__, chg_status3);

	chg_status5 = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_STATUS5);
	if (chg_status5<0) {
		pr_info("Error : SM5703_STATUS5 can't get charging status (%d)\n", chg_status5);
	}

	pr_info("%s charger->full_charged = %d, charger->cable_type = %d \n",__func__,charger->full_charged,charger->cable_type);

	nCHG = gpio_get_value(charger->pdata->chgen_gpio);

	if ((chg_status3 & SM5703_STATUS3_DONE) || (chg_status3 & SM5703_STATUS3_TOPOFF)) {
		status = POWER_SUPPLY_STATUS_FULL;
		charger->full_charged = true;
		pr_info("%s : Status, Power Supply Full \n", __func__);
	} else if (chg_status3 & SM5703_STATUS3_CHGON) {
		status = POWER_SUPPLY_STATUS_CHARGING;    
	} else {
		if (nCHG)
			status = POWER_SUPPLY_STATUS_DISCHARGING;
		else
			status = POWER_SUPPLY_STATUS_NOT_CHARGING;
	}


	/* TEMP_TEST : when OTG is enabled(charging_current -1), handle OTG func. */
	if (charger->charging_current < 0) {
		/* For OTG mode, SM5703 would still report "charging" */
		status = POWER_SUPPLY_STATUS_DISCHARGING;
		ret = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_STATUS1);
		if (ret & SM5703_STATUS1_OTGFAIL) {
			pr_info("%s: otg overcurrent limit\n", __func__);
			sm5703_charger_otg_control(charger, false);
		}

	}

	return status;
}

static int sm5703_get_charging_health(struct sm5703_charger_data *charger)
{
	int vbus_status = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_STATUS5);
	int health = POWER_SUPPLY_HEALTH_GOOD;
	int chg_status3;
	int nCHG = 0;

	chg_status3 = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_STATUS3);

	pr_info("%s : is_charging = %d, STATUS3 = 0x%x, cable_type = %d, is_current_reduced = %d\n",
		__func__, charger->is_charging, chg_status3, charger->cable_type, charger->is_current_reduced);

	// temp for test
	pr_info("%s : vbus_status = %d\n", __func__, vbus_status);

	if (vbus_status < 0) {
		health = POWER_SUPPLY_HEALTH_UNKNOWN;
		pr_info("%s : Health : %d, vbus_status : %d\n", __func__, health,vbus_status);

		return (int)health;
	}

	if (vbus_status & SM5703_STATUS5_VBUSOK)
		health = POWER_SUPPLY_HEALTH_GOOD;
	else if (vbus_status & SM5703_STATUS5_VBUSOVP)
		health = POWER_SUPPLY_HEALTH_OVERVOLTAGE;
	else if (vbus_status & SM5703_STATUS5_VBUSUVLO)
		health = POWER_SUPPLY_HEALTH_UNDERVOLTAGE;
	else
		health = POWER_SUPPLY_HEALTH_UNKNOWN;
	
	if (health == POWER_SUPPLY_HEALTH_GOOD) {
		/* check if chgen */
		nCHG = gpio_get_value(charger->pdata->chgen_gpio);

		/* print the log at the abnormal case */
		if ((charger->is_charging == 1) && (chg_status3 & SM5703_STATUS3_DONE) &&
			(!nCHG)) {
			gpio_direction_output(charger->pdata->chgen_gpio,
				(charger->is_charging)); /* Disable Charger */
			sm5703_test_read(charger->sm5703->i2c_client);
			gpio_direction_output(charger->pdata->chgen_gpio,
				!(charger->is_charging)); /* re-enable Charger */
			pr_info("%s : FORCE RE-ENABLE Charger in Fake DONE state\n", __func__);
		}
	}

	pr_info("%s : Health : %d\n", __func__, health);

	return (int)health;
}

static int sec_chg_get_property(struct power_supply *psy,
		enum power_supply_property psp,
		union power_supply_propval *val)
{

	int chg_curr, aicr, vbus_status;
	struct sm5703_charger_data *charger =
		container_of(psy, struct sm5703_charger_data, psy_chg);

	switch (psp) {
		case POWER_SUPPLY_PROP_ONLINE:
			vbus_status = sm5703_reg_read(charger->sm5703->i2c_client, SM5703_STATUS5);
			if (charger->cable_type != POWER_SUPPLY_TYPE_BATTERY &&
				!(vbus_status & SM5703_STATUS5_VBUSOK))
					charger->cable_type = POWER_SUPPLY_TYPE_BATTERY;

			val->intval = charger->cable_type;
			pr_info("%s: Charger Cable type : %d\n", __func__, charger->cable_type);
			break;
		case POWER_SUPPLY_PROP_STATUS:
			val->intval = sm5703_get_charging_status(charger);
			break;
		case POWER_SUPPLY_PROP_HEALTH:
			val->intval = sm5703_get_charging_health(charger);
			break;
		case POWER_SUPPLY_PROP_CURRENT_MAX:
			sm5703_test_read(charger->sm5703->i2c_client);
			val->intval = sm5703_get_fast_charging_current(charger->sm5703->i2c_client);
			// AOSP expects microamperes
			val->intval *= 1000;
			break;
		case POWER_SUPPLY_PROP_CURRENT_AVG:
			break;
		case POWER_SUPPLY_PROP_CURRENT_NOW:
			if (charger->charging_current) {
				aicr = sm5703_get_input_current_limit(charger->sm5703->i2c_client);
				chg_curr = sm5703_get_fast_charging_current(charger->sm5703->i2c_client);
				val->intval = MINVAL(aicr, chg_curr);
			} else
				val->intval = 0;
			break;
#if defined(CONFIG_BATTERY_SWELLING) || defined(CONFIG_BATTERY_SWELLING_SELF_DISCHARGING)
		case POWER_SUPPLY_PROP_VOLTAGE_MAX:
			val->intval = sm5703_get_regulation_voltage(charger);
		break;
#endif
		case POWER_SUPPLY_PROP_CHARGE_TYPE:
			if (!charger->is_charging || charger->cable_type == POWER_SUPPLY_TYPE_BATTERY) {
				val->intval = POWER_SUPPLY_CHARGE_TYPE_NONE;
			} else if (charger->current_max <= SLOW_CHARGING_CURRENT_STANDARD) {
				val->intval = POWER_SUPPLY_CHARGE_TYPE_SLOW;
				pr_info("%s: slow-charging mode\n", __func__);
			} else
				val->intval = POWER_SUPPLY_CHARGE_TYPE_FAST;
			break;
		case POWER_SUPPLY_PROP_CHARGING_ENABLED:
			val->intval = charger->is_charging;
			break;
		case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
			break;
		default:
			return -EINVAL;
	}

	return 0;
}

static int sec_chg_set_property(struct power_supply *psy,
		enum power_supply_property psp,
		const union power_supply_propval *val)
{
	struct sm5703_charger_data *charger =
		container_of(psy, struct sm5703_charger_data, psy_chg);

	int topoff, chg_current;
	union power_supply_propval value;
	int previous_cable_type = charger->cable_type;

	switch (psp) {
		case POWER_SUPPLY_PROP_STATUS:
			charger->status = val->intval;
			break;
			/* val->intval : type */
		case POWER_SUPPLY_PROP_ONLINE:
			charger->cable_type = val->intval;
			if (previous_cable_type != charger->cable_type) {
				charger->current_max = charger->pdata->charging_current_table
						[charger->cable_type].input_current_limit;
				charger->charging_current = charger->pdata->charging_current_table
						[charger->cable_type].fast_charging_current;

				charger->is_current_reduced = false;
				__pm_relax(charger->vbuslimit_wake_lock);
				cancel_delayed_work(&charger->vbuslimit_work);
#if EN_AICL_IRQ
				sm5703_set_aicl_irq(charger, 0);
#endif
			}

			if (val->intval == POWER_SUPPLY_TYPE_POWER_SHARING) {
				psy_do_property("ps", get,
						POWER_SUPPLY_PROP_STATUS, value);

				sm5703_charger_otg_control(charger, value.intval);
			} else if (charger->cable_type == POWER_SUPPLY_TYPE_BATTERY) {
				pr_info("%s:[BATT] Type Battery\n", __func__);
				/* sm5703_enable_charger_switch(charger, 0); */
				if (previous_cable_type == POWER_SUPPLY_TYPE_OTG)
					sm5703_charger_otg_control(charger, false);
				/* set default input current */
				charger->current_max = charger->pdata->charging_current_table
						[POWER_SUPPLY_TYPE_USB].input_current_limit;
				charger->is_mdock = false;
				sm5703_set_input_current_limit(charger, charger->current_max);
			} else if (charger->cable_type == POWER_SUPPLY_TYPE_OTG) {
				pr_info("%s: OTG mode\n", __func__);
				//2017.01.06 : If Lanhub cable is changed to OTG cable, needed to disable charger operation.
				pr_info("%s: previous_cable_type = %d, cable_type = %d\n", __func__,previous_cable_type, charger->cable_type);
				if (previous_cable_type == POWER_SUPPLY_TYPE_LAN_HUB)
				{				
					pr_info("%s: LAN HUB condition is turned off by charger driver\n", __func__);
					sm5703_enable_charger_switch(charger, 0);
				}
				sm5703_charger_otg_control(charger, true);
			} else {
				pr_info("%s:[BATT] Set charging"
					", Cable type = %d\n", __func__, charger->cable_type);
				/* check mdock */
				if (charger->is_mdock) { /* if mdock was alread inserted, then check OTG, or NOTG state */
					if (charger->cable_type == POWER_SUPPLY_TYPE_SMART_NOTG) {
						charger->charging_current =
							charger->pdata->charging_current_table
							[POWER_SUPPLY_TYPE_MDOCK_TA].fast_charging_current;
						charger->current_max =
							charger->pdata->charging_current_table
							[POWER_SUPPLY_TYPE_MDOCK_TA].input_current_limit;
					} else if (charger->cable_type == POWER_SUPPLY_TYPE_SMART_OTG) {
						charger->charging_current =
							charger->pdata->charging_current_table
							[POWER_SUPPLY_TYPE_MDOCK_TA].fast_charging_current - 500;
						charger->current_max =
							charger->pdata->charging_current_table
							[POWER_SUPPLY_TYPE_MDOCK_TA].input_current_limit - 500;
					}
				} else { /*if mdock wasn't inserted, then check mdock state*/
					if (charger->cable_type == POWER_SUPPLY_TYPE_MDOCK_TA) {
						charger->is_mdock = true;
					}
				}

				//2017.01.06 : If OTG cable is changed to Lanhub cable, needed to disable OTG operation.				
				pr_info("%s: previous_cable_type = %d\n", __func__,previous_cable_type);
				if (previous_cable_type == POWER_SUPPLY_TYPE_OTG && charger->cable_type == POWER_SUPPLY_TYPE_LAN_HUB)
				{
					pr_info("%s:OTG condition is turned off by charger driver\n", __func__);				
					sm5703_charger_otg_control(charger, false);
				}

				/* Enable charger */
				sm5703_configure_charger(charger);
			}
#if EN_TEST_READ
			//msleep(100);
			sm5703_test_read(charger->sm5703->i2c_client);
#endif
			break;
		case POWER_SUPPLY_PROP_CURRENT_AVG:
#if defined(CONFIG_BATTERY_SWELLING)
			if (val->intval > charger->charging_current) {
				break;
			}
#endif
			chg_current = val->intval * charger->siop_level / 100;
			topoff = sm5703_get_current_topoff_setting(charger);
			pr_info("%s:Set chg current = %d mA, topoff = %d mA\n", __func__,
					chg_current, topoff);
			mutex_lock(&charger->io_lock);
			__sm5703_set_fast_charging_current(charger->sm5703->i2c_client,
					chg_current);
			__sm5703_set_termination_current_limit(
					charger->sm5703->i2c_client, topoff);
			mutex_unlock(&charger->io_lock);
			break;
		case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
			if (val->intval == charger->siop_level) {
				break;
			}
			charger->siop_level = val->intval;
		case POWER_SUPPLY_PROP_CURRENT_NOW:
			/* set charging current */
			if (charger->is_charging) {
				sm5703_configure_charger(charger);
			}
			if (sec_bat_get_slate_mode() == ENABLE)
			                sm5703_assign_bits(charger->sm5703->i2c_client,
                                                SM5703_CNTL, SM5703_OPERATION_MODE_MASK,
                                                SM5703_OPERATION_MODE_SUSPEND);
			break;
		case POWER_SUPPLY_PROP_CURRENT_MAX:
			/* set topoff current */
			pr_info("%s: Set topoff current = %d mA\n", __func__, val->intval);
			__sm5703_set_termination_current_limit(
					charger->sm5703->i2c_client, val->intval);
			break;
		case POWER_SUPPLY_PROP_POWER_NOW:
			topoff = sm5703_get_current_topoff_setting(charger);
			pr_info("%s:Set Power Now -> chg current = %d mA, topoff = %d mA\n", __func__,
					val->intval, topoff);
			sm5703_set_charging_current(charger, topoff, 0);
			break;
#if defined(CONFIG_BATTERY_SWELLING) || defined(CONFIG_BATTERY_SWELLING_SELF_DISCHARGING)
		case POWER_SUPPLY_PROP_VOLTAGE_MAX:
			pr_info("%s: float voltage(%d)\n", __func__, val->intval);
			charger->pdata->chg_float_voltage = val->intval;
			sm5703_set_regulation_voltage(charger, val->intval);
			break;
#endif
		case POWER_SUPPLY_PROP_HEALTH:
			//charger->ovp = val->intval;
			break;
		case POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL:
			sm5703_charger_otg_control(charger, val->intval);
			power_supply_changed(&charger->psy_otg);
			break;
		case POWER_SUPPLY_PROP_CHARGING_ENABLED:
			sm5703_enable_charger_switch(charger, val->intval);
			break;
		default:
			return -EINVAL;
	}

	return 0;
}

static int sm5703_otg_get_property(struct power_supply *psy,
				enum power_supply_property psp,
				union power_supply_propval *val)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = otg_enable_flag;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int sm5703_otg_set_property(struct power_supply *psy,
				enum power_supply_property psp,
				const union power_supply_propval *val)
{
	union power_supply_propval value;
	struct sm5703_charger_data *charger =
		container_of(psy, struct sm5703_charger_data, psy_otg);

	switch (psp) {
	case POWER_SUPPLY_PROP_ONLINE:
		value.intval = val->intval;
		pr_info("%s: OTG %s\n", __func__, value.intval > 0 ? "on" : "off");
		psy_do_property("sm5703-charger", set,
					POWER_SUPPLY_PROP_CHARGE_OTG_CONTROL, value);
		power_supply_changed(&charger->psy_otg);
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

ssize_t sm5703_chg_show_attrs(struct device *dev,
		const ptrdiff_t offset, char *buf)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct sm5703_charger_data *charger =
		container_of(psy, struct sm5703_charger_data, psy_chg);
	int i = 0;
	char *str = NULL;

	switch (offset) {
	case CHG_REG:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%x\n",
				charger->reg_addr);
		break;
	case CHG_DATA:
		i += scnprintf(buf + i, PAGE_SIZE - i, "%x\n",
				charger->reg_data);
		break;
	case CHG_REGS:
		str = kzalloc(sizeof(char) * 256, GFP_KERNEL);
		if (!str)
			return -ENOMEM;

		sm5703_read_regs(charger->sm5703->i2c_client, str);
		i += scnprintf(buf + i, PAGE_SIZE - i, "%s\n",
				str);

		kfree(str);
		break;
	default:
		i = -EINVAL;
		break;
	}

	return i;
}

ssize_t sm5703_chg_store_attrs(struct device *dev,
		const ptrdiff_t offset,
		const char *buf, size_t count)
{
	struct power_supply *psy = dev_get_drvdata(dev);
	struct sm5703_charger_data *charger =
		container_of(psy, struct sm5703_charger_data, psy_chg);

	int ret = 0;
	int x = 0;
	uint8_t data = 0;

	switch (offset) {
	case CHG_REG:
		if (sscanf(buf, "%x\n", &x) == 1) {
			charger->reg_addr = x;
			data = sm5703_reg_read(charger->sm5703->i2c_client,
					charger->reg_addr);
			charger->reg_data = data;
			dev_dbg(dev, "%s: (read) addr = 0x%x, data = 0x%x\n",
					__func__, charger->reg_addr, charger->reg_data);
			ret = count;
		}
		break;
	case CHG_DATA:
		if (sscanf(buf, "%x\n", &x) == 1) {
			data = (u8)x;

			dev_dbg(dev, "%s: (write) addr = 0x%x, data = 0x%x\n",
					__func__, charger->reg_addr, data);
			ret = sm5703_reg_write(charger->sm5703->i2c_client,
					charger->reg_addr, data);
			if (ret < 0) {
				dev_dbg(dev, "I2C write fail Reg0x%x = 0x%x\n",
						(int)charger->reg_addr, (int)data);
			}
			ret = count;
		}
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret;
}

struct sm5703_chg_irq_handler {
	char *name;
	int irq_index;
	irqreturn_t (*handler)(int irq, void *data);
};
#if EN_NOBAT_IRQ
static irqreturn_t sm5703_chg_nobat_irq_handler(int irq, void *data)
{
	struct sm5703_charger_data *info = data;
	struct i2c_client *iic = info->sm5703->i2c_client;

	/* set full charged flag
	 * until TA/USB unplug event / stop charging by PSY
	 */

	pr_info("%s : Nobat\n", __func__);

#if EN_TEST_READ
	sm5703_test_read(iic);
#endif

	return IRQ_HANDLED;
}
#endif /*EN_NOBAT_IRQ*/

#if EN_DONE_IRQ
static irqreturn_t sm5703_chg_done_irq_handler(int irq, void *data)
{
	struct sm5703_charger_data *info = data;
	struct i2c_client *iic = info->sm5703->i2c_client;

	/* set full charged flag
	 * until TA/USB unplug event / stop charging by PSY
	 */

	pr_info("%s : Full charged(done)\n", __func__);
	info->full_charged = true;

#if EN_TEST_READ
	sm5703_test_read(iic);
#endif

	return IRQ_HANDLED;
}
#endif/*EN_DONE_IRQ*/

#if EN_TOPOFF_IRQ
static irqreturn_t sm5703_chg_topoff_irq_handler(int irq, void *data)
{
	struct sm5703_charger_data *info = data;
	struct i2c_client *iic = info->sm5703->i2c_client;

	/* set full charged flag
	 * until TA/USB unplug event / stop charging by PSY
	 */

	pr_info("%s : Full charged(topoff)\n", __func__);
	info->full_charged = true;

#if EN_TEST_READ
	sm5703_test_read(iic);
#endif

	return IRQ_HANDLED;
}
#endif /*EN_TOPOFF_IRQ*/

#if EN_CHGON_IRQ
static irqreturn_t sm5703_chg_chgon_irq_handler(int irq, void *data)
{
	struct sm5703_charger_data *info = data;
	struct i2c_client *iic = info->sm5703->i2c_client;

	pr_info("%s : Chgon\n", __func__);

#if EN_TEST_READ
	sm5703_test_read(iic);
#endif

	return IRQ_HANDLED;
}
#endif /*EN_CHGON_IRQ*/

#if EN_OTGFAIL_IRQ
static irqreturn_t sm5703_chg_otgfail_irq_handler(int irq, void *data)
{
	struct sm5703_charger_data *info = data;
	/* struct i2c_client *iic = info->sm5703->i2c_client; */
		queue_work(info->wq, &info->otg_fail_work);

	return IRQ_HANDLED;
}

static void sm5703_chg_otg_fail_work(struct work_struct *work)
{
	struct sm5703_charger_data *charger =
			container_of(work, struct sm5703_charger_data, otg_fail_work);
	struct i2c_client *i2c = charger->sm5703->i2c_client;


	int ret;
	int otg_check_cnt = 0;
	

#ifdef CONFIG_USB_HOST_NOTIFY
	struct otg_notify *o_notify;

	o_notify = get_otg_notify();
#endif

	__pm_stay_awake(charger->otg_fail_wake_lock);

	pr_info("%s : start \n", __func__);

	ret = sm5703_reg_read(i2c, SM5703_STATUS1);
	if (ret & SM5703_STATUS1_OTGFAIL) {
		pr_info("%s: otg overcurrent limit\n", __func__);
		
		ret = sm5703_reg_read(i2c, SM5703_INTMSK1);
		pr_info("%s: Pre check SM5703_INTMSK1 = 0x%x\n", __func__,ret);
		
		sm5703_assign_bits(i2c, SM5703_INTMSK1, 0x40, 0x40); //OTG Fail Interrupt Mask

		ret = sm5703_reg_read(i2c, SM5703_INTMSK1);
		pr_info("%s: Halfway check SM5703_INTMSK1 = 0x%x\n", __func__,ret);
		
		sm5703_charger_otg_control(charger, false);
		sm5703_charger_otg_control(charger, true);

		while (otg_check_cnt < 5) //OTG Fail Check retry
		{
			msleep(20);
			ret = sm5703_reg_read(i2c, SM5703_STATUS1);		
			pr_info("%s: SM5703_STATUS1 = 0x%x\n", __func__, ret);
			if (ret & SM5703_STATUS1_OTGFAIL) {	
#ifdef CONFIG_USB_HOST_NOTIFY
				send_otg_notify(o_notify, NOTIFY_EVENT_OVERCURRENT, 0);
#endif
				sm5703_charger_otg_control(charger, false);
				break;
			} else {
				pr_info("%s: otg_check_cnt = %d\n", __func__, otg_check_cnt);				
			}
			otg_check_cnt++;
		}
		sm5703_assign_bits(i2c, SM5703_INTMSK1, 0x40, 0x00); //OTG Fail Interrupt UnMask
	}
	ret = sm5703_reg_read(i2c, SM5703_INTMSK1);
	pr_info("%s: Final check SM5703_INTMSK1 = 0x%x\n", __func__,ret);

	__pm_relax(charger->otg_fail_wake_lock);
	
}
#endif /*EN_CHGON_IRQ*/

static void sm5703_chg_vbuslimit_work(struct work_struct *work)
{
	struct sm5703_charger_data *charger =
			container_of(work, struct sm5703_charger_data, vbuslimit_work.work);
	struct i2c_client *i2c = charger->sm5703->i2c_client;

	if (charger->cable_type != POWER_SUPPLY_TYPE_BATTERY) {
		int vbuslimit_state;

		vbuslimit_state = sm5703_reg_read(i2c, SM5703_STATUS1) & 0x08;
		if (vbuslimit_state || (charger->current_max <= MINIMUM_INPUT_CURRENT)) {
			/* check slow charging */
			if (charger->is_current_reduced &&
				charger->current_max <= SLOW_CHARGING_CURRENT_STANDARD) {
				union power_supply_propval value;
				psy_do_property("battery", set,
					POWER_SUPPLY_PROP_CHARGE_TYPE, value);
				pr_info("%s: slow charging on : input current(%dmA), cable type(%d)\n",
					__func__, charger->current_max, charger->cable_type);
			}
			__pm_relax(charger->vbuslimit_wake_lock);
		} else {
			/* reduce input current & restart vbuslimit work */
			int reg_data, temp;

			mutex_lock(&charger->io_lock);
			charger->is_current_reduced = true;

			charger->current_max -= REDUCE_CURRENT_STEP;
			reg_data = sm5703_reg_read(i2c, SM5703_VBUSCNTL);
			reg_data &= ~SM5703_VBUSLIMIT;
			temp = ((charger->current_max - 100) / 50) | reg_data;
			sm5703_reg_write(i2c, SM5703_VBUSCNTL, temp);
			pr_info("%s: reduce input current(%d)\n", __func__, charger->current_max);
			mutex_unlock(&charger->io_lock);

			queue_delayed_work_on(0, charger->wq,
				&charger->vbuslimit_work, msecs_to_jiffies(VBUSLIMIT_DELAY));
		}
		pr_info("%s: vbuslimit state(%d)\n", __func__, vbuslimit_state);
	} else {
		__pm_relax(charger->vbuslimit_wake_lock);
	}
}

#if EN_VBUSLIMIT_IRQ
static irqreturn_t sm5703_chg_vbuslimit_irq_handler(int irq, void *data)
{
	struct sm5703_charger_data *charger = data;
	struct i2c_client *i2c = charger->sm5703->i2c_client;

	pr_info("%s: VBUS Limit\n", __func__);

#if EN_TEST_READ
	sm5703_test_read(i2c);
#endif

	return IRQ_HANDLED;
}
#endif /* EN_VBUSLIMIT_IRQ */

#if EN_AICL_IRQ
static irqreturn_t sm5703_chg_aicl_irq_handler(int irq, void *data)
{
	struct sm5703_charger_data *charger = data;
	struct i2c_client *i2c = charger->sm5703->i2c_client;

	pr_info("%s: AICL\n", __func__);

	sm5703_set_aicl_irq(charger, 1);

	if (charger->pdata->chg_vbuslimit &&
		charger->cable_type != POWER_SUPPLY_TYPE_BATTERY) {
		/* start vbuslimit work */
		__pm_stay_awake(charger->vbuslimit_wake_lock);
		queue_delayed_work_on(0, charger->wq,
			&charger->vbuslimit_work, msecs_to_jiffies(START_VBUSLIMIT_DELAY));
	}

#if EN_TEST_READ
	sm5703_test_read(i2c);
#endif

	return IRQ_HANDLED;
}
#endif /* EN_AICL_IRQ */

const struct sm5703_chg_irq_handler sm5703_chg_irq_handlers[] = {
#if EN_NOBAT_IRQ    
	{
		.name = "NOBAT",
		.handler = sm5703_chg_nobat_irq_handler,
		.irq_index = SM5703_NOBAT_IRQ,
	},
#endif /*EN_NOBAT_IRQ*/
#if EN_DONE_IRQ
	{
		.name = "DONE",
		.handler = sm5703_chg_done_irq_handler,
		.irq_index = SM5703_DONE_IRQ,
	},
#endif/*EN_DONE_IRQ*/	
#if EN_TOPOFF_IRQ	
	{
		.name = "TOPOFF",
		.handler = sm5703_chg_topoff_irq_handler,
		.irq_index = SM5703_TOPOFF_IRQ,
	},
#endif /*EN_TOPOFF_IRQ*/
#if EN_CHGON_IRQ
	{
		.name = "CHGON",
		.handler = sm5703_chg_chgon_irq_handler,
		.irq_index = SM5703_CHGON_IRQ,
	},
#endif /*EN_CHGON_IRQ*/
#if EN_OTGFAIL_IRQ
	{
		.name = "OTGFAIL",
		.handler = sm5703_chg_otgfail_irq_handler,
		.irq_index = SM5703_OTGFAIL_IRQ,
	},
#endif /* EN_OTGFAIL_IRQ */
#if EN_VBUSLIMIT_IRQ
	{
		.name = "VBUSLIMIT",
		.handler = sm5703_chg_vbuslimit_irq_handler,
		.irq_index = SM5703_VBUSLIMIT_IRQ,
	},
#endif /* EN_VBUSLIMIT_IRQ */
#if EN_AICL_IRQ
	{
		.name = "AICL",
		.handler = sm5703_chg_aicl_irq_handler,
		.irq_index = SM5703_AICL_IRQ,
	},
#endif /* EN_AICL_IRQ */
};


static int register_irq(struct platform_device *pdev,
		struct sm5703_charger_data *info)
{
	int irq;
	int i, j;
	int ret;
	const struct sm5703_chg_irq_handler *irq_handler = sm5703_chg_irq_handlers;
	const char *irq_name;
	for (i = 0; i < ARRAY_SIZE(sm5703_chg_irq_handlers); i++) {
		irq_name = sm5703_get_irq_name_by_index(irq_handler[i].irq_index);
		irq = platform_get_irq_byname(pdev, irq_name);
		ret = request_threaded_irq(irq, NULL, irq_handler[i].handler,
				IRQF_ONESHOT | IRQF_TRIGGER_FALLING |
				IRQF_NO_SUSPEND, irq_name, info);
		if (ret < 0) {
			pr_err("%s : Failed to request IRQ (%s): #%d: %d\n",
					__func__, irq_name, irq, ret);
			goto err_irq;
		}

		pr_info("%s : Register IRQ%d(%s) successfully\n",
				__func__, irq, irq_name);
	}

	return 0;
err_irq:
	for (j = 0; j < i; j++) {
		irq_name = sm5703_get_irq_name_by_index(irq_handler[j].irq_index);
		irq = platform_get_irq_byname(pdev, irq_name);
		free_irq(irq, info);
	}

	return ret;
}

static void unregister_irq(struct platform_device *pdev,
		struct sm5703_charger_data *info)
{
	int irq;
	int i;
	const char *irq_name;
	const struct sm5703_chg_irq_handler *irq_handler = sm5703_chg_irq_handlers;

	for (i = 0; i < ARRAY_SIZE(sm5703_chg_irq_handlers); i++) {
		irq_name = sm5703_get_irq_name_by_index(irq_handler[i].irq_index);
		irq = platform_get_irq_byname(pdev, irq_name);
		free_irq(irq, info);
	}
}

#ifdef CONFIG_OF
static int sec_bat_read_u32_index_dt(const struct device_node *np,
		const char *propname,
		u32 index, u32 *out_value)
{
	struct property *prop = of_find_property(np, propname, NULL);
	u32 len = (index + 1) * sizeof(*out_value);

	if (!prop)
		return (-EINVAL);
	if (!prop->value)
		return (-ENODATA);
	if (len > prop->length)
		return (-EOVERFLOW);

	*out_value = be32_to_cpup(((__be32 *)prop->value) + index);

	return 0;
}

static int sm5703_charger_parse_dt(struct device *dev,
		struct sm5703_charger_platform_data *pdata)
{
	struct device_node *np = of_find_node_by_name(NULL, "charger");
	const u32 *p;
	int ret, i, len;

	ret = of_property_read_u32(np, "chg_autostop", &pdata->chg_autostop);
	if (ret < 0) {
		pr_info("%s : cannot get chg autostop\n", __func__);
		pdata->chg_autostop = 0;
	}

	ret = of_property_read_u32(np, "chg_autoset", &pdata->chg_autoset);
	if (ret < 0) {
		pr_info("%s : cannot get chg autoset\n", __func__);
		pdata->chg_autoset = 0;
	}

	ret = of_property_read_u32(np, "chg_aiclen", &pdata->chg_aiclen);
	if (ret < 0) {
		pr_info("%s : cannot get chg aiclen\n", __func__);
		pdata->chg_aiclen = 0;
	}

	ret = of_property_read_u32(np, "chg_aiclth", &pdata->chg_aiclth);
	if (ret < 0) {
		pr_info("%s : cannot get chg aiclth\n", __func__);
		pdata->chg_aiclth = 4500;
	}

	ret = of_property_read_u32(np, "chg_vbuslimit", &pdata->chg_vbuslimit);
	if (ret < 0) {
		pr_info("%s : cannot get chg vbuslimit\n", __func__);
		pdata->chg_vbuslimit = 0;
	}

	ret = of_property_read_u32(np, "fg_vol_val", &pdata->fg_vol_val);
	if (ret < 0) {
		pr_info("%s : cannot get fg_vol_val\n", __func__);
		pdata->fg_vol_val = 4350;
	}

	ret = of_property_read_u32(np, "fg_soc_val", &pdata->fg_soc_val);
	if (ret < 0) {
		pr_info("%s : cannot get fg_soc_val\n", __func__);
		pdata->fg_soc_val = 95;
	}

	ret = of_property_read_u32(np, "fg_curr_avr_val",
		&pdata->fg_curr_avr_val);
	if (ret < 0) {
		pr_info("%s : cannot get fg_curr_avr_val\n", __func__);
		pdata->fg_curr_avr_val = 150;
	}

	ret = of_property_read_u32(np, "otg_current",
		&pdata->otg_current);
	if (ret < 0) {
		pr_info("%s : cannot get otg_current and set default value(1.2A).\n", __func__);
		pdata->otg_current = 1200;
	}

	ret = of_property_read_u32(np, "bst_iq3limit",
		&pdata->bst_iq3limit);
	if (ret < 0) {
		pr_info("%s : cannot get bst_iq3limit\n", __func__);
		pdata->bst_iq3limit = 0;
	}

	ret = of_property_read_u32(np, "battery,chg_float_voltage",
			&pdata->chg_float_voltage);
	if (ret < 0) {
		pr_info("%s : cannot get chg float voltage\n", __func__);
		pdata->chg_float_voltage = 4350;
	}

	pdata->chgen_gpio = of_get_named_gpio(np, "battery,chg_gpio_en", 0);
	if (pdata->chgen_gpio < 0) {
		pr_err("%s : cannot get chgen gpio : %d\n",
			__func__, pdata->chgen_gpio);
		return -ENODATA;	
	} else {
		pr_info("%s: chgen gpio : %d\n", __func__, pdata->chgen_gpio);
	}

	np = of_find_node_by_name(NULL, "battery");
	if (!np) {
		pr_info("%s : np NULL\n", __func__);
		return -ENODATA;
	}

	ret = of_property_read_string(np,
		"battery,charger_name", (char const **)&pdata->charger_name);
	if (ret) {
		pdata->charger_name = "sm5703-charger";
		pr_info("%s: Charger name is Empty. Set default.\n", __func__);
	}

	ret = of_property_read_u32(np, "battery,full_check_type",
			&pdata->full_check_type);
	pr_info("%s full_check_type: %d\n", __func__, pdata->full_check_type);
	if (ret < 0)
		pr_err("%s error reading battery,full_check_type %d\n", __func__, ret);

	ret = of_property_read_u32(np, "battery,full_check_type_2nd",
			&pdata->full_check_type_2nd);
	pr_info("%s full_check_type_2nd: %d\n", __func__, pdata->full_check_type_2nd);
	if (ret < 0)
		pr_err("%s error reading battery,full_check_type_2nd %d\n", __func__, ret);

	p = of_get_property(np, "battery,input_current_limit", &len);

	len = len / sizeof(u32);

	pdata->charging_current_table =
		kzalloc(sizeof(sec_charging_current_t) * len, GFP_KERNEL);

	for(i = 0; i < len; i++) {
		ret = sec_bat_read_u32_index_dt(np,
			"battery,input_current_limit", i,
			&pdata->charging_current_table[i].input_current_limit);
		ret = sec_bat_read_u32_index_dt(np,
			"battery,fast_charging_current", i,
			&pdata->charging_current_table[i].fast_charging_current);
		ret = sec_bat_read_u32_index_dt(np,
			"battery,full_check_current_1st", i,
			&pdata->charging_current_table[i].full_check_current_1st);
		ret = sec_bat_read_u32_index_dt(np,
			"battery,full_check_current_2nd", i,
			&pdata->charging_current_table[i].full_check_current_2nd);
	}

	dev_info(dev,"sm5703 charger parse dt retval = %d\n", ret);
	return ret;
}

static struct of_device_id sm5703_charger_match_table[] = {
	{ .compatible = "siliconmitus,sm5703-charger",},
	{},
};
#else
static int sm5703_charger_parse_dt(struct device *dev,
		struct sm5703_charger_platform_data *pdata)
{
	return -ENOSYS;
}
#define sm5703_charger_match_table NULL
#endif /* CONFIG_OF */

static int sm5703_charger_probe(struct platform_device *pdev)
{
	sm5703_mfd_chip_t *chip = dev_get_drvdata(pdev->dev.parent);
#ifndef CONFIG_OF
	struct sm5703_mfd_platform_data *mfd_pdata =
				dev_get_platdata(chip->dev);
#endif	
	struct sm5703_charger_data *charger;
	struct power_supply_config psy_cfg = {};
	struct power_supply *psr;
	struct power_supply_desc *psy_chg_desc;
	struct power_supply_desc *psy_otg_desc;
	int ret = 0;

	otg_enable_flag = 0;

	pr_info("%s:[BATT] SM5703 Charger driver probe..\n", __func__);

	charger = kzalloc(sizeof(*charger), GFP_KERNEL);
	if (!charger)
		return -ENOMEM;

	mutex_init(&charger->io_lock);
	charger->sm5703= chip;
	charger->client = chip->i2c_client;

#ifdef CONFIG_OF	
	charger->pdata = devm_kzalloc(&pdev->dev,
			sizeof(*(charger->pdata)), GFP_KERNEL);
	if (!charger->pdata) {
		dev_err(&pdev->dev, "Failed to allocate memory\n");
		ret = -ENOMEM;
		goto err_parse_dt_nomem;
	}
	ret = sm5703_charger_parse_dt(&pdev->dev, charger->pdata);
	if (ret < 0)
		goto err_parse_dt;
#else		
	charger->pdata = mfd_pdata->charger_platform_data;
#endif

	platform_set_drvdata(pdev, charger);

	if (charger->pdata->charger_name == NULL)
		charger->pdata->charger_name = "sm5703-charger";
	
	psy_chg_desc = (struct power_supply_desc *) charger->psy_chg.desc;
	psy_otg_desc = (struct power_supply_desc *) charger->psy_otg.desc;

	psy_chg_desc->name           = charger->pdata->charger_name;
	psy_chg_desc->type           = POWER_SUPPLY_TYPE_UNKNOWN;
	psy_chg_desc->get_property   = sec_chg_get_property;
	psy_chg_desc->set_property   = sec_chg_set_property;
	psy_chg_desc->properties     = sec_charger_props;
	psy_chg_desc->num_properties = ARRAY_SIZE(sec_charger_props);
	psy_otg_desc->name		= "otg";
	psy_otg_desc->type		= POWER_SUPPLY_TYPE_OTG;
	psy_otg_desc->get_property	= sm5703_otg_get_property;
	psy_otg_desc->set_property	= sm5703_otg_set_property;
	psy_otg_desc->properties		= sm5703_otg_props;
	psy_otg_desc->num_properties	= ARRAY_SIZE(sm5703_otg_props);
	
	charger->siop_level = 100;
	charger->ovp = 0;
	charger->is_mdock = false;

	ret = gpio_request(charger->pdata->chgen_gpio, "sm5703_nCHGEN");
	if (ret<0) {
		pr_err("%s : Request GPIO %d failed : %d\n",
				__func__, (int)charger->pdata->chgen_gpio,ret);
	}

	sm5703_chg_init(charger);

	charger->wq = create_workqueue("sm5703chg_workqueue");
	if (charger->pdata->chg_vbuslimit) {
		INIT_DELAYED_WORK(&charger->vbuslimit_work, sm5703_chg_vbuslimit_work);
		charger->vbuslimit_wake_lock = wakeup_source_create("sm5703-vbuslimit");
	}

#if EN_OTGFAIL_IRQ
	charger->otg_fail_wake_lock = wakeup_source_create("otg_fail_wake_lock");
	INIT_WORK(&charger->otg_fail_work, sm5703_chg_otg_fail_work);
#endif	

	psr = power_supply_register(&pdev->dev, psy_chg_desc, &psy_cfg);
	if (IS_ERR(psr)) {
		pr_err("%s: Failed to Register psy_chg\n", __func__);
		goto err_power_supply_register;
	}
	psr = power_supply_register(&pdev->dev, psy_otg_desc, &psy_cfg);
	if (IS_ERR(psr)) {
		pr_err("%s: Failed to Register otg_chg\n", __func__);
		goto err_power_supply_register_otg;
	}
	ret = register_irq(pdev, charger);
	if (ret < 0)
		goto err_reg_irq;

	sm5703_test_read(charger->sm5703->i2c_client);
	pr_info("%s:[BATT] SM5703 charger driver loaded OK\n", __func__);

	return 0;
err_reg_irq:
	power_supply_unregister(&charger->psy_otg);
err_power_supply_register_otg:
	power_supply_unregister(&charger->psy_chg);
err_power_supply_register:
	destroy_workqueue(charger->wq);
	if (charger->pdata->chg_vbuslimit) {
		wakeup_source_destroy(charger->vbuslimit_wake_lock);
	}
err_parse_dt:
err_parse_dt_nomem:
	mutex_destroy(&charger->io_lock);
	kfree(charger);
	return ret;
}

static int sm5703_charger_remove(struct platform_device *pdev)
{
	struct sm5703_charger_data *charger;
	pr_info("%s: SM5703 Charger driver remove\n", __func__);
	charger = platform_get_drvdata(pdev);
	unregister_irq(pdev, charger);
	power_supply_unregister(&charger->psy_chg);
	destroy_workqueue(charger->wq);
	if (charger->pdata->chg_vbuslimit) {
		wakeup_source_destroy(charger->vbuslimit_wake_lock);
	}
	mutex_destroy(&charger->io_lock);
	gpio_free(charger->pdata->chgen_gpio);
	kfree(charger);
	return 0;
}

#if defined CONFIG_PM
static int sm5703_charger_suspend(struct device *dev)
{
	return 0;
}

static int sm5703_charger_resume(struct device *dev)
{
	return 0;
}
#else
#define sm5703_charger_suspend NULL
#define sm5703_charger_resume NULL
#endif

static void sm5703_charger_shutdown(struct device *dev)
{
	pr_info("%s: SM5703 Charger driver shutdown\n", __func__);
}

static SIMPLE_DEV_PM_OPS(sm5703_charger_pm_ops, sm5703_charger_suspend,
		sm5703_charger_resume);

static struct platform_driver sm5703_charger_driver = {
	.driver		= {
		.name	= "sm5703-charger",
		.owner	= THIS_MODULE,
		.of_match_table = sm5703_charger_match_table,
		.pm 	= &sm5703_charger_pm_ops,
		.shutdown = sm5703_charger_shutdown,
#ifdef CONFIG_MULTITHREAD_PROBE
		.multithread_probe = 1,
#endif
	},
	.probe		= sm5703_charger_probe,
	.remove		= sm5703_charger_remove,
};

static int __init sm5703_charger_init(void)
{
	int ret = 0;

	pr_info("%s \n", __func__);
	ret = platform_driver_register(&sm5703_charger_driver);

	return ret;
}
subsys_initcall(sm5703_charger_init);

static void __exit sm5703_charger_exit(void)
{
	platform_driver_unregister(&sm5703_charger_driver);
}
module_exit(sm5703_charger_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Charger driver for SM5703");
