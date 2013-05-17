/*
 * LED Thermal Trigger
 *
 * Copyright (C) 2013 Stratos Karafotis <stratosk@semaphore.gr>
 *
 * Based on Atsushi Nemoto's ledtrig-heartbeat.c
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */
#define DEBUG
#define pr_fmt(fmt) "ledtrig_thermal: " fmt

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/msm_tsens.h>
#include <linux/workqueue.h>
#include <linux/earlysuspend.h>
#include <linux/leds.h>
#include "leds.h"

#define HIGH_TEMP	(62)	/* in Celcius */
#define LOW_TEMP	(52)
#define SENSOR_ID	(7)
#define DELAY_OFF	(5 * HZ)
#define DELAY_ON	(2 * HZ)

static void check_temp(struct work_struct *work);
static DECLARE_DELAYED_WORK(check_temp_work, check_temp);
static unsigned delay;
static int brightness;
static int active;

static void thermal_trig_activate(struct led_classdev *led_cdev)
{
	delay = DELAY_OFF;
	schedule_delayed_work(&check_temp_work, delay);
	active = 1;
	pr_info("%s: activated\n", __func__);
}

static void thermal_trig_deactivate(struct led_classdev *led_cdev)
{
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();
	active = 0;
	led_set_brightness(led_cdev, LED_OFF);
	pr_info("%s: deactivated\n", __func__);
}

static struct led_trigger thermal_led_trigger = {
	.name     = "thermal",
	.activate = thermal_trig_activate,
	.deactivate = thermal_trig_deactivate,
};

static void check_temp(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	unsigned long temp = 0;
	int ret = 0;
	int br = 0;
	int diff = 0;

	tsens_dev.sensor_num = SENSOR_ID;
	ret = tsens_get_temp(&tsens_dev, &temp);
	if (ret) {
		pr_debug("%s: Unable to read TSENS sensor %d\n", __func__,
				tsens_dev.sensor_num);
		goto reschedule;
	}

	/* A..B -> C..D		x' = (D-C)*(X-A)/(B-A) */
	if (temp > LOW_TEMP)
		br = (LED_FULL * (temp - LOW_TEMP)) / (HIGH_TEMP - LOW_TEMP);

	diff = abs(br - brightness);
	if (diff > 120)
		brightness = br;
	else if (diff > 40)
		br > brightness ? (brightness += 10) : (brightness -= 10);
	else if (diff > 20)
		br > brightness ? (brightness += 5) : (brightness -= 5);
	else if (diff > 10)
		br > brightness ? (brightness += 2) : (brightness -= 2);
	else
		br > brightness ? ++brightness : --brightness;

	if (brightness < LED_OFF)
		brightness = LED_OFF;
	else if (brightness > LED_FULL)
		brightness = LED_FULL;

	pr_debug("%s: temp: %lu, br: %u, led_br: %u\n", __func__,
					temp, br, brightness);

	led_trigger_event(&thermal_led_trigger, brightness);

reschedule:
	delay = (brightness == LED_OFF) ? DELAY_OFF : DELAY_ON;
	schedule_delayed_work(&check_temp_work, delay);
}

static void thermal_trig_early_suspend(struct early_suspend *h)
{
	if (!active)
		return;

	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();

	if (brightness)
		led_trigger_event(&thermal_led_trigger, LED_OFF);

	pr_debug("%s: led_br: %u\n", __func__, brightness);

	return;
}

static void thermal_trig_late_resume(struct early_suspend *h)
{
	if (!active)
		return;

	delay = (brightness == LED_OFF) ? DELAY_OFF : DELAY_ON;
	schedule_delayed_work(&check_temp_work, delay);

	pr_debug("%s: led_br: %u\n", __func__, brightness);

	return;
}

static struct early_suspend thermal_trig_suspend_data = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = thermal_trig_early_suspend,
	.resume = thermal_trig_late_resume,
};

static int __init thermal_trig_init(void)
{
	int ret;
	delay = DELAY_OFF;
	brightness = 0;
	active = 0;

	ret = led_trigger_register(&thermal_led_trigger);
	if (!ret)
		register_early_suspend(&thermal_trig_suspend_data);

	return ret;
}

static void __exit thermal_trig_exit(void)
{
	cancel_delayed_work(&check_temp_work);
	flush_scheduled_work();

	unregister_early_suspend(&thermal_trig_suspend_data);
	led_trigger_unregister(&thermal_led_trigger);
}

module_init(thermal_trig_init);
module_exit(thermal_trig_exit);

MODULE_AUTHOR("Stratos Karafotis <stratosk@semaphore.gr>");
MODULE_DESCRIPTION("Thermal LED trigger");
MODULE_LICENSE("GPL");
