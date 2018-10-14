/*
 *  USB ATI Remote support
 *
 *                Copyright (c) 2011, 2012 Anssi Hannula <anssi.hannula@iki.fi>
 *  Version 2.2.0 Copyright (c) 2004 Torrey Hoffman <thoffman@arnor.net>
 *  Version 2.1.1 Copyright (c) 2002 Vladimir Dergachev
 *
 *  This 2.2.0 version is a rewrite / cleanup of the 2.1.1 driver, including
 *  porting to the 2.6 kernel interfaces, along with other modification
 *  to better match the style of the existing usb/input drivers.  However, the
 *  protocol and hardware handling is essentially unchanged from 2.1.1.
 *
 *  The 2.1.1 driver was derived from the usbati_remote and usbkbd drivers by
 *  Vojtech Pavlik.
 *
 *  Changes:
 *
 *  Feb 2004: Torrey Hoffman <thoffman@arnor.net>
 *            Version 2.2.0
 *  Jun 2004: Torrey Hoffman <thoffman@arnor.net>
 *            Version 2.2.1
 *            Added key repeat support contributed by:
 *                Vincent Vanackere <vanackere@lif.univ-mrs.fr>
 *            Added support for the "Lola" remote contributed by:
 *                Seth Cohn <sethcohn@yahoo.com>
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
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
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 * Hardware & software notes
 *
 * These remote controls are distributed by ATI as part of their
 * "All-In-Wonder" video card packages.  The receiver self-identifies as a
 * "USB Receiver" with manufacturer "X10 Wireless Technology Inc".
 *
 * The "Lola" remote is available from X10.  See:
 *    http://www.x10.com/products/lola_sg1.htm
 * The Lola is similar to the ATI remote but has no mouse support, and slightly
 * different keys.
 *
 * It is possible to use multiple receivers and remotes on multiple computers
 * simultaneously by configuring them to use specific channels.
 *
 * The RF protocol used by the remote supports 16 distinct channels, 1 to 16.
 * Actually, it may even support more, at least in some revisions of the
 * hardware.
 *
 * Each remote can be configured to transmit on one channel as follows:
 *   - Press and hold the "hand icon" button.
 *   - When the red LED starts to blink, let go of the "hand icon" button.
 *   - When it stops blinking, input the channel code as two digits, from 01
 *     to 16, and press the hand icon again.
 *
 * The timing can be a little tricky.  Try loading the module with debug=1
 * to have the kernel print out messages about the remote control number
 * and mask.  Note: debugging prints remote numbers as zero-based hexadecimal.
 *
 * The driver has a "channel_mask" parameter. This bitmask specifies which
 * channels will be ignored by the module.  To mask out channels, just add
 * all the 2^channel_number values together.
 *
 * For instance, set channel_mask = 2^4 = 16 (binary 10000) to make ati_remote
 * ignore signals coming from remote controls transmitting on channel 4, but
 * accept all other channels.
 *
 * Or, set channel_mask = 65533, (0xFFFD), and all channels except 1 will be
 * ignored.
 *
 * The default is 0 (respond to all channels). Bit 0 and bits 17-32 of this
 * parameter are unused.
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb/input.h>
#include <linux/wait.h>
#include <linux/jiffies.h>
#include <media/rc-core.h>
#include "xbox_remote_keymap.h"

/*
 * Module and Version Information, Module Parameters
 */

 /* USB vendor ids for XBOX DVD Dongles */
#define VENDOR_MS1		0x040b
#define VENDOR_MS2		0x045e
#define VENDOR_MS3		0xFFFF

#define DRIVER_VERSION		"2.2.1"
#define DRIVER_AUTHOR           "Torrey Hoffman <thoffman@arnor.net>"
#define DRIVER_DESC             "ATI/X10 RF USB Remote Control"

#define NAME_BUFSIZE      80    /* size of product name, path buffers */
#define DATA_BUFSIZE      63    /* size of URB data buffers */

/*
 * Duplicate event filtering time.
 * Sequential, identical KIND_FILTERED inputs with less than
 * FILTER_TIME milliseconds between them are considered as repeat
 * events. The hardware generates 5 events for the first keypress
 * and we have to take this into account for an accurate repeat
 * behaviour.
 */
#define FILTER_TIME	300 /* msec */
#define REPEAT_DELAY	500 /* msec */

static unsigned long channel_mask;
module_param(channel_mask, ulong, 0644);
MODULE_PARM_DESC(channel_mask, "Bitmask of remote control channels to ignore");

//static int debug;
static int debug = 1;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "Enable extra debug messages and information");

static int repeat_filter = FILTER_TIME;
module_param(repeat_filter, int, 0644);
MODULE_PARM_DESC(repeat_filter, "Repeat filter time, default = 60 msec");

static int repeat_delay = REPEAT_DELAY;
module_param(repeat_delay, int, 0644);
MODULE_PARM_DESC(repeat_delay, "Delay before sending repeats, default = 500 msec");

//static bool mouse = false;
static bool mouse = true;
module_param(mouse, bool, 0444);
MODULE_PARM_DESC(mouse, "Enable mouse device, default = yes");

#define dbginfo(dev, format, arg...) \
	do { if (debug) dev_info(dev , format , ## arg); } while (0)
#undef err
#define err(format, arg...) printk(KERN_ERR format , ## arg)

struct xbox_receiver_type {
	/* either default_keymap or get_default_keymap should be set */
	const char *default_keymap;
	const char *(*get_default_keymap)(struct usb_interface *interface);
};

//static const char *get_medion_keymap(struct usb_interface *interface)
//{
//	struct usb_device *udev = interface_to_usbdev(interface);
//	return RC_MAP_XBOX;
//}

static const struct xbox_receiver_type type_xbox	= {
	.default_keymap = RC_MAP_XBOX
};


static const struct usb_device_id xbox_remote_table[] = {
	
    /* Gamester Xbox DVD Movie Playback Kit IR */
	{
        USB_DEVICE(VENDOR_MS1, 0x6521),
        .driver_info = (unsigned long)&type_xbox     
    },

	/* Microsoft Xbox DVD Movie Playback Kit IR */
	{ 
        USB_DEVICE(VENDOR_MS2, 0x0284),
        .driver_info = (unsigned long)&type_xbox
    }, 

	/*
	 * Some Chinese manufacturer -- conflicts with the joystick from the
	 * same manufacturer
	 */
	{ USB_DEVICE(VENDOR_MS3, 0xFFFF) },

	/* Terminating entry */
	{ }
};

MODULE_DEVICE_TABLE(usb, xbox_remote_table);

/* Get hi and low bytes of a 16-bits int */
#define HI(a)	((unsigned char)((a) >> 8))
#define LO(a)	((unsigned char)((a) & 0xff))

#define SEND_FLAG_IN_PROGRESS	1
#define SEND_FLAG_COMPLETE	2

/* Device initialization strings */
//static char init1[] = { 0x01, 0x00, 0x20, 0x14 };
//static char init2[] = { 0x01, 0x00, 0x20, 0x14, 0x20, 0x20, 0x20 };

struct xbox_remote {
	struct input_dev *idev;
	struct rc_dev *rdev;
	struct usb_device *udev;
	struct usb_interface *interface;

	struct urb *irq_urb;
	struct urb *out_urb;
	struct usb_endpoint_descriptor *endpoint_in;
	struct usb_endpoint_descriptor *endpoint_out;
	unsigned char *inbuf;
	unsigned char *outbuf;
	dma_addr_t inbuf_dma;
	dma_addr_t outbuf_dma;

	unsigned char old_data;     /* Detect duplicate events */
	unsigned long old_jiffies;
	unsigned long acc_jiffies;  /* handle acceleration */
	unsigned long first_jiffies;

	unsigned int repeat_count;

	char rc_name[NAME_BUFSIZE];
	char rc_phys[NAME_BUFSIZE];
	char mouse_name[NAME_BUFSIZE];
	char mouse_phys[NAME_BUFSIZE];

	wait_queue_head_t wait;
	int send_flags;

	int users; /* 0-2, users are rc and input */
	struct mutex open_mutex;
};

/* "Kinds" of messages sent from the hardware to the driver. */
#define KIND_END        0
#define KIND_LITERAL    1   /* Simply pass to input system as EV_KEY */
#define KIND_FILTERED   2   /* Add artificial key-up events, drop keyrepeats */
#define KIND_ACCEL      3   /* Translate to EV_REL mouse-move events */

/* Translation table from hardware messages to input events. */
static const struct {
	unsigned char kind;
	unsigned char data;	/* Raw key code from remote */
	unsigned short code;	/* Input layer translation */
}  xbox_remote_tbl[] = {
	/* Directional control pad axes.  Code is xxyy */
	{KIND_ACCEL,    0x70, 0xff00},	/* left */
	{KIND_ACCEL,    0x71, 0x0100},	/* right */
	{KIND_ACCEL,    0x72, 0x00ff},	/* up */
	{KIND_ACCEL,    0x73, 0x0001},	/* down */

	/* Directional control pad diagonals */
	{KIND_ACCEL,    0x74, 0xffff},	/* left up */
	{KIND_ACCEL,    0x75, 0x01ff},	/* right up */
	{KIND_ACCEL,    0x77, 0xff01},	/* left down */
	{KIND_ACCEL,    0x76, 0x0101},	/* right down */

	/* "Mouse button" buttons.  The code below uses the fact that the
	 * lsbit of the raw code is a down/up indicator. */
	{KIND_LITERAL,  0x78, BTN_LEFT}, /* left btn down */
	{KIND_LITERAL,  0x79, BTN_LEFT}, /* left btn up */
	{KIND_LITERAL,  0x7c, BTN_RIGHT},/* right btn down */
	{KIND_LITERAL,  0x7d, BTN_RIGHT},/* right btn up */

	/* Artificial "doubleclick" events are generated by the hardware.
	 * They are mapped to the "side" and "extra" mouse buttons here. */
	{KIND_FILTERED, 0x7a, BTN_SIDE}, /* left dblclick */
	{KIND_FILTERED, 0x7e, BTN_EXTRA},/* right dblclick */

	/* Non-mouse events are handled by rc-core */
	{KIND_END, 0x00, 0}
};

/*
 * xbox_remote_dump_input
 */
static void xbox_remote_dump(struct device *dev, unsigned char *data,
			    unsigned int len)
{
	if (len == 1) {
		if (data[0] != (unsigned char)0xff && data[0] != 0x00)
			dev_warn(dev, "Weird byte 0x%02x\n", data[0]);
	} else if (len == 4)
		dev_warn(dev, "Weird key %*ph\n", 4, data);
	else {
		//dev_warn(dev, "Weird data, len=%d %*ph ...\n", len, 6, data);
    }
}

/*
 * xbox_remote_open
 */
static int xbox_remote_open(struct xbox_remote *xbox_remote)
{
	int err = 0;

	mutex_lock(&xbox_remote->open_mutex);

	if (xbox_remote->users++ != 0)
		goto out; /* one was already active */

	/* On first open, submit the read urb which was set up previously. */
	xbox_remote->irq_urb->dev = xbox_remote->udev;
	if (usb_submit_urb(xbox_remote->irq_urb, GFP_KERNEL)) {
		dev_err(&xbox_remote->interface->dev,
			"%s: usb_submit_urb failed!\n", __func__);
		err = -EIO;
	}

out:	mutex_unlock(&xbox_remote->open_mutex);
	return err;
}

/*
 * xbox_remote_close
 */
static void xbox_remote_close(struct xbox_remote *xbox_remote)
{
	mutex_lock(&xbox_remote->open_mutex);
	if (--xbox_remote->users == 0)
		usb_kill_urb(xbox_remote->irq_urb);
	mutex_unlock(&xbox_remote->open_mutex);
}

static int xbox_remote_input_open(struct input_dev *inputdev)
{
	struct xbox_remote *xbox_remote = input_get_drvdata(inputdev);
	return xbox_remote_open(xbox_remote);
}

static void xbox_remote_input_close(struct input_dev *inputdev)
{
	struct xbox_remote *xbox_remote = input_get_drvdata(inputdev);
	xbox_remote_close(xbox_remote);
}

static int xbox_remote_rc_open(struct rc_dev *rdev)
{
	struct xbox_remote *xbox_remote = rdev->priv;
	return xbox_remote_open(xbox_remote);
}

static void xbox_remote_rc_close(struct rc_dev *rdev)
{
	struct xbox_remote *xbox_remote = rdev->priv;
	xbox_remote_close(xbox_remote);
}

///*
// * xbox_remote_irq_out
// */
//static void xbox_remote_irq_out(struct urb *urb)
//{
//	struct xbox_remote *xbox_remote = urb->context;
//
//	if (urb->status) {
//		dev_dbg(&xbox_remote->interface->dev, "%s: status %d\n",
//			__func__, urb->status);
//		return;
//	}
//
//	xbox_remote->send_flags |= SEND_FLAG_COMPLETE;
//	wmb();
//	wake_up(&xbox_remote->wait);
//}

///*
// * xbox_remote_sendpacket
// *
// * Used to send device initialization strings
// */
//static int xbox_remote_sendpacket(struct xbox_remote *xbox_remote, u16 cmd,
//	unsigned char *data)
//{
//	int retval = 0;
//
//	/* Set up out_urb */
//	memcpy(xbox_remote->out_urb->transfer_buffer + 1, data, LO(cmd));
//	((char *) xbox_remote->out_urb->transfer_buffer)[0] = HI(cmd);
//
//	xbox_remote->out_urb->transfer_buffer_length = LO(cmd) + 1;
//	xbox_remote->out_urb->dev = xbox_remote->udev;
//	xbox_remote->send_flags = SEND_FLAG_IN_PROGRESS;
//
//	retval = usb_submit_urb(xbox_remote->out_urb, GFP_ATOMIC);
//	if (retval) {
//		dev_dbg(&xbox_remote->interface->dev,
//			 "sendpacket: usb_submit_urb failed: %d\n", retval);
//		return retval;
//	}
//
//	wait_event_timeout(xbox_remote->wait,
//		((xbox_remote->out_urb->status != -EINPROGRESS) ||
//			(xbox_remote->send_flags & SEND_FLAG_COMPLETE)),
//		HZ);
//	usb_kill_urb(xbox_remote->out_urb);
//
//	return retval;
//}

struct accel_times {
	const char	value;
	unsigned int	msecs;
};

static const struct accel_times accel[] = {
	{  1,  125 },
	{  2,  250 },
	{  4,  500 },
	{  6, 1000 },
	{  9, 1500 },
	{ 13, 2000 },
	{ 20,    0 },
};

///*
// * xbox_remote_compute_accel
// *
// * Implements acceleration curve for directional control pad
// * If elapsed time since last event is > 1/4 second, user "stopped",
// * so reset acceleration. Otherwise, user is probably holding the control
// * pad down, so we increase acceleration, ramping up over two seconds to
// * a maximum speed.
// */
//static int xbox_remote_compute_accel(struct xbox_remote *xbox_remote)
//{
//	unsigned long now = jiffies, reset_time;
//	int i;
//
//	reset_time = msecs_to_jiffies(250);
//
//	if (time_after(now, xbox_remote->old_jiffies + reset_time)) {
//		xbox_remote->acc_jiffies = now;
//		return 1;
//	}
//	for (i = 0; i < ARRAY_SIZE(accel) - 1; i++) {
//		unsigned long timeout = msecs_to_jiffies(accel[i].msecs);
//
//		if (time_before(now, xbox_remote->acc_jiffies + timeout))
//			return accel[i].value;
//	}
//	return accel[i].value;
//}

/*
 * xbox_remote_report_input
 */
static void xbox_remote_input_report(struct urb *urb)
{
	struct xbox_remote *xbox_remote = urb->context;
	unsigned char *data= xbox_remote->inbuf;
	struct input_dev *dev = xbox_remote->idev;
	int index = -1;
	int remote_num;
	unsigned char scancode;
	u32 wheel_keycode = KEY_RESERVED;
	int i;

	/* Deal with strange looking inputs */
	if (urb->actual_length != 6 
        || data[0] != 0x00
	    ||  data[1] != 0x06
	    ||  data[3] != 0x0a
       )
    {
		xbox_remote_dump(&urb->dev->dev, data, urb->actual_length);
		return;
	}
        
	xbox_remote_dump(&urb->dev->dev, data, urb->actual_length);
    scancode = data[2];

    unsigned long now = jiffies;
	
    dbginfo(
            &xbox_remote->interface->dev,
		    "time: %lu key data %02x, scancode %02x\n",
            jiffies_to_msecs(now),
            data[2], 
            scancode
           );

    //dbginfo(&xbox_remote->interface->dev, "index: %d", index);
        
    if (index < 0) {
	   
        //dbginfo(&xbox_remote->interface->dev, 
        //        "status: data: %02X | old_data: %02X", 
        //        data[2], xbox_remote->old_data
        //        );
        //
        //dbginfo(&xbox_remote->interface->dev, 
        //        "status: now: %lu | next jff: %lu", 
        //        now, xbox_remote->old_jiffies + msecs_to_jiffies(repeat_filter)
        //        );
        //
        //
        //dbginfo(&xbox_remote->interface->dev, 
        //        "status: old: %lu | flt: %lu", 
        //        now, xbox_remote->old_jiffies,  msecs_to_jiffies(repeat_filter)
        //        );

        //if data is the old one 
        //and
        //now is before old_time + repeat filter
		if (xbox_remote->old_data == data[2] &&
		    time_before(now, xbox_remote->old_jiffies +
				     msecs_to_jiffies(repeat_filter))) 
        {
			xbox_remote->repeat_count++;
            //dbginfo(&xbox_remote->interface->dev, "filtering %d", xbox_remote->repeat_count);
		} 
        else {
			xbox_remote->repeat_count = 0;
			xbox_remote->old_jiffies = now;
			xbox_remote->first_jiffies = now;
			xbox_remote->old_data = data[2];
            //dbginfo(&xbox_remote->interface->dev, "passing");
		}







//        //dbginfo(&xbox_remote->interface->dev, "filtering");
//        
//
//		/* Filter duplicate events which happen "too close" together. */
//		if (xbox_remote->old_data == data[2] &&
//		    time_before(now, xbox_remote->old_jiffies +
//				     msecs_to_jiffies(repeat_filter))) 
//        {
//			xbox_remote->repeat_count++;
//            dbginfo(&xbox_remote->interface->dev, "filtering");
//		} 
//        else {
//			xbox_remote->repeat_count = 0;
//			//xbox_remote->first_jiffies = now;
//			xbox_remote->old_jiffies = now;
//			xbox_remote->old_data = data[2];
//            dbginfo(&xbox_remote->interface->dev, "passing");
//		}
//        
        //dbginfo(&xbox_remote->interface->dev, "now: %d", now);

		//xbox_remote->old_jiffies = now;

		/* Ensure we skip at least the 4 first duplicate events
		 * (generated by a single keypress), and continue skipping
		 * until repeat_delay msecs have passed.
		 */
		if (xbox_remote->repeat_count > 0 &&
		    (xbox_remote->repeat_count < 5 ||
		     time_before(now, xbox_remote->first_jiffies +
				      msecs_to_jiffies(repeat_delay))))
			return;

		//if (index >= 0) {
		//if (index >= 0) {
			//input_event(dev, EV_KEY, xbox_remote_tbl[index].code, 1);
			//input_event(dev, EV_KEY, xbox_remote_tbl[index].code, 0);
			
            
        //    dbginfo(&xbox_remote->interface->dev, "input event: %x", scancode);
        //    dbginfo(&xbox_remote->interface->dev, "dev: %p", dev);

		//}
        
        //input_event(dev, EV_KEY, scancode, 1);
		//input_event(dev, EV_KEY, scancode, 0);
        
        rc_keydown_notimeout(xbox_remote->rdev,
						     RC_PROTO_OTHER,
						     scancode, data[2]);
		rc_keyup(xbox_remote->rdev);
    }
	
    input_sync(dev);
}

	//if (scancode >= 0x70) {
	//	/*
	//	 * This is either a mouse or scrollwheel event, depending on
	//	 * the remote/keymap.
	//	 * Get the keycode assigned to scancode 0x78/0x70. If it is
	//	 * set, assume this is a scrollwheel up/down event.
	//	 */
	//	wheel_keycode = rc_g_keycode_from_table(xbox_remote->rdev,
	//						scancode & 0x78);

	//	if (wheel_keycode == KEY_RESERVED) {
	//		/* scrollwheel was not mapped, assume mouse */

	//		/* Look up event code index in the mouse translation
	//		 * table.
	//		 */
	//		for (i = 0; xbox_remote_tbl[i].kind != KIND_END; i++) {
	//			if (scancode == xbox_remote_tbl[i].data) {
	//				index = i;
	//				break;
	//			}
	//		}
	//	}
	//}

	//if (index >= 0 && xbox_remote_tbl[index].kind == KIND_LITERAL) {
	//	/*
	//	 * The lsbit of the raw key code is a down/up flag.
	//	 * Invert it to match the input layer's conventions.
	//	 */
	//	input_event(dev, EV_KEY, xbox_remote_tbl[index].code,
	//		!(data[2] & 1));

	//	xbox_remote->old_jiffies = jiffies;

	//} else

    //    //if (index < 0 || xbox_remote_tbl[index].kind == KIND_FILTERED) {
    //    if (index < 0) {
	//	unsigned long now = jiffies;

	//	/* Filter duplicate events which happen "too close" together. */
	//	if (xbox_remote->old_data == data[2] &&
	//	    time_before(now, xbox_remote->old_jiffies +
	//			     msecs_to_jiffies(repeat_filter))) {
	//		xbox_remote->repeat_count++;
	//	} else {
	//		xbox_remote->repeat_count = 0;
	//		xbox_remote->first_jiffies = now;
	//	}

	//	xbox_remote->old_jiffies = now;

	//	/* Ensure we skip at least the 4 first duplicate events
	//	 * (generated by a single keypress), and continue skipping
	//	 * until repeat_delay msecs have passed.
	//	 */
	//	if (xbox_remote->repeat_count > 0 &&
	//	    (xbox_remote->repeat_count < 5 ||
	//	     time_before(now, xbox_remote->first_jiffies +
	//			      msecs_to_jiffies(repeat_delay))))
	//		return;

	//	if (index >= 0) {
	//		//input_event(dev, EV_KEY, xbox_remote_tbl[index].code, 1);
	//		//input_event(dev, EV_KEY, xbox_remote_tbl[index].code, 0);
	//		input_event(dev, EV_KEY, scancode, 1);
	//		input_event(dev, EV_KEY, scancode, 0);
	//	}
    //    
    //    }
    //    
//        //MOUSE
//        //######################################################
//
//        else {
//			/* Not a mouse event, hand it to rc-core. */
//			int count = 1;
//
//			if (wheel_keycode != KEY_RESERVED) {
//				/*
//				 * This is a scrollwheel event, send the
//				 * scroll up (0x78) / down (0x70) scancode
//				 * repeatedly as many times as indicated by
//				 * rest of the scancode.
//				 */
//				count = (scancode & 0x07) + 1;
//				scancode &= 0x78;
//			}
//
//			while (count--) {
//				/*
//				* We don't use the rc-core repeat handling yet as
//				* it would cause ghost repeats which would be a
//				* regression for this driver.
//				*/
//				rc_keydown_notimeout(xbox_remote->rdev,
//						     RC_PROTO_OTHER,
//						     scancode, data[2]);
//				rc_keyup(xbox_remote->rdev);
//			}
//			goto nosync;
//		}
//
//	} else if (xbox_remote_tbl[index].kind == KIND_ACCEL) {
//		signed char dx = xbox_remote_tbl[index].code >> 8;
//		signed char dy = xbox_remote_tbl[index].code & 255;
//
//		/*
//		 * Other event kinds are from the directional control pad, and
//		 * have an acceleration factor applied to them.  Without this
//		 * acceleration, the control pad is mostly unusable.
//		 */
//		int acc = xbox_remote_compute_accel(xbox_remote);
//		if (dx)
//			input_report_rel(dev, REL_X, dx * acc);
//		if (dy)
//			input_report_rel(dev, REL_Y, dy * acc);
//		xbox_remote->old_jiffies = jiffies;
//
//	} else {
//		dev_dbg(&xbox_remote->interface->dev, "xbox_remote kind=%d\n",
//			xbox_remote_tbl[index].kind);
//		return;
//	}
    

/*
 * xbox_remote_irq_in
 */
static void xbox_remote_irq_in(struct urb *urb)
{
	struct xbox_remote *xbox_remote = urb->context;
	int retval;

	switch (urb->status) {
	case 0:			/* success */
		xbox_remote_input_report(urb);
		break;
	case -ECONNRESET:	/* unlink */
	case -ENOENT:
	case -ESHUTDOWN:
		dev_dbg(&xbox_remote->interface->dev,
			"%s: urb error status, unlink?\n",
			__func__);
		return;
	default:		/* error */
		dev_dbg(&xbox_remote->interface->dev,
			"%s: Nonzero urb status %d\n",
			__func__, urb->status);
	}

	retval = usb_submit_urb(urb, GFP_ATOMIC);
	if (retval)
		dev_err(&xbox_remote->interface->dev,
			"%s: usb_submit_urb()=%d\n",
			__func__, retval);
}

/*
 * xbox_remote_alloc_buffers
 */
static int xbox_remote_alloc_buffers(struct usb_device *udev,
				    struct xbox_remote *xbox_remote)
{
	xbox_remote->inbuf = usb_alloc_coherent(udev, DATA_BUFSIZE, GFP_ATOMIC,
					       &xbox_remote->inbuf_dma);
	if (!xbox_remote->inbuf)
		return -1;

	xbox_remote->outbuf = usb_alloc_coherent(udev, DATA_BUFSIZE, GFP_ATOMIC,
						&xbox_remote->outbuf_dma);
	if (!xbox_remote->outbuf)
		return -1;

	xbox_remote->irq_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!xbox_remote->irq_urb)
		return -1;

	xbox_remote->out_urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!xbox_remote->out_urb)
		return -1;

	return 0;
}

/*
 * xbox_remote_free_buffers
 */
static void xbox_remote_free_buffers(struct xbox_remote *xbox_remote)
{
	usb_free_urb(xbox_remote->irq_urb);
	usb_free_urb(xbox_remote->out_urb);

	usb_free_coherent(xbox_remote->udev, DATA_BUFSIZE,
		xbox_remote->inbuf, xbox_remote->inbuf_dma);

	usb_free_coherent(xbox_remote->udev, DATA_BUFSIZE,
		xbox_remote->outbuf, xbox_remote->outbuf_dma);
}

static void xbox_remote_input_init(struct xbox_remote *xbox_remote)
{
	struct input_dev *idev = xbox_remote->idev;
	int i;

	idev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);
	idev->keybit[BIT_WORD(BTN_MOUSE)] = BIT_MASK(BTN_LEFT) |
		BIT_MASK(BTN_RIGHT) | BIT_MASK(BTN_SIDE) | BIT_MASK(BTN_EXTRA);
	idev->relbit[0] = BIT_MASK(REL_X) | BIT_MASK(REL_Y);
	for (i = 0; xbox_remote_tbl[i].kind != KIND_END; i++)
		if (xbox_remote_tbl[i].kind == KIND_LITERAL ||
		    xbox_remote_tbl[i].kind == KIND_FILTERED)
			__set_bit(xbox_remote_tbl[i].code, idev->keybit);

	input_set_drvdata(idev, xbox_remote);

	idev->open = xbox_remote_input_open;
	idev->close = xbox_remote_input_close;

	idev->name = xbox_remote->mouse_name;
	idev->phys = xbox_remote->mouse_phys;

	usb_to_input_id(xbox_remote->udev, &idev->id);
	idev->dev.parent = &xbox_remote->interface->dev;
}

static void xbox_remote_rc_init(struct xbox_remote *xbox_remote)
{
	struct rc_dev *rdev = xbox_remote->rdev;

	rdev->priv = xbox_remote;
	rdev->allowed_protocols = RC_PROTO_BIT_OTHER;
	rdev->driver_name = "xbox_remote";

	rdev->open = xbox_remote_rc_open;
	rdev->close = xbox_remote_rc_close;

	rdev->device_name = xbox_remote->rc_name;
	rdev->input_phys = xbox_remote->rc_phys;

	usb_to_input_id(xbox_remote->udev, &rdev->input_id);
	rdev->dev.parent = &xbox_remote->interface->dev;
}

static int xbox_remote_initialize(struct xbox_remote *xbox_remote)
{
	struct usb_device *udev = xbox_remote->udev;
	int pipe, maxp;

	init_waitqueue_head(&xbox_remote->wait);

	/* Set up irq_urb */
	pipe = usb_rcvintpipe(udev, xbox_remote->endpoint_in->bEndpointAddress);
	maxp = usb_maxpacket(udev, pipe, usb_pipeout(pipe));
	maxp = (maxp > DATA_BUFSIZE) ? DATA_BUFSIZE : maxp;

	usb_fill_int_urb(xbox_remote->irq_urb, udev, pipe, xbox_remote->inbuf,
			 maxp, xbox_remote_irq_in, xbox_remote,
			 xbox_remote->endpoint_in->bInterval);
	xbox_remote->irq_urb->transfer_dma = xbox_remote->inbuf_dma;
	xbox_remote->irq_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	///* Set up out_urb */
	//pipe = usb_sndintpipe(udev, xbox_remote->endpoint_out->bEndpointAddress);
	//maxp = usb_maxpacket(udev, pipe, usb_pipeout(pipe));
	//maxp = (maxp > DATA_BUFSIZE) ? DATA_BUFSIZE : maxp;

	//usb_fill_int_urb(xbox_remote->out_urb, udev, pipe, xbox_remote->outbuf,
	//		 maxp, xbox_remote_irq_out, xbox_remote,
	//		 xbox_remote->endpoint_out->bInterval);
	//xbox_remote->out_urb->transfer_dma = xbox_remote->outbuf_dma;
	//xbox_remote->out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

	///* send initialization strings */
	//if ((xbox_remote_sendpacket(xbox_remote, 0x8004, init1)) ||
	//    (xbox_remote_sendpacket(xbox_remote, 0x8007, init2))) {
	//	dev_err(&xbox_remote->interface->dev,
	//		 "Initializing xbox_remote hardware failed.\n");
	//	return -EIO;
	//}

	return 0;
}

/*
 * xbox_remote_probe
 */
static int xbox_remote_probe(struct usb_interface *interface,
	const struct usb_device_id *id)
{
	struct usb_device *udev = interface_to_usbdev(interface);
	struct usb_host_interface *iface_host = interface->cur_altsetting;
	struct usb_endpoint_descriptor *endpoint_in, *endpoint_out;
	struct xbox_receiver_type *type = (struct xbox_receiver_type *)id->driver_info;
	struct xbox_remote *xbox_remote;
	struct input_dev *input_dev;
	struct rc_dev *rc_dev;
	int err = -ENOMEM;

	if (iface_host->desc.bNumEndpoints != 1) {
		err("%s: Unexpected desc.bNumEndpoints\n", __func__);
		return -ENODEV;
	}

	endpoint_in = &iface_host->endpoint[0].desc;
	//endpoint_out = &iface_host->endpoint[1].desc;

	if (!usb_endpoint_is_int_in(endpoint_in)) {
		err("%s: Unexpected endpoint_in\n", __func__);
		return -ENODEV;
	}
	if (le16_to_cpu(endpoint_in->wMaxPacketSize) == 0) {
		err("%s: endpoint_in message size==0? \n", __func__);
		return -ENODEV;
	}

	xbox_remote = kzalloc(sizeof (struct xbox_remote), GFP_KERNEL);
	rc_dev = rc_allocate_device(RC_DRIVER_SCANCODE);
	if (!xbox_remote || !rc_dev)
		goto exit_free_dev_rdev;

	/* Allocate URB buffers, URBs */
	if (xbox_remote_alloc_buffers(udev, xbox_remote))
		goto exit_free_buffers;

	xbox_remote->endpoint_in = endpoint_in;
	//xbox_remote->endpoint_out = endpoint_out;
	xbox_remote->udev = udev;
	xbox_remote->rdev = rc_dev;
	xbox_remote->interface = interface;

	usb_make_path(udev, xbox_remote->rc_phys, sizeof(xbox_remote->rc_phys));
	strlcpy(xbox_remote->mouse_phys, xbox_remote->rc_phys,
		sizeof(xbox_remote->mouse_phys));

	strlcat(xbox_remote->rc_phys, "/input0", sizeof(xbox_remote->rc_phys));
	strlcat(xbox_remote->mouse_phys, "/input1", sizeof(xbox_remote->mouse_phys));

	snprintf(xbox_remote->rc_name, sizeof(xbox_remote->rc_name), "%s%s%s",
		udev->manufacturer ?: "",
		udev->manufacturer && udev->product ? " " : "",
		udev->product ?: "");

	if (!strlen(xbox_remote->rc_name))
		snprintf(xbox_remote->rc_name, sizeof(xbox_remote->rc_name),
			DRIVER_DESC "(%04x,%04x)",
			le16_to_cpu(xbox_remote->udev->descriptor.idVendor),
			le16_to_cpu(xbox_remote->udev->descriptor.idProduct));

	snprintf(xbox_remote->mouse_name, sizeof(xbox_remote->mouse_name),
		 "%s mouse", xbox_remote->rc_name);

	rc_dev->map_name = RC_MAP_XBOX; /* default map */

	/* set default keymap according to receiver model */
	if (type) {
		if (type->default_keymap)
			rc_dev->map_name = type->default_keymap;
		else if (type->get_default_keymap)
			rc_dev->map_name = type->get_default_keymap(interface);
	}

	xbox_remote_rc_init(xbox_remote);
	mutex_init(&xbox_remote->open_mutex);

	/* Device Hardware Initialization - fills in xbox_remote->idev from udev. */
	err = xbox_remote_initialize(xbox_remote);
	if (err)
		goto exit_kill_urbs;

	/* Set up and register rc device */
	err = rc_register_device(xbox_remote->rdev);
	if (err)
		goto exit_kill_urbs;

	/* Set up and register mouse input device */
	if (mouse) {
		input_dev = input_allocate_device();
		if (!input_dev) {
			err = -ENOMEM;
			goto exit_unregister_device;
		}

		xbox_remote->idev = input_dev;
		xbox_remote_input_init(xbox_remote);
		err = input_register_device(input_dev);

		if (err)
			goto exit_free_input_device;
	}

	usb_set_intfdata(interface, xbox_remote);
	return 0;

 exit_free_input_device:
	input_free_device(input_dev);
 exit_unregister_device:
	rc_unregister_device(rc_dev);
	rc_dev = NULL;
 exit_kill_urbs:
	usb_kill_urb(xbox_remote->irq_urb);
	usb_kill_urb(xbox_remote->out_urb);
 exit_free_buffers:
	xbox_remote_free_buffers(xbox_remote);
 exit_free_dev_rdev:
	 rc_free_device(rc_dev);
	kfree(xbox_remote);
	return err;
}

/*
 * xbox_remote_disconnect
 */
static void xbox_remote_disconnect(struct usb_interface *interface)
{
	struct xbox_remote *xbox_remote;

	xbox_remote = usb_get_intfdata(interface);
	usb_set_intfdata(interface, NULL);
	if (!xbox_remote) {
		dev_warn(&interface->dev, "%s - null device?\n", __func__);
		return;
	}

	usb_kill_urb(xbox_remote->irq_urb);
	usb_kill_urb(xbox_remote->out_urb);
	if (xbox_remote->idev)
		input_unregister_device(xbox_remote->idev);
	rc_unregister_device(xbox_remote->rdev);
	xbox_remote_free_buffers(xbox_remote);
	kfree(xbox_remote);
}

/* usb specific object to register with the usb subsystem */
static struct usb_driver xbox_remote_driver = {
	.name         = "xbox_remote",
	.probe        = xbox_remote_probe,
	.disconnect   = xbox_remote_disconnect,
	.id_table     = xbox_remote_table,
};

module_usb_driver(xbox_remote_driver);

MODULE_AUTHOR(DRIVER_AUTHOR);
MODULE_DESCRIPTION(DRIVER_DESC);
MODULE_LICENSE("GPL");
