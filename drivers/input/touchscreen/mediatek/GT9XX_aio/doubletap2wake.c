/*
 * drivers/input/touchscreen/doubletap2wake.c
 *
 *
 * Copyright (c) 2013, Dennis Rassmann <showp1984@gmail.com>
 * Copyright (c) 2015, Vineeth Raj <contact.twn@openmailbox.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/input.h>
#include <linux/hrtimer.h>
#include <asm-generic/cputime.h>
#include <linux/input/doubletap2wake.h>
#include <linux/pocket_mod.h>
#include "touchscreen.h"
#include <linux/input/smartwake.h>


#ifdef CONFIG_POCKETMOD
#include <linux/pocket_mod.h>
#endif
 
#define WAKE_HOOKS_DEFINED

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
#include <linux/lcd_notify.h>
#else
#include <linux/earlysuspend.h>
#endif
#endif


/* Version, author, desc, etc */
#define DRIVER_AUTHOR "Dennis Rassmann <showp1984@gmail.com>"
#define DRIVER_DESCRIPTION "Doubletap2wake for almost any device"
#define DRIVER_VERSION "1.0"
#define LOGTAG "[doubletap2wake]: "

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESCRIPTION);
MODULE_VERSION(DRIVER_VERSION);
MODULE_LICENSE("GPLv2");

/* Tuneables */
#define DT2W_DEBUG         0
#define DT2W_DEFAULT       1

#define DT2W_PWRKEY_DUR   60
#define DT2W_FEATHER      50
#define DT2W_TIME         600
#define VIB_STRENGTH	  50

extern void set_vibrate(int value);
unsigned int vib_strength = VIB_STRENGTH;
unsigned int Dt2w_regions=0;

/* Resources */
int dt2w_switch = DT2W_DEFAULT;
static cputime64_t tap_time_pre = 0;
static int touch_x = 0, touch_y = 0, touch_nr = 0, x_pre = 0, y_pre = 0;
static bool touch_x_called = false, touch_y_called = false, touch_cnt = true;
static bool exec_count = true;
bool dt2w_scr_suspended = false;
bool in_phone_call = false;
static unsigned long pwrtrigger_time[2] = {0, 0};
#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
static struct notifier_block dt2w_lcd_notif;
#endif
#endif
static struct input_dev * doubletap2wake_pwrdev;
static DEFINE_MUTEX(pwrkeyworklock);
static struct workqueue_struct *dt2w_input_wq;
static struct work_struct dt2w_input_work;

enum support_gesture_e {
	TW_SUPPORT_NONE_SLIDE_WAKEUP = 0x0,
	TW_SUPPORT_UP_SLIDE_WAKEUP = 0x1,
	TW_SUPPORT_DOWN_SLIDE_WAKEUP = 0x2,
	TW_SUPPORT_LEFT_SLIDE_WAKEUP = 0x4,
	TW_SUPPORT_RIGHT_SLIDE_WAKEUP = 0x8,
	TW_SUPPORT_E_SLIDE_WAKEUP = 0x10,
	TW_SUPPORT_O_SLIDE_WAKEUP = 0x20,
	TW_SUPPORT_C_SLIDE_WAKEUP = 0x80,
	TW_SUPPORT_M_SLIDE_WAKEUP = 0x100,
	TW_SUPPORT_DOUBLE_CLICK_WAKEUP = 0x200,

	TW_SUPPORT_GESTURE_IN_ALL = (
			TW_SUPPORT_UP_SLIDE_WAKEUP |
			TW_SUPPORT_DOWN_SLIDE_WAKEUP |
			TW_SUPPORT_LEFT_SLIDE_WAKEUP |
			TW_SUPPORT_RIGHT_SLIDE_WAKEUP |
			TW_SUPPORT_E_SLIDE_WAKEUP |
			TW_SUPPORT_O_SLIDE_WAKEUP |
			TW_SUPPORT_C_SLIDE_WAKEUP |
			TW_SUPPORT_M_SLIDE_WAKEUP |
			TW_SUPPORT_DOUBLE_CLICK_WAKEUP)
};

/* Read cmdline for dt2w */
static int __init read_dt2w_cmdline(char *dt2w)
{
	if (strcmp(dt2w, "1") == 0) {
		pr_info("[cmdline_dt2w]: DoubleTap2Wake enabled. | dt2w='%s'\n", dt2w);
		dt2w_switch = 1;
	} else if (strcmp(dt2w, "0") == 0) {
		pr_info("[cmdline_dt2w]: DoubleTap2Wake disabled. | dt2w='%s'\n", dt2w);
		dt2w_switch = 0;
	} else {
		pr_info("[cmdline_dt2w]: No valid input found. Going with default: | dt2w='%u'\n", dt2w_switch);
	}
	return 1;
}
__setup("dt2w=", read_dt2w_cmdline);

/* reset on finger release */
static void doubletap2wake_reset(void) {
	exec_count = true;
	touch_nr = 0;
	tap_time_pre = 0;
	x_pre = 0;
	y_pre = 0;
}

/* PowerKey work func */
static void doubletap2wake_presspwr(struct work_struct * doubletap2wake_presspwr_work) {
	if (!mutex_trylock(&pwrkeyworklock))
		return;

	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 1);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);
	msleep(DT2W_PWRKEY_DUR);
	input_event(doubletap2wake_pwrdev, EV_KEY, KEY_POWER, 0);
	input_event(doubletap2wake_pwrdev, EV_SYN, 0, 0);
	msleep(DT2W_PWRKEY_DUR);
	mutex_unlock(&pwrkeyworklock);
	return;
}
static DECLARE_WORK(doubletap2wake_presspwr_work, doubletap2wake_presspwr);

/* PowerKey trigger */
static void doubletap2wake_pwrtrigger(void) {
	/*pwrtrigger_time[1] = pwrtrigger_time[0];
        pwrtrigger_time[0] = jiffies;	

	if (pwrtrigger_time[0] - pwrtrigger_time[1] < TRIGGER_TIMEOUT)
		return;*/

	schedule_work(&doubletap2wake_presspwr_work);
	return;
}

/* unsigned */
static unsigned int calc_feather(int coord, int prev_coord) {
	int calc_coord = 0;
	calc_coord = coord-prev_coord;
	if (calc_coord < 0)
		calc_coord = calc_coord * (-1);
	return calc_coord;
}

/* init a new touch */
static void new_touch(int x, int y) {
	tap_time_pre = ktime_to_ms(ktime_get());
	x_pre = x;
	y_pre = y;
	touch_nr++;
}

/* Doubletap2wake main function */
static void detect_doubletap2wake(int x, int y, bool st)
{
	bool single_touch = st;
#if DT2W_DEBUG
	pr_info(LOGTAG"x,y(%4d,%4d) single:%s\n",
		x, y, (single_touch) ? "true" : "false");
#endif
	if ((single_touch) && (dt2w_switch > 0) && (exec_count) && (touch_cnt)) {
		touch_cnt = false;
		if (touch_nr == 0) {
			new_touch(x, y);
		} else if (touch_nr == 1) {
			if ((calc_feather(x, x_pre) < DT2W_FEATHER) &&
			    (calc_feather(y, y_pre) < DT2W_FEATHER) &&
			    ((ktime_to_ms(ktime_get())-tap_time_pre) < DT2W_TIME))
				touch_nr++;
			else {
				doubletap2wake_reset();
				new_touch(x, y);
			}
		} else {
			doubletap2wake_reset();
			new_touch(x, y);
		}
		if ((touch_nr > 1) && (support_gesture & TW_SUPPORT_DOUBLE_CLICK_WAKEUP)) {
			#if DT2W_DEBUG
			pr_info(LOGTAG"------------DTWP 0N----------");
			#endif
 			sprintf(wakeup_slide,"double_click");
			exec_count = false;
			set_vibrate(vib_strength);
			doubletap2wake_pwrtrigger();
			doubletap2wake_reset();
		}
	}
}

static void dt2w_input_callback(struct work_struct *unused) {
#ifdef CONFIG_POCKETMOD
  	if (device_is_pocketed()){
  		return;
  	}
 	else
#endif
//avoid button presses being recognized as touches
//Dt2w_regions=0;
//0 = touch only
//1 = buttons only
//2 = both

	if (touch_y < 1280 && Dt2w_regions==0) {
	detect_doubletap2wake(touch_x, touch_y, true);
    }
    else if (touch_y > 1280 && Dt2w_regions==1) {
	detect_doubletap2wake(touch_x, touch_y, true);
    }
    else if (touch_y < 1920 && Dt2w_regions==2) {
   	detect_doubletap2wake(touch_x, touch_y, true);
    }
	else if (touch_y < 1280) {
	detect_doubletap2wake(touch_x, touch_y, true);
    }


	return;
}

static void dt2w_input_event(struct input_handle *handle, unsigned int type,
				unsigned int code, int value) {
#if DT2W_DEBUG
	pr_info("doubletap2wake: code: %s|%u, val: %i\n",
		((code==ABS_MT_POSITION_X) ? "X" :
		(code==ABS_MT_POSITION_Y) ? "Y" :
		((code==ABS_MT_TRACKING_ID)||
			(code==330)) ? "ID" : "undef"), code, value);
#endif

	if (in_phone_call)
		return;

	if (!(dt2w_scr_suspended))
		return;

	if (code == ABS_MT_SLOT) {
		doubletap2wake_reset();
		return;
	}

	/*
	 * '330'? Many touch panels are 'broken' in the sense of not following the
	 * multi-touch protocol given in Documentation/input/multi-touch-protocol.txt.
	 * According to the docs, touch panels using the type B protocol must send in
	 * a ABS_MT_TRACKING_ID event after lifting the contact in the first slot.
	 * This should in the flow of events, help us set the necessary doubletap2wake
	 * variable and proceed as per the algorithm.
	 *
	 * This however is not the case with various touch panel drivers, and hence
	 * there is no reliable way of tracking ABS_MT_TRACKING_ID on such panels.
	 * Some of the panels however do track the lifting of contact, but with a
	 * different event code, and a different event value.
	 *
	 * So, add checks for those event codes and values to keep the algo flow.
	 *
	 * synaptics_s3203 => code: 330; val: 0
	 *
	 * Note however that this is not possible with panels like the CYTTSP3 panel
	 * where there are no such events being reported for the lifting of contacts
	 * though i2c data has a ABS_MT_TRACKING_ID or equivalent event variable
	 * present. In such a case, make sure the touch_cnt variable is publicly
	 * available for modification.
	 *
	 */
	if ((code == ABS_MT_TRACKING_ID && value == -1) || (code == 330 && value == 0)) {
		touch_cnt = true;
		return;
	}

	if (code == ABS_MT_POSITION_X) {
		touch_x = value;
		touch_x_called = true;
	}

	if (code == ABS_MT_POSITION_Y) {
		touch_y = value;
		touch_y_called = true;
	}

	if (touch_x_called || touch_y_called) {
		touch_x_called = false;
		touch_y_called = false;
		queue_work_on(0, dt2w_input_wq, &dt2w_input_work);
	}
}

static int input_dev_filter(struct input_dev *dev) {
	if (strstr(dev->name, "touch")||
			strstr(dev->name, "mtk-tpd")) {
		return 0;
	} else {
		return 1;
	}
}

static int dt2w_input_connect(struct input_handler *handler,
				struct input_dev *dev, const struct input_device_id *id) {
	struct input_handle *handle;
	int error;

	if (input_dev_filter(dev))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "dt2w";

	error = input_register_handle(handle);
	if (error)
		goto err2;

	error = input_open_device(handle);
	if (error)
		goto err1;

	return 0;
err1:
	input_unregister_handle(handle);
err2:
	kfree(handle);
	return error;
}

static void dt2w_input_disconnect(struct input_handle *handle) {
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id dt2w_ids[] = {
	{ .driver_info = 1 },
	{ },
};

static struct input_handler dt2w_input_handler = {
	.event		= dt2w_input_event,
	.connect	= dt2w_input_connect,
	.disconnect	= dt2w_input_disconnect,
	.name		= "dt2w_inputreq",
	.id_table	= dt2w_ids,
};

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
static int lcd_notifier_callback(struct notifier_block *this,
				unsigned long event, void *data)
{
	switch (event) {
	case LCD_EVENT_ON_END:
		dt2w_scr_suspended = false;
		break;
	case LCD_EVENT_OFF_END:
		dt2w_scr_suspended = true;
		break;
	default:
		break;
	}

	return 0;
}
#else
static void dt2w_early_suspend(struct early_suspend *h) {
	dt2w_scr_suspended = true;
}

static void dt2w_late_resume(struct early_suspend *h) {
	dt2w_scr_suspended = false;
}

static struct early_suspend dt2w_early_suspend_handler = {
	.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN,
	.suspend = dt2w_early_suspend,
	.resume = dt2w_late_resume,
	
static ssize_t vib_strength_show(struct device *dev,
 		 struct device_attribute *attr, char *buf)
 {
 	return snprintf(buf, PAGE_SIZE, "%d\n", vib_strength);
 }
 
 static ssize_t vib_strength_dump(struct device *dev,
 		 struct device_attribute *attr, const char *buf, size_t count)
 {
 	int ret;
 	unsigned long input;
 
 	ret = kstrtoul(buf, 0, &input);
 	if (ret < 0)
 		return ret;
 
 	if (input < 0 || input > 90)
 		input = 20;				
 
 	vib_strength = input;			
 	
 	return count;
 }
 
 static DEVICE_ATTR(vib_strength, (S_IWUSR|S_IRUGO),
 	vib_strength_show, vib_strength_dump);
	
};
#endif
#endif

/*
 * SYSFS stuff below here
 */


/*
 * INIT / EXIT stuff below here
 */
static int __init doubletap2wake_init(void)
{
	int rc = 0;

	doubletap2wake_pwrdev = input_allocate_device();
	if (!doubletap2wake_pwrdev) {
		pr_err("Can't allocate suspend autotest power button\n");
		goto err_alloc_dev;
	}

	input_set_capability(doubletap2wake_pwrdev, EV_KEY, KEY_POWER);
	doubletap2wake_pwrdev->name = "dt2w_pwrkey";
	doubletap2wake_pwrdev->phys = "dt2w_pwrkey/input0";

	rc = input_register_device(doubletap2wake_pwrdev);
	if (rc) {
		pr_err("%s: input_register_device err=%d\n", __func__, rc);
		goto err_input_dev;
	}
	dt2w_input_wq = create_workqueue("dt2wiwq");
	if (!dt2w_input_wq) {
		pr_err("%s: Failed to create dt2wiwq workqueue\n", __func__);
		return -EFAULT;
	}
	INIT_WORK(&dt2w_input_work, dt2w_input_callback);
	rc = input_register_handler(&dt2w_input_handler);
	if (rc)
		pr_err("%s: Failed to register dt2w_input_handler\n", __func__);

#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
	dt2w_lcd_notif.notifier_call = lcd_notifier_callback;
	if (lcd_register_client(&dt2w_lcd_notif) != 0) {
		pr_err("%s: Failed to register lcd callback\n", __func__);
	}
#else
	register_early_suspend(&dt2w_early_suspend_handler);
#endif
#endif

err_input_dev:
	input_free_device(doubletap2wake_pwrdev);
err_alloc_dev:
	pr_info(LOGTAG"%s done\n", __func__);
	return 0;
}

static void __exit doubletap2wake_exit(void)
{
#ifndef WAKE_HOOKS_DEFINED
#ifndef CONFIG_HAS_EARLYSUSPEND
	lcd_unregister_client(&dt2w_lcd_notif);
#endif
#endif
	input_unregister_handler(&dt2w_input_handler);
	destroy_workqueue(dt2w_input_wq);
	input_unregister_device(doubletap2wake_pwrdev);
	input_free_device(doubletap2wake_pwrdev);
	return;
}

module_init(doubletap2wake_init);
module_exit(doubletap2wake_exit);
