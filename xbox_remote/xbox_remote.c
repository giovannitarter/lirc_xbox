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
#define VENDOR_MS1      0x040b
#define VENDOR_MS2      0x045e
#define VENDOR_MS3      0xFFFF

#define DRIVER_VERSION          "1.0"
#define DRIVER_AUTHOR           "Giovanni Tarter <giovanni.tarter@gmail.com>"
#define DRIVER_DESC             "XBox DVD Remote"

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
#define FILTER_TIME 300 /* msec */
#define REPEAT_DELAY    500 /* msec */

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

#define dbginfo(dev, format, arg...) \
    do { if (debug) dev_info(dev , format , ## arg); } while (0)
#undef err
#define err(format, arg...) printk(KERN_ERR format , ## arg)


static const struct usb_device_id xbox_remote_table[] = {
    
    
    /* Gamester Xbox DVD Movie Playback Kit IR */
    {
        USB_DEVICE(VENDOR_MS1, 0x6521)
    },

    /* Microsoft Xbox DVD Movie Playback Kit IR */
    { 
        USB_DEVICE(VENDOR_MS2, 0x0284)
    }, 

    /*
     * Some Chinese manufacturer -- conflicts with the joystick from the
     * same manufacturer
     */
    { 
        USB_DEVICE(VENDOR_MS3, 0xFFFF) 
    },

    /* Terminating entry */
    { }
};

MODULE_DEVICE_TABLE(usb, xbox_remote_table);

/* Get hi and low bytes of a 16-bits int */
#define HI(a)   ((unsigned char)((a) >> 8))
#define LO(a)   ((unsigned char)((a) & 0xff))

#define SEND_FLAG_IN_PROGRESS   1
#define SEND_FLAG_COMPLETE  2

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

    wait_queue_head_t wait;
    int send_flags;

    int users; /* 0-2, users are rc and input */
    struct mutex open_mutex;
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

out:    mutex_unlock(&xbox_remote->open_mutex);
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

    if (index < 0) {
       
        //if data is the old one 
        //and
        //now is before old_time + repeat filter
        if (xbox_remote->old_data == data[2] &&
            time_before(now, xbox_remote->old_jiffies +
                     msecs_to_jiffies(repeat_filter))) 
        {
            xbox_remote->repeat_count++;
        } 
        else {
            xbox_remote->repeat_count = 0;
            xbox_remote->old_jiffies = now;
            xbox_remote->first_jiffies = now;
            xbox_remote->old_data = data[2];
        }

        /* Ensure we skip at least the 4 first duplicate events
         * (generated by a single keypress), and continue skipping
         * until repeat_delay msecs have passed.
         */
        if (xbox_remote->repeat_count > 0 &&
            (xbox_remote->repeat_count < 5 ||
             time_before(now, xbox_remote->first_jiffies +
                      msecs_to_jiffies(repeat_delay))))
            return;

        rc_keydown_notimeout(xbox_remote->rdev,
                             RC_PROTO_OTHER,
                             scancode, data[2]);
        rc_keyup(xbox_remote->rdev);
    }
    
    input_sync(dev);
}


/*
 * xbox_remote_irq_in
 */
static void xbox_remote_irq_in(struct urb *urb)
{
    struct xbox_remote *xbox_remote = urb->context;
    int retval;

    switch (urb->status) {
    case 0:         /* success */
        xbox_remote_input_report(urb);
        break;
    case -ECONNRESET:   /* unlink */
    case -ENOENT:
    case -ESHUTDOWN:
        dev_dbg(&xbox_remote->interface->dev,
            "%s: urb error status, unlink?\n",
            __func__);
        return;
    default:        /* error */
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

    idev->evbit[0] = BIT_MASK(EV_KEY) | BIT_MASK(EV_REL);

    input_set_drvdata(idev, xbox_remote);

    idev->open = xbox_remote_input_open;
    idev->close = xbox_remote_input_close;

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
    //       maxp, xbox_remote_irq_out, xbox_remote,
    //       xbox_remote->endpoint_out->bInterval);
    //xbox_remote->out_urb->transfer_dma = xbox_remote->outbuf_dma;
    //xbox_remote->out_urb->transfer_flags |= URB_NO_TRANSFER_DMA_MAP;

    ///* send initialization strings */
    //if ((xbox_remote_sendpacket(xbox_remote, 0x8004, init1)) ||
    //    (xbox_remote_sendpacket(xbox_remote, 0x8007, init2))) {
    //  dev_err(&xbox_remote->interface->dev,
    //       "Initializing xbox_remote hardware failed.\n");
    //  return -EIO;
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
    struct xbox_remote *xbox_remote;
    struct input_dev *input_dev;
    struct rc_dev *rc_dev;
    int err = -ENOMEM;

    request_module("xbox_remote_keymap");
    
    if (iface_host->desc.bNumEndpoints == 0) {
        return -ENODEV;
    }

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
    strlcat(xbox_remote->rc_phys, "/input0", sizeof(xbox_remote->rc_phys));

    snprintf(xbox_remote->rc_name, sizeof(xbox_remote->rc_name), "%s%s%s",
        udev->manufacturer ?: "",
        udev->manufacturer && udev->product ? " " : "",
        udev->product ?: "");

    if (!strlen(xbox_remote->rc_name))
        snprintf(xbox_remote->rc_name, sizeof(xbox_remote->rc_name),
            DRIVER_DESC "(%04x,%04x)",
            le16_to_cpu(xbox_remote->udev->descriptor.idVendor),
            le16_to_cpu(xbox_remote->udev->descriptor.idProduct));

    rc_dev->map_name = RC_MAP_XBOX; /* default map */

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
