/* Alessandro Rubini, 2012, public domain */

/*
 * A minimal device: one input channel only: returns an 8-byte sample
 * with a native-endianness timespec
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>

ZIO_PARAM_TRIGGER(zmini_trigger);
ZIO_PARAM_BUFFER(zmini_buffer);

static int zmini_ndev = 1;
module_param_named(ndev, zmini_ndev, int, 0444);
static int zmini_nchan = 1;
module_param_named(nchan, zmini_nchan, int, 0444);

static int zmini_input(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;
	struct timespec *ts;

	/* Return immediately: just fill the blocks */
	chan_for_each(chan, cset) {
		block = chan->active_block;
		if (!block)
			continue;
		if (block->datalen < sizeof(*ts))
			continue;
		/* we are allowed to return less data than allocated */
		chan->current_ctrl->nsamples = 1;
		block->datalen = sizeof(struct timespec);
		getnstimeofday(block->data);
	}
	return 0; /* Already done */
}
static struct zio_cset zmini_cset[] = {
	{
		ZIO_SET_OBJ_NAME("timespec-in"),
		.raw_io =	zmini_input,
		.flags =	ZIO_DIR_INPUT,
		.n_chan =	1, /* changed at insmod */
		.ssize =	sizeof(struct timespec),
	},
};

static struct zio_device zmini_tmpl = {
	.owner =		THIS_MODULE,
	.cset =			zmini_cset,
	.n_cset =		ARRAY_SIZE(zmini_cset),
};

/* The driver uses a table of templates */
static const struct zio_device_id zmini_table[] = {
	{"zmini", &zmini_tmpl},
	{},
};

static struct zio_driver zmini_zdrv = {
	.driver = {
		.name = "zmini",
		.owner = THIS_MODULE,
	},
	.id_table = zmini_table,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};

/* The device is registered in the bus: we can have several */
static struct zio_device **zmini_dev_array;

static int __init zmini_init(void)
{
	int err, i;
	struct zio_device *zdev;

	/* customize the driver and register it */
	if (zmini_trigger)
		zmini_tmpl.preferred_trigger = zmini_trigger;
	if (zmini_buffer)
		zmini_tmpl.preferred_buffer = zmini_buffer;
	zmini_cset[0].n_chan = zmini_nchan;

	err = zio_register_driver(&zmini_zdrv);
	if (err)
		return err;

	/* allocate the devices and register them */
	if (zmini_ndev < 0 || zmini_ndev > 1024) {
		pr_err("%s: ndev is %i: out of range\n", KBUILD_MODNAME,
		       zmini_ndev);
		return -EINVAL;
	}
	zmini_dev_array = kzalloc(zmini_ndev * sizeof(*zdev), GFP_KERNEL);
	for (i = 0; i < zmini_ndev; i++) {
		zdev = zio_allocate_device();
		if (IS_ERR(zdev)) {
			err = PTR_ERR(zdev);
			goto out;
		}
		zdev->owner = THIS_MODULE;
		err = zio_register_device(zdev, "zmini", 0);
		if (err) {
			zio_free_device(zdev);
			goto out;
		}
		zmini_dev_array[i] = zdev;
	}
	return 0;
out:
	for (i = 0; i < zmini_ndev && zmini_dev_array[i]; i++) {
		zio_unregister_device(zmini_dev_array[i]);
		zio_free_device(zmini_dev_array[i]);
	}
	zio_unregister_driver(&zmini_zdrv);
	kfree(zmini_dev_array);
	return err;
}

static void __exit zmini_exit(void)
{
	int i;

	for (i = 0; i < zmini_ndev && zmini_dev_array[i]; i++) {
		zio_unregister_device(zmini_dev_array[i]);
		zio_free_device(zmini_dev_array[i]);
	}
	zio_unregister_driver(&zmini_zdrv);
	kfree(zmini_dev_array);
	return;
}

module_init(zmini_init);
module_exit(zmini_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_LICENSE("GPL and additional rights");

ADDITIONAL_VERSIONS;
