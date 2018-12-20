/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>


ZIO_PARAM_TRIGGER(ad788x_trigger);
ZIO_PARAM_BUFFER(ad788x_buffer);

enum ad788x_devices {
	ID_AD7476,
	ID_AD7887,
	ID_AD7888,
};

static const struct spi_device_id ad788x_id[] = {
	{"ad7476", ID_AD7476},
	{"ad7887", ID_AD7887},
	{"ad7888", ID_AD7888},
	{}
};
#define AD788x_PM_NAME		"power-management"
#define AD7887_DUAL_NAME	"dual-channel"
#define AD788x_ADDR_SHIFT	11
#define AD788x_PM_ADDR		0x0300
#define AD788x_PM_SHIFT		8
#define AD7888_VREF_ADDR	0x0400
#define AD7888_VREF_SHIFT	10
#define AD7887_VREF_ADDR	0x2000
#define AD7887_VREF_SHIFT	13
#define AD7887_SINDUAL_ADDR	0x1000
#define AD7887_SINDUAL_SHIFT	12

struct ad788x_context {
	struct spi_message	message;
	struct spi_transfer	transfer;
	struct zio_cset		*cset;
	unsigned int		chan_enable; /* number of enabled channel */
	uint32_t		nsamples; /* number of samples */
};

struct ad788x {
	struct zio_device	*zdev;
	enum ad788x_devices	type;
	struct spi_device	*spi;
	uint16_t		cmd;
};
/*
 * AD788x don't have register to store data configuration; configuration
 * option are sent every time when we want acquire. So, there is no address
 * register to set but only a value. Address is used as mask in the tx_buf.
 */
/* Standard attributes for AD7887*/
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_dev_ad7887) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 12),
	/* vref_src can be internal (0) or external (1)*/
	ZIO_ATTR(zdev, ZIO_ATTR_VREFTYPE, ZIO_RW_PERM,
		 AD7887_VREF_ADDR, 1),
};
/* Standard attributes for AD7888*/
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_dev_ad7888) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 12),
	/* vref_src can be internal (0) or external (1)*/
	ZIO_ATTR(zdev, ZIO_ATTR_VREFTYPE, ZIO_RW_PERM,
		 AD7888_VREF_ADDR, 0),
};
/* Standard attributes for AD7476*/
static ZIO_ATTR_DEFINE_STD(ZIO_DEV, zattr_dev_ad7476) = {
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 12),
};
/* Extended attributes for AD7887 */
static struct zio_attribute zattr_dev_ext_ad7887[] = {
	ZIO_ATTR_EXT(AD788x_PM_NAME, ZIO_RW_PERM, AD788x_PM_ADDR, 0x0),
	/* 0 single channel, 1 dual channel*/
	ZIO_ATTR_EXT(AD7887_DUAL_NAME, ZIO_RW_PERM,
		     AD7887_SINDUAL_ADDR, 1),
};
/* Extended attributes for AD7888 */
static struct zio_attribute zattr_dev_ext_ad7888[] = {
	ZIO_ATTR_EXT(AD788x_PM_NAME, ZIO_RW_PERM, AD788x_PM_ADDR, 0x0),
};

static int ad788x_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	unsigned long mask = zattr->id;
	struct ad788x *ad788x;

	ad788x = to_zio_dev(dev)->priv_d;
	switch (mask) {
	case AD788x_PM_ADDR:		/* power management */
		if (usr_val < 4)
			return -EINVAL;
		ad788x->cmd = (ad788x->cmd & ~mask) | usr_val;
		break;
	case AD7887_VREF_ADDR:		/* v_ref source ad7887 */
	case AD7888_VREF_ADDR:		/* v_ref source ad7888 */
	case AD7887_SINDUAL_ADDR:	/* ad7887 single or dual */
		if (usr_val > 1) /* single bit: 0 or 1 */
			return -EINVAL;
		ad788x->cmd = (ad788x->cmd & ~mask) | (usr_val ? mask : 0);
		break;
	}
	pr_debug("%s:%d 0x%x\n", __func__, __LINE__, ad788x->cmd);
	return 0;
}

/* read from AD788x and return the pointer to the data */
static void ad788x_complete(void *cont)
{
	struct ad788x_context *context = (struct ad788x_context *) cont;
	struct zio_channel *chan;
	struct zio_cset *cset;
	uint16_t *data, *buf;
	int i, j = 0;

	cset = context->cset;
	data = (uint16_t *) context->transfer.rx_buf;
	data = &data[1];
	/* demux data */
	chan_for_each(chan, cset) {
		if (!chan->active_block)
			continue;
		buf = (uint16_t *)chan->active_block->data;
		for (i = 0; i < context->nsamples; ++i)
			buf[i] = data[i * context->chan_enable + j];
		++j;
	}
	zio_trigger_data_done(cset);
	/* free context */
	kfree(context->transfer.tx_buf);
	kfree(context->transfer.rx_buf);
	kfree(context);
}

static inline uint16_t *ad788x_build_command(struct ad788x *ad788x,
					     struct ad788x_context *context,
					     uint32_t size)
{
	struct zio_channel *chan;
	uint16_t *command;
	int i, k;

	if (ad788x->type < ID_AD7887)
		return NULL;

	/* configure transfer buffer*/
	command = kmalloc(size, GFP_ATOMIC);
	if (!command)
		return ERR_PTR(-ENOMEM);

	/* configure transfer buffer*/
	for (i = 0,  k = 0; i < context->nsamples; ++i) {
		chan_for_each(chan, context->cset) {
			if (!chan->active_block)
				continue;
			command[k++] = (chan->index << AD788x_ADDR_SHIFT) |
							ad788x->cmd;
		}
	}
	command[k] = ad788x->cmd;

	return command;
}

static int ad788x_input_cset(struct zio_cset *cset)
{
	int err = -EBUSY;
	struct ad788x *ad788x;
	struct ad788x_context *context;
	uint32_t size, nsamples;

	/* alloc context */
	context = kzalloc(sizeof(struct ad788x_context), GFP_ATOMIC);
	if (!context)
		return -ENOMEM;

	ad788x = cset->zdev->priv_d;
	context->chan_enable = zio_get_n_chan_enabled(cset);

	/* prepare SPI message and transfer */
	nsamples = cset->chan->current_ctrl->nsamples;

	/*
	 * Calculate buffer size
	 * nsamples + 1: we need one extra fake sample because SPI answer is
	 *               shifted by 1
	 */
	size = (context->chan_enable * (nsamples + 1) * cset->ssize);

	spi_message_init(&context->message);
	context->message.complete = ad788x_complete;
	context->message.context = context;
	context->cset = cset;
	context->nsamples = nsamples;
	context->transfer.len = size;

	context->transfer.rx_buf = kmalloc(size, GFP_ATOMIC);
	if (!context->transfer.rx_buf) {
		err = -ENOMEM;
		goto err_alloc_rx;
	}

	/* configure transfer buffer*/
	context->transfer.tx_buf = ad788x_build_command(ad788x, context, size);
	if (IS_ERR(context->transfer.tx_buf)) {
		err = PTR_ERR(context->transfer.tx_buf);
		goto err_alloc_tx;
	}

	spi_message_add_tail(&context->transfer, &context->message);

	/* start acquisition */
	err = spi_async_locked(ad788x->spi, &context->message);
	if (!err)
		return -EAGAIN;

	kfree(context->transfer.tx_buf);
err_alloc_tx:
	kfree(context->transfer.rx_buf);
err_alloc_rx:
	kfree(context);
	return err;
}

static const struct zio_sysfs_operations ad788x_s_op = {
	.conf_set = ad788x_conf_set,
};

/* channel sets available */
static struct zio_cset ad7887_ain_cset[] = { /* ad7887 cset */
	{
		.raw_io = ad788x_input_cset,
		.ssize = 2,
		.n_chan = 2,
		.flags = ZIO_CSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};
static struct zio_cset ad7888_ain_cset[] = { /* ad7888 cset */
	{
		.raw_io = ad788x_input_cset,
		.ssize = 2,
		.n_chan = 8,
		.flags = ZIO_CSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};
static struct zio_cset ad7476_ain_cset[] = { /* ad7476 cset */
	{
		.raw_io = ad788x_input_cset,
		.ssize = 2,
		.n_chan = 1,
		.flags = ZIO_CSET_TYPE_ANALOG |	/* is analog */
			 ZIO_DIR_INPUT		/* is input */,
	},
};
static struct zio_device ad788x_tmpl[] = {
	[ID_AD7476] = { /* ad7476 template */
			.owner = THIS_MODULE,
			.flags = 0,
			.cset = ad7476_ain_cset,
			.n_cset = ARRAY_SIZE(ad7476_ain_cset),
			.zattr_set = {
				.std_zattr = zattr_dev_ad7476,
			},
	},
	[ID_AD7887] = { /* ad7887 template */
			.owner = THIS_MODULE,
			.s_op = &ad788x_s_op,
			.flags = 0,
			.cset = ad7887_ain_cset,
			.n_cset = ARRAY_SIZE(ad7887_ain_cset),
			.zattr_set = {
				.std_zattr = zattr_dev_ad7887,
				.ext_zattr = zattr_dev_ext_ad7887,
				.n_ext_attr = ARRAY_SIZE(zattr_dev_ext_ad7887),
			},
	},
	[ID_AD7888] = { /* ad7888 template */
			.owner = THIS_MODULE,
			.s_op = &ad788x_s_op,
			.flags = 0,
			.cset = ad7888_ain_cset,
			.n_cset = ARRAY_SIZE(ad7888_ain_cset),
			.zattr_set = {
				.std_zattr = zattr_dev_ad7888,
				.ext_zattr = zattr_dev_ext_ad7888,
				.n_ext_attr = ARRAY_SIZE(zattr_dev_ext_ad7888),
			},
	},
};

static int ad788x_zio_probe(struct zio_device *zdev)
{
	struct zio_attribute_set *zattr_set;
	struct ad788x *ad788x;
	int vshift;

	pr_info("%s:%d\n", __func__, __LINE__);
	ad788x = zdev->priv_d;
	zattr_set = &zdev->zattr_set;
	ad788x->zdev = zdev;

	/* Setting up the default value for the SPI command */
	vshift = (ad788x->type == ID_AD7887 ? AD7887_VREF_SHIFT :
					      AD7888_VREF_SHIFT);
	ad788x->cmd = zattr_set->std_zattr[ZIO_ATTR_VREFTYPE].value << vshift;
	ad788x->cmd |= zattr_set->ext_zattr[0].value << AD788x_PM_SHIFT;
	if (ad788x->type == ID_AD7887)
		ad788x->cmd |= zattr_set->ext_zattr[1].value <<
							AD7887_SINDUAL_SHIFT;
	return 0;
}

static const struct zio_device_id ad788x_table[] = {
	{"ad7476", &ad788x_tmpl[ID_AD7476]},
	{"ad7887", &ad788x_tmpl[ID_AD7887]},
	{"ad7888", &ad788x_tmpl[ID_AD7888]},
	{},
};
static struct zio_driver ad788x_zdrv = {
	.driver = {
		.name = "zio-ad788x",
		.owner = THIS_MODULE,
	},
	.id_table = ad788x_table,
	.probe = ad788x_zio_probe,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};

static int ad788x_spi_probe(struct spi_device *spi)
{
	const struct spi_device_id *spi_id;
	struct zio_device *zdev;
	struct ad788x *ad788x;
	int err = 0;
	uint32_t dev_id;

	ad788x = kzalloc(sizeof(struct ad788x), GFP_KERNEL);
	if (!ad788x)
		return -ENOMEM;
	/* Configure SPI */

	spi->bits_per_word = 16;
	err = spi_setup(spi);
	if (err)
		return err;
	spi_id = spi_get_device_id(spi);
	if (!spi_id)
		return -ENODEV;
	ad788x->spi = spi;
	ad788x->type = spi_id->driver_data;

	/* zdev here is the generic device */
	zdev = zio_allocate_device();
	zdev->priv_d = ad788x;
	zdev->owner = THIS_MODULE;
	spi_set_drvdata(spi, zdev);

	dev_id = spi->chip_select | (spi->master->bus_num << 8);

	/* Register a ZIO device */
	err = zio_register_device(zdev, spi_id->name, dev_id);
	if (err)
		kfree(ad788x);
	return err;
}

static int ad788x_spi_remove(struct spi_device *spi)
{
	struct zio_device *zdev;
	struct ad788x *ad788x;

	/* zdev here is the generic device */
	zdev = spi_get_drvdata(spi);
	ad788x = zdev->priv_d;
	zio_unregister_device(zdev);
	kfree(ad788x);
	zio_free_device(zdev);
	return 0;
}

static struct spi_driver ad788x_driver = {
	.driver = {
		.name	= "ad788x",
		.bus	= &spi_bus_type,
		.owner	= THIS_MODULE,
	},
	.id_table	= ad788x_id,
	.probe		= ad788x_spi_probe,
	.remove		= ad788x_spi_remove,
};

static int __init ad788x_init(void)
{
	int err, i;

	for (i = 0; i < ARRAY_SIZE(ad788x_tmpl); ++i) {
		if (ad788x_trigger)
			ad788x_tmpl[i].preferred_trigger = ad788x_trigger;
		if (ad788x_buffer)
			ad788x_tmpl[i].preferred_buffer = ad788x_buffer;
	}
	err = zio_register_driver(&ad788x_zdrv);
	if (err)
		return err;
	return spi_register_driver(&ad788x_driver);
}
static void __exit ad788x_exit(void)
{
	driver_unregister(&ad788x_driver.driver);
	zio_unregister_driver(&ad788x_zdrv);
}

module_init(ad788x_init);
module_exit(ad788x_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("AD788x driver for ZIO framework");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
