/*
 *  sec_adc.c
 *  Samsung Mobile Battery Driver
 *
 *  Copyright (C) 2012 Samsung Electronics
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/power/sec_adc.h>

static struct iio_channel *temp_adc;

static void sec_bat_adc_ap_init(struct platform_device *pdev)
{
	temp_adc = iio_channel_get_all(&pdev->dev);
}


#ifndef CONFIG_BATTERY_ADC_CHANNEL_SEPERATION
static int sec_bat_adc_ap_read(int channel)
{
	int data = -1;
	int ret = 0;

	switch (channel)
	{
	case SEC_BAT_ADC_CHANNEL_CABLE_CHECK:
	case SEC_BAT_ADC_CHANNEL_BAT_CHECK:
		ret = iio_read_channel_raw(&temp_adc[3], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_debug("VF ADC(%d)\n", data);
		break;
	case SEC_BAT_ADC_CHANNEL_TEMP:
	case SEC_BAT_ADC_CHANNEL_TEMP_AMBIENT:
		ret = iio_read_channel_raw(&temp_adc[0], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_debug("TEMP ADC(%d)\n", data);
		break;
	case SEC_BAT_ADC_CHANNEL_FULL_CHECK:
	case SEC_BAT_ADC_CHANNEL_VOLTAGE_NOW:
	case SEC_BAT_ADC_CHANNEL_NUM:
		break;
	case SEC_BAT_ADC_CHANNEL_CHG_TEMP:
	case SEC_BAT_ADC_CHANNEL_INBAT_VOLTAGE:
		ret = iio_read_channel_raw(&temp_adc[1], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_debug("TEMP ADC(%d)\n", data);
		break;
	case SEC_BAT_ADC_CHANNEL_DISCHARGING_CHECK:
		ret = iio_read_channel_raw(&temp_adc[2], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_debug("DISCHARGING CHECK(%d)\n", data);
		break;
	default:
		break;
	}
	return data;
}

#else

static int sec_bat_adc_ap_read(int channel)
{
	int data = -1;
	int ret = 0;

	switch (channel)
	{
	case SEC_BAT_ADC_CHANNEL_CABLE_CHECK:
	case SEC_BAT_ADC_CHANNEL_BAT_CHECK:
		break;
	case SEC_BAT_ADC_CHANNEL_TEMP:
	case SEC_BAT_ADC_CHANNEL_TEMP_AMBIENT:
		ret = iio_read_channel_raw(&temp_adc[0], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_debug("TEMP ADC(%d)\n", data);
		break;
	case SEC_BAT_ADC_CHANNEL_FULL_CHECK:
	case SEC_BAT_ADC_CHANNEL_VOLTAGE_NOW:
	case SEC_BAT_ADC_CHANNEL_NUM:
		break;
	case SEC_BAT_ADC_CHANNEL_INBAT_VOLTAGE:
		ret = iio_read_channel_raw(&temp_adc[1], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_info("INBAT ADC(%d)\n", data);
		break;
#ifdef CONFIG_BATTERY_SWELLING_SELF_DISCHARGING
	case SEC_BAT_ADC_CHANNEL_DISCHARGING_CHECK:
		ret = iio_read_channel_raw(&temp_adc[2], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_info("DISCHARGING CHECK(%d)\n", data);
		break;
#endif
	case SEC_BAT_ADC_CHANNEL_CHG_TEMP:
		ret = iio_read_channel_raw(&temp_adc[3], &data);
		if (ret < 0)
			pr_info("read channel error[%d]\n", ret);
		else
			pr_info("CHG TEMP ADC(%d)\n", data);
		break;		
		
	default:
		break;
	}
	return data;
}

#endif


static void sec_bat_adc_ap_exit(void)
{
	iio_channel_release(temp_adc);
}

static void sec_bat_adc_none_init(struct platform_device *pdev)
{
}

static int sec_bat_adc_none_read(int channel)
{
	return 0;
}

static void sec_bat_adc_none_exit(void)
{
}

static void sec_bat_adc_ic_init(struct platform_device *pdev)
{
}

static int sec_bat_adc_ic_read(int channel)
{
	return 0;
}

static void sec_bat_adc_ic_exit(void)
{
}
static int adc_read_type(struct sec_battery_info *battery, int channel)
{
	int adc = 0;

	switch (battery->pdata->temp_adc_type)
	{
	case SEC_BATTERY_ADC_TYPE_NONE :
		adc = sec_bat_adc_none_read(channel);
		break;
	case SEC_BATTERY_ADC_TYPE_AP :
		adc = sec_bat_adc_ap_read(channel);
		break;
	case SEC_BATTERY_ADC_TYPE_IC :
		adc = sec_bat_adc_ic_read(channel);
		break;
	case SEC_BATTERY_ADC_TYPE_NUM :
		break;
	default :
		break;
	}
	return adc;
}

static void adc_init_type(struct platform_device *pdev,
			  struct sec_battery_info *battery)
{
	switch (battery->pdata->temp_adc_type)
	{
	case SEC_BATTERY_ADC_TYPE_NONE :
		sec_bat_adc_none_init(pdev);
		break;
	case SEC_BATTERY_ADC_TYPE_AP :
		sec_bat_adc_ap_init(pdev);
		break;
	case SEC_BATTERY_ADC_TYPE_IC :
		sec_bat_adc_ic_init(pdev);
		break;
	case SEC_BATTERY_ADC_TYPE_NUM :
		break;
	default :
		break;
	}
}

static void adc_exit_type(struct sec_battery_info *battery)
{
	switch (battery->pdata->temp_adc_type)
	{
	case SEC_BATTERY_ADC_TYPE_NONE :
		sec_bat_adc_none_exit();
		break;
	case SEC_BATTERY_ADC_TYPE_AP :
		sec_bat_adc_ap_exit();
		break;
	case SEC_BATTERY_ADC_TYPE_IC :
		sec_bat_adc_ic_exit();
		break;
	case SEC_BATTERY_ADC_TYPE_NUM :
		break;
	default :
		break;
	}
}

int adc_read(struct sec_battery_info *battery, int channel)
{
	int adc = 0;

	adc = adc_read_type(battery, channel);

	dev_dbg(battery->dev, "[%s]adc = %d\n", __func__, adc);

	return adc;
}

void adc_init(struct platform_device *pdev, struct sec_battery_info *battery)
{
	adc_init_type(pdev, battery);
}

void adc_exit(struct sec_battery_info *battery)
{
	adc_exit_type(battery);
}

