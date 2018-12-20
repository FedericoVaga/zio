/* Federico Vaga 2013, GNU GPLv2 or later */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/device.h>
#include <linux/usb.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/version.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

ZIO_PARAM_TRIGGER(zvmk80xx_trigger);
ZIO_PARAM_BUFFER(zvmk80xx_buffer);

enum urb_tx_msg_field {
	VMK8055_DI_REG = 0,
	VMK8055_DO_REG,
	VMK8055_A1_REG,
	VMK8055_A2_REG,
	VMK8055_CNT1_REG,
	VMK8055_CNT2_REG,
};

enum urb_msg_commands {
	VMK8055_CMD_RST =	0x00,
	VMK8055_CMD_DEB1_TIME =	0x01,
	VMK8055_CMD_DEB2_TIME =	0x02,
	VMK8055_CMD_RST_CNT1 =	0x03,
	VMK8055_CMD_RST_CNT2 =	0x04,
	VMK8055_CMD_WRT_AD =	0x05,
};

enum urb_dir {
	URB_RCV_FLAG = 0x1,
	URB_SND_FLAG = 0x2,
};

struct zvmk80xx_dev {
	struct zio_device *zdev;
	struct zio_device *hwzdev;

	/* FROM COMEDI */
	struct usb_device *udev;
	struct usb_interface *intf;
	struct usb_endpoint_descriptor *ep_rx;
	struct usb_endpoint_descriptor *ep_tx;
	struct usb_anchor rx_anchor;
	struct usb_anchor tx_anchor;

	uint8_t *usb_rx_buf;
	uint8_t *usb_tx_buf;
};

/*
 * zvmk80xx_cset
 * @cset: associated cset
 * @timer: used to pilot the I/O
 * @next_run: time of the next I/O operation
 * @period: ms between each I/O operation
 * @urb:
 *
 * @msg_index: index where but data within an URB
 * @pkt_sent: number of sample sent to vmk80xx
 *
 * @pkt_size: URB message size
 */
struct zvmk80xx_cset {
	struct zio_cset *cset;
	struct timer_list timer;
	unsigned long next_run;
	unsigned long period;

	struct urb *urb;
	uint8_t msg[8];
	unsigned int msg_index;
	uint32_t pkt_sent;
	uint32_t pkt_recv;

	size_t pkt_size;
};

enum zvmk80xx_ext {
	ZVMK80XX_PERIOD,
	ZVMK80XX_BITS,
};

ZIO_ATTR_DEFINE_STD(ZIO_DEV, zvmk80xx_do_ao_ai_attr) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, ZVMK80XX_BITS, 8),
};
ZIO_ATTR_DEFINE_STD(ZIO_DEV, zvmk80xx_di_attr) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, ZVMK80XX_BITS, 5),
};
static struct zio_attribute zvmk80xx_cset_attr[] = {
	ZIO_ATTR_EXT("ms-period", ZIO_RW_PERM, ZVMK80XX_PERIOD, 20),
};

static int zvmk80xx_conf_set(struct device *dev, struct zio_attribute *zattr,
			     uint32_t  usr_val)
{
	struct zio_cset *cset = to_zio_cset(dev);
	struct zvmk80xx_cset *zvmk80xx_cset = cset->priv_d;

	if (usr_val < 20) {
		dev_err(dev, "unstable signal under 20ms\n");
		return -EINVAL;
	}
	zvmk80xx_cset->period = msecs_to_jiffies(usr_val);

	return 0;
}

static const struct zio_sysfs_operations zvmk80xx_sysfs_ops = {
	.conf_set = zvmk80xx_conf_set,
};

/* Prototypes */
static void zvmk80xx_urb_callback(struct urb *urb);


/* * * * * * * * * * * * * * * * * * Timer * * * * * * * * * * * * * * * * */

/*
 * zvmk80xx_start_timer
 * @zvmk80xx_cset: channel set context for VMK80XX
 *
 * It programs the channel set timer for the next run
 */
static void zvmk80xx_start_timer(struct zvmk80xx_cset *zvmk80xx_cset)
{
	zvmk80xx_cset->next_run += zvmk80xx_cset->period;
	mod_timer(&zvmk80xx_cset->timer, zvmk80xx_cset->next_run);
}


/*
 * zvmk80xx_send_urb
 * @arg: timer context, a pointer to struct zvmk80xx_cset
 *
 * This runs when the timer expires and on raw_io
 */
static void __zvmk80xx_send_urb(struct zvmk80xx_cset *zvmk80xx_cset)
{
	struct zio_channel *chan;
	uint8_t *data, *buf;
	unsigned int sent, nsample;
	int i, status;

	/* Initialize buffer */
	buf = zvmk80xx_cset->msg;

	if (zvmk80xx_cset->cset->flags & ZIO_DIR_OUTPUT) {
		dev_dbg(&zvmk80xx_cset->cset->head.dev,
			"%s:%d output\n", __func__, __LINE__);
		chan_for_each(chan, zvmk80xx_cset->cset) {
			if (!chan->active_block)
				continue;
			/* Set output data */
			data = chan->active_block->data;
			i = zvmk80xx_cset->msg_index + chan->index;
			buf[i] = data[zvmk80xx_cset->pkt_sent];
			dev_dbg(&chan->head.dev, "data (out) 0x%x\n", buf[i]);

		}

	} else {
		dev_dbg(&zvmk80xx_cset->cset->head.dev,
			"%s:%d input\n", __func__, __LINE__);	}

	/* Send URB packet */
	dev_dbg(&zvmk80xx_cset->cset->head.dev,
		"Message to send: 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x",
		buf[0], buf[1], buf[2], buf[3],
		buf[4], buf[5], buf[6], buf[7]);
	status = usb_submit_urb(zvmk80xx_cset->urb, GFP_ATOMIC);
	if (status) {
		buf = zvmk80xx_cset->msg;
		dev_err(&zvmk80xx_cset->cset->head.dev,
			"Cannot send URB (%d)\n", status);
	} else {
		/* Succesfully sent */
		zvmk80xx_cset->pkt_sent++;
	}

	/* Update context */
	sent = zvmk80xx_cset->pkt_sent;
	nsample = zvmk80xx_cset->cset->ti->nsamples;
	if (sent < nsample)
		zvmk80xx_start_timer(zvmk80xx_cset);
}

#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
static void zvmk80xx_send_urb(unsigned long arg)
{
	struct zvmk80xx_cset *zvmk80xx_cset = (void *)arg;

	__zvmk80xx_send_urb(zvmk80xx_cset);
}
#else
static void zvmk80xx_send_urb(struct timer_list *t)
{
	struct zvmk80xx_cset *zvmk80xx_cset =
		from_timer(zvmk80xx_cset, t, timer);

	__zvmk80xx_send_urb(zvmk80xx_cset);
}
#endif

/* * * * * * * * * * * * * * * * * * * ZIO * * * * * * * * * * * * * * * * */

/*
 * zvmk80xx_generic_raw_io
 * @cset: channel set
 *
 * It handles the data transfer for every channel set. The driver configures
 * specific channel set information on initialization.
 */
static int zvmk80xx_generic_raw_io(struct zio_cset *cset)
{
	struct zvmk80xx_cset *zvmk80xx_cset = cset->priv_d;

	dev_dbg(&cset->head.dev, "%s:%d\n", __func__, __LINE__);
	zvmk80xx_cset->next_run = jiffies;
	zvmk80xx_cset->pkt_sent = 0;
	zvmk80xx_cset->period =
			msecs_to_jiffies(cset->zattr_set.ext_zattr[0].value);
	/* Program the next usb transfer */
	__zvmk80xx_send_urb(zvmk80xx_cset);

	return -EAGAIN;
}


/*
 * zvmk80xx_init_cset
 * @cset: channel set
 */
static int zvmk80xx_init_cset(struct zio_cset *cset)
{
	struct zvmk80xx_dev *zvmk80xx = cset->zdev->priv_d;
	struct zvmk80xx_cset *zvmk80xx_cset;

	/* Allocate a cset-wide context structure */
	zvmk80xx_cset = kzalloc(sizeof(struct zvmk80xx_cset), GFP_KERNEL);
	if (!zvmk80xx_cset)
		return -ENOMEM;

	zvmk80xx_cset->urb = usb_alloc_urb(0, GFP_KERNEL);
	if (!zvmk80xx_cset->urb) {
		kfree(zvmk80xx_cset);
		return -ENOMEM;
	}

#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
	setup_timer(&zvmk80xx_cset->timer, zvmk80xx_send_urb,
		    (unsigned long)zvmk80xx_cset);
#else
	timer_setup(&zvmk80xx_cset->timer, zvmk80xx_send_urb, 0);
#endif

	switch (cset->index) {
	case 0: /* digital input */
		zvmk80xx_cset->msg_index = VMK8055_DI_REG;
		break;
	case 1: /* digital output */
		zvmk80xx_cset->msg_index = VMK8055_DO_REG;
		zvmk80xx_cset->msg[0] = VMK8055_CMD_WRT_AD;
		break;
	case 2: /* analog input */
		zvmk80xx_cset->msg_index = VMK8055_A1_REG;
		break;
	case 3: /* analog output */
		zvmk80xx_cset->msg_index = VMK8055_A1_REG;
		zvmk80xx_cset->msg[0] = VMK8055_CMD_WRT_AD;
		break;
	}

	/* Prepare the URB */
	if (cset->flags & ZIO_DIR_OUTPUT) {
		zvmk80xx_cset->pkt_size =
				le16_to_cpu(zvmk80xx->ep_tx->wMaxPacketSize);
		usb_fill_int_urb(zvmk80xx_cset->urb, zvmk80xx->udev,
			usb_sndintpipe(zvmk80xx->udev,
				       zvmk80xx->ep_tx->bEndpointAddress),
			zvmk80xx_cset->msg, zvmk80xx_cset->pkt_size,
			zvmk80xx_urb_callback, zvmk80xx_cset,
			zvmk80xx->ep_tx->bInterval);
	} else {
		zvmk80xx_cset->pkt_size =
				le16_to_cpu(zvmk80xx->ep_tx->wMaxPacketSize);
		usb_fill_int_urb(zvmk80xx_cset->urb, zvmk80xx->udev,
			usb_rcvintpipe(zvmk80xx->udev,
				       zvmk80xx->ep_rx->bEndpointAddress),
			zvmk80xx_cset->msg, zvmk80xx_cset->pkt_size,
			zvmk80xx_urb_callback, zvmk80xx_cset,
			zvmk80xx->ep_rx->bInterval);
	}

	zvmk80xx_cset->cset = cset;
	cset->priv_d = zvmk80xx_cset;

	return 0;
}
/*
 * zvmk80xx_exit_cset
 * @cset: channel set
 */
static void zvmk80xx_exit_cset(struct zio_cset *cset)
{
	struct zvmk80xx_cset *zvmk80xx_cset = cset->priv_d;

	del_timer_sync(&zvmk80xx_cset->timer);

	usb_kill_urb(zvmk80xx_cset->urb);
	usb_free_urb(zvmk80xx_cset->urb);
	kfree(zvmk80xx_cset);
}

static struct zio_cset zvmk8055_cset[] = {
	{
		ZIO_SET_OBJ_NAME("digital-input"),
		.raw_io =	zvmk80xx_generic_raw_io,
		.n_chan =	1,
		.ssize =	1,
		.flags =	ZIO_DIR_INPUT | ZIO_CSET_TYPE_DIGITAL,
		.zattr_set = {
			.std_zattr = zvmk80xx_di_attr,
			.ext_zattr = zvmk80xx_cset_attr,
			.n_ext_attr = ARRAY_SIZE(zvmk80xx_cset_attr),
		},
	},
	{
		ZIO_SET_OBJ_NAME("digital-output"),
		.raw_io =	zvmk80xx_generic_raw_io,
		.n_chan =	1,
		.ssize =	1, /* 8 bit, 8 DIO */
		.flags =	ZIO_DIR_OUTPUT | ZIO_CSET_TYPE_DIGITAL,
		.zattr_set = {
			.std_zattr = zvmk80xx_do_ao_ai_attr,
			.ext_zattr = zvmk80xx_cset_attr,
			.n_ext_attr = ARRAY_SIZE(zvmk80xx_cset_attr),
		},
	},
	{
		ZIO_SET_OBJ_NAME("analog-input"),
		.raw_io =	zvmk80xx_generic_raw_io,
		.n_chan =	2,
		.ssize =	1,
		.flags =	ZIO_DIR_INPUT | ZIO_CSET_TYPE_ANALOG,
		.zattr_set = {
			.std_zattr = zvmk80xx_do_ao_ai_attr,
			.ext_zattr = zvmk80xx_cset_attr,
			.n_ext_attr = ARRAY_SIZE(zvmk80xx_cset_attr),
		},
	},
	{
		ZIO_SET_OBJ_NAME("analog-output"),
		.raw_io =	zvmk80xx_generic_raw_io,
		.n_chan =	2,
		.ssize =	1,
		.flags =	ZIO_DIR_OUTPUT | ZIO_CSET_TYPE_ANALOG,
		.zattr_set = {
			.std_zattr = zvmk80xx_do_ao_ai_attr,
			.ext_zattr = zvmk80xx_cset_attr,
			.n_ext_attr = ARRAY_SIZE(zvmk80xx_cset_attr),
		},
	},
};

static struct zio_device zvmk8055_tmpl = {
	.owner =	THIS_MODULE,
	.cset =		zvmk8055_cset,
	.n_cset =	ARRAY_SIZE(zvmk8055_cset),
	.s_op =		&zvmk80xx_sysfs_ops,
};

static int zvmk80xx_zio_probe(struct zio_device *zdev)
{
	struct zvmk80xx_dev *zvmk80xx = zdev->priv_d;
	int i, err = 0;

	zvmk80xx->zdev = zdev;

	for (i = 0; i < zdev->n_cset && !err; ++i)
		err = zvmk80xx_init_cset(&zdev->cset[i]);

	return err;
}

static int zvmk80xx_zio_remove(struct zio_device *zdev)
{
	int i;

	for (i = 0; i < zdev->n_cset; ++i)
		zvmk80xx_exit_cset(&zdev->cset[i]);

	return 0;
}


static struct zio_device *zvmk80xx_dev;
static const struct zio_device_id zvmk80xx_table[] = {
	{"zvmk8055", &zvmk8055_tmpl},
	/* TODO zvmk8061 */
	{},
};

static struct zio_driver zvmk80xx_zdrv = {
	.driver = {
		.name = "zvmk80xx",
		.owner = THIS_MODULE,
	},
	.id_table = zvmk80xx_table,
	.probe = zvmk80xx_zio_probe,
	.remove = zvmk80xx_zio_remove,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};


/* * * * * * * * * * * * *  * USB Interface * * * * * * * * * * * * * * * * */

/*
 * zvmk80xx_urb_callback
 */
static void zvmk80xx_urb_callback(struct urb *urb)
{
	struct zvmk80xx_cset *zvmk80xx_cset = urb->context;
	struct zio_channel *chan;
	unsigned long flags = zvmk80xx_cset->cset->flags;
	int stat = urb->status;
	uint8_t *buf, *data, tmp;
	uint32_t sent, succ;
	int i;

	if (stat &&
		!(stat == -ENOENT || stat == -ECONNRESET || stat == -ESHUTDOWN))
		dev_err(&zvmk80xx_cset->cset->head.dev,
			"zvmk80xx: nonzero urb status (%d)\n", stat);

	buf = urb->transfer_buffer;
	/* If it is an input channel set, store data */
	if ((flags & ZIO_DIR) == ZIO_DIR_INPUT) {
		dev_dbg(&zvmk80xx_cset->cset->head.dev,
			"%s:%d input\n", __func__, __LINE__);
		chan_for_each(chan, zvmk80xx_cset->cset) {
			if (!chan->active_block)
				continue;
			/* Set output data */
			data = chan->active_block->data;
			i = zvmk80xx_cset->msg_index + chan->index;
			tmp = buf[i];
			if ((flags & ZIO_CSET_TYPE) == ZIO_CSET_TYPE_DIGITAL)
				tmp = ((tmp & 0xc0)>>3) |
				      ((tmp & 0x01)<<2) |
				      ((tmp & 0x30)>>4);
			dev_dbg(&chan->head.dev, "(in) data[%i] = 0x%x\n",
				zvmk80xx_cset->pkt_sent, tmp);
			data[zvmk80xx_cset->pkt_sent - 1] = tmp;
		}
	} else {
		dev_dbg(&zvmk80xx_cset->cset->head.dev,
			"%s:%d output\n", __func__, __LINE__);
	}

	/* Successfully received */
	zvmk80xx_cset->pkt_recv++;

	succ = zvmk80xx_cset->pkt_recv;
	sent = zvmk80xx_cset->pkt_sent;
	if (sent >= zvmk80xx_cset->cset->ti->nsamples) {
		if (succ < sent)
			dev_err(&zvmk80xx_cset->cset->head.dev,
				"lost %i samples\n", sent - succ);
		zio_trigger_data_done(zvmk80xx_cset->cset);
	}
}


static int zvmk80xx_usb_init(struct zvmk80xx_dev *zvmk80xx)
{
	struct usb_host_interface *iface_desc;
	struct usb_endpoint_descriptor *ep_desc;
	int i;


	iface_desc = zvmk80xx->intf->cur_altsetting;
	if (iface_desc->desc.bNumEndpoints != 2)
		return -ENODEV;

	for (i = 0; i < iface_desc->desc.bNumEndpoints; i++) {
		ep_desc = &iface_desc->endpoint[i].desc;

		if (usb_endpoint_is_int_in(ep_desc)) {
			zvmk80xx->ep_rx = ep_desc;
			continue;
		}

		if (usb_endpoint_is_int_out(ep_desc)) {
			zvmk80xx->ep_tx = ep_desc;
			continue;
		}
	}

	if (!zvmk80xx->ep_rx || !zvmk80xx->ep_tx)
		return -ENODEV;

	zvmk80xx->udev = interface_to_usbdev(zvmk80xx->intf);

	/* Set USB interface private data */
	usb_set_intfdata(zvmk80xx->intf, zvmk80xx);

	return 0;
}

static void zvmk80xx_usb_exit(struct zvmk80xx_dev *zvmk80xx)
{
	/* Remove USB interface private data */
	usb_set_intfdata(zvmk80xx->intf, NULL);
}

static int vmk80xx_usb_probe(struct usb_interface *intf,
			     const struct usb_device_id *id)
{
	struct zvmk80xx_dev *zvmk80xx;
	struct zio_device *zdev;
	int err;

	zvmk80xx = kzalloc(sizeof(struct zvmk80xx_dev), GFP_KERNEL);
	if (IS_ERR_OR_NULL(zvmk80xx))
		return -ENOMEM;

	zvmk80xx->intf = intf;
	err = zvmk80xx_usb_init(zvmk80xx);

	if (zvmk80xx_trigger)
		zvmk8055_tmpl.preferred_trigger = zvmk80xx_trigger;
	if (zvmk80xx_buffer)
		zvmk8055_tmpl.preferred_buffer = zvmk80xx_buffer;

	zdev = zio_allocate_device();
	if (IS_ERR(zdev)) {
		err = PTR_ERR(zdev);
		goto out_allocate;
	}
	zdev->owner = THIS_MODULE;
	zvmk80xx->hwzdev = zdev;
	zdev->priv_d = zvmk80xx;
	err = zio_register_device(zdev, "zvmk8055", 0);
	if (err)
		goto out_dev;
	return 0;
out_dev:
	zio_free_device(zdev);
out_allocate:
	zio_unregister_driver(&zvmk80xx_zdrv);
	kfree(zvmk80xx);
	return err;
}

static void vmk80xx_usb_disconnect(struct usb_interface *intf)
{
	struct zvmk80xx_dev *zvmk80xx = usb_get_intfdata(intf);

	zvmk80xx_usb_exit(zvmk80xx);

	zio_unregister_device(zvmk80xx->hwzdev);
	zio_free_device(zvmk80xx_dev);
	kfree(zvmk80xx);
}

enum usb_supported_device {
	VMK8055,
};

static const struct usb_device_id zvmk80xx_usb_id_table[] = {
	{ USB_DEVICE(0x10cf, 0x5500), .driver_info = VMK8055 },
	{ USB_DEVICE(0x10cf, 0x5501), .driver_info = VMK8055 },
	{ USB_DEVICE(0x10cf, 0x5502), .driver_info = VMK8055 },
	{ USB_DEVICE(0x10cf, 0x5503), .driver_info = VMK8055 },
	{ }
};
MODULE_DEVICE_TABLE(usb, zvmk80xx_usb_id_table);

static struct usb_driver zvmk80xx_usb_driver = {
	.name		= "zvmk80xx",
	.probe		= vmk80xx_usb_probe,
	.disconnect	= vmk80xx_usb_disconnect,
	.id_table	= zvmk80xx_usb_id_table,
};

static int __init zvmk80xx_init(void)
{
	int err;

	err = usb_register(&zvmk80xx_usb_driver);
	if (err)
		return err;
	err = zio_register_driver(&zvmk80xx_zdrv);
	if (err) {
		usb_deregister(&zvmk80xx_usb_driver);
		return err;
	}
	return 0;
}

static void __exit zvmk80xx_exit(void)
{
	zio_unregister_driver(&zvmk80xx_zdrv);
	usb_deregister(&zvmk80xx_usb_driver);
}

module_init(zvmk80xx_init);
module_exit(zvmk80xx_exit);

MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_VERSION(GIT_VERSION);
MODULE_DESCRIPTION("A zio driver for Velleman USB board K8055");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
