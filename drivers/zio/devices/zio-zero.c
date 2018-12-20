/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
/*
 * zio-zero is a simple zio driver, with both input and output.  The
 *  channels are completely software driven. The input channels fill
 *  the data block with zeroes, random data and sequential numbers,
 *  respectively. The output channel just discards data it receives.
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <asm/unaligned.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>

#define ZZERO_VERSION ZIO_HEX_VERSION(1, 1, 0)

ZIO_PARAM_TRIGGER(zzero_trigger);
ZIO_PARAM_BUFFER(zzero_buffer);


ZIO_ATTR_DEFINE_STD(ZIO_DEV, zzero_zattr_dev) = {
	ZIO_SET_ATTR_VERSION(ZZERO_VERSION),
};

ZIO_ATTR_DEFINE_STD(ZIO_DEV, zzero_zattr_cset8) = {
	/* 8 bit -> ssize = 1 */
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 8),
};
ZIO_ATTR_DEFINE_STD(ZIO_DEV, zzero_zattr_cset32) = {
	/* 32 bit -> ssize = 4 */
	ZIO_ATTR(zdev, ZIO_ATTR_NBITS, ZIO_RO_PERM, 0, 32),
	ZIO_ATTR_RNG(zdev, ZIO_ATTR_OFFSET, ZIO_RW_PERM, 0, 0, 0, 100),
	ZIO_ATTR_RNG(zdev, ZIO_ATTR_GAIN, ZIO_RW_PERM, 0, 1, 1, 10),

};
/* This attribute is the sequence point for input channel number 0 of cset 2 */
enum zzero_ext {
	ZZERO_SEQ,
};
static struct zio_attribute zzero_cset1_ext[] = {
	ZIO_ATTR_EXT("sequence", ZIO_RW_PERM, ZZERO_SEQ, 0),
};
/*
 * This generates a sequence of 32-bit little-endian numbers.
 * It is meant to be used for diagnostics and regression testing of buffers
 */
static void zzero_get_sequence(struct zio_channel *chan,
			       void *data, int datalen)
{
	uint32_t *ptr = data;
	uint32_t *value = &chan->cset->zattr_set.ext_zattr[ZZERO_SEQ].value;
	uint32_t gain = chan->cset->zattr_set.std_zattr[ZIO_ATTR_GAIN].value;
	uint32_t offset = chan->cset->zattr_set.std_zattr[ZIO_ATTR_OFFSET].value;

	while (datalen >= 4) {
		put_unaligned_le32((*value * gain) + offset, ptr);
		(*value)++;
		datalen -= 4;
		ptr++;
	}
}

static void zzero_get_interleaved(struct zio_channel *chan,
				  struct zio_block *block, uint8_t *datum)
{
	uint8_t *data;
	int i;

	data = block->data;
	for (i = 0; i < chan->current_ctrl->nsamples; i++) {
		switch (i%3) {
		case 0:
			data[i] = 0;
			break;
		case 1:
			get_random_bytes(&data[i], 1);
			break;
		case 2:
			data[i] = (*datum)++;
		}
	}
}
/* 8 bits input function */
static int zzero_input_8(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;
	static uint8_t datum;
	uint8_t *data;
	int i;

	/* Return immediately: just fill the blocks */
	chan_for_each(chan, cset) {
		block = chan->active_block;
		if (!block)
			continue;
		switch (chan->index) {
		case 0: /* zero */
			memset(block->data, 0x0, block->datalen);
			break;
		case 1: /* random */
			get_random_bytes(block->data, block->datalen);
			break;
		case 2: /* sequence */
			data = block->data;
			for (i = 0; i < chan->current_ctrl->nsamples; i++)
				data[i] = datum++;
			break;
		case 3: /* Interleaved */
			zzero_get_interleaved(chan, block, &datum);
			break;
		}
	}
	return 0; /* Already done */
}
/* 32 bits input function */
static int zzero_input_32(struct zio_cset *cset)
{
	struct zio_channel *chan = cset->chan; /* single channel */
	struct zio_block *block;

	/* Return immediately: just fill the blocks */
	block = chan->active_block;
	if (!block)
		return 0;
	zzero_get_sequence(chan, block->data, block->datalen);

	return 0; /* Already done */
}
static int zzero_output(struct zio_cset *cset)
{
	/* We just eat data, like /dev/zero and /dev/null */
	return 0; /* Already done */
}

static int zzero_conf_set(struct device *dev, struct zio_attribute *zattr,
		      uint32_t  usr_val)
{
	/*
	 * This function is called to change sequence number and its gain and
	 * offset values. Any number is valid.
	 */
	return 0;
}

static const struct zio_sysfs_operations zzero_sysfs_ops = {
	.conf_set = zzero_conf_set,
};

static struct zio_cset zzero_cset[] = {
	{
		ZIO_SET_OBJ_NAME("zero-input-8"),
		.raw_io =	zzero_input_8,
		.n_chan =	3,
		.ssize =	1,
		.flags =	ZIO_DIR_INPUT | ZIO_CSET_TYPE_ANALOG |
				ZIO_CSET_CHAN_INTERLEAVE,
		.zattr_set = {
			.std_zattr = zzero_zattr_cset8,
		},
	},
	{
		ZIO_SET_OBJ_NAME("zero-output-8"),
		.raw_io =	zzero_output,
		.n_chan =	1,
		.ssize =	1,
		.flags =	ZIO_DIR_OUTPUT | ZIO_CSET_TYPE_ANALOG,
		.zattr_set = {
			.std_zattr = zzero_zattr_cset8,
		},
	},
	{
		ZIO_SET_OBJ_NAME("zero-input-32"),
		.raw_io =	zzero_input_32,
		.n_chan =	1,
		.ssize =	4,
		.flags =	ZIO_DIR_INPUT | ZIO_CSET_TYPE_ANALOG,
		.zattr_set = {
			.std_zattr = zzero_zattr_cset32,
			.ext_zattr = zzero_cset1_ext,
			.n_ext_attr = ARRAY_SIZE(zzero_cset1_ext),
		},
	},
};

static struct zio_device zzero_tmpl = {
	.owner =		THIS_MODULE,
	.cset =			zzero_cset,
	.n_cset =		ARRAY_SIZE(zzero_cset),
	.s_op =			&zzero_sysfs_ops,
	.zattr_set = {
		.std_zattr = zzero_zattr_dev,
	}
};

static struct zio_device *zzero_dev;
static const struct zio_device_id zzero_table[] = {
	{"zzero", &zzero_tmpl},
	{},
};

static struct zio_driver zzero_zdrv = {
	.driver = {
		.name = "zzero",
		.owner = THIS_MODULE,
	},
	.id_table = zzero_table,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};

static int __init zzero_init(void)
{
	int err;

	if (zzero_trigger)
		zzero_tmpl.preferred_trigger = zzero_trigger;
	if (zzero_buffer)
		zzero_tmpl.preferred_buffer = zzero_buffer;

	err = zio_register_driver(&zzero_zdrv);
	if (err)
		return err;
	zzero_dev = zio_allocate_device();
	if (IS_ERR(zzero_dev)) {
		err = PTR_ERR(zzero_dev);
		goto out_all;
	}
	zzero_dev->owner = THIS_MODULE;
	err = zio_register_device(zzero_dev, "zzero", 0);
	if (err)
		goto out_dev;
	return 0;
out_dev:
	zio_free_device(zzero_dev);
out_all:
	zio_unregister_driver(&zzero_zdrv);
	return err;
}

static void __exit zzero_exit(void)
{
	zio_unregister_device(zzero_dev);
	zio_free_device(zzero_dev);
	zio_unregister_driver(&zzero_zdrv);
}

module_init(zzero_init);
module_exit(zzero_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Federico Vaga <federico.vaga@gmail.com>");
MODULE_DESCRIPTION("A zio driver which fakes zero, random and sawtooth input");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
