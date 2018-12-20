/* Alessandro Rubini for CERN, 2013, GNU GPLv2 or later */

/*
 * This is a simple TDC (time to digital converter). The events it
 * stamps are interrupts (one interrupt source only) and has two csets:
 *
 * cset 0 (1 channel) returns the stamps as timespec, several per block.
 * cset 1 (1 channel) returns zero-sized blocks with the stamp in the control.
 *
 * The driver is used to experiment with self-timed peripherals. cset 0
 * includes a stop_io function that shows how to return a partial block
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>

#include <linux/zio.h>
#include <linux/zio-trigger.h>

ZIO_PARAM_BUFFER(ztdc_buffer);

int ztdc_irq = -1;
module_param_named(irq, ztdc_irq, int, 0444);

/* The interrupt handler is taking timestamps and filling blocks */
irqreturn_t ztdc_handler(int irq, void *dev_id)
{
	struct timespec ts, *tsp;
	struct zio_device *dev = dev_id;
	struct zio_cset *cset;
	struct zio_channel *chan;
	struct zio_block *block;

	getnstimeofday(&ts);

	/*
	 * fill cset 0: several per block. We return the block only when full,
	 * Actually, if we get stop_io, we return it as partially-filled.
	 * The first stamp is saved in the trigger too, whence it reaches the
	 * control for all channels ad data_done time.
	 */
	cset = dev->cset;
	chan = cset->chan;
	block = chan->active_block;
	if (block) {
		if (!block->uoff)
			cset->ti->tstamp = ts;
		tsp = block->data + block->uoff;
		*tsp = ts;
		block->uoff += sizeof(ts);
		if (block->uoff == block->datalen) {
			block->uoff = 0; /* for read method */
			zio_trigger_data_done(cset);
		}
	} else {
		chan->current_ctrl->zio_alarms |= ZIO_ALARM_LOST_TRIGGER;
	}

	/*
	 * fill cset 1: a zero-size thing: save the stamp in the trigger
	 * because that's whence data_done copies it to all channels.
	 * Also, fix nsamples in the current control, where it is
	 * prepared for us every time the trigger is armed.
	 */
	cset = dev->cset + 1;
	chan = cset->chan;
	block = chan->active_block;
	if (block) {
		cset->ti->tstamp = ts;
		chan->current_ctrl->nsamples = 1;
		zio_trigger_data_done(cset);
	} else {
		chan->current_ctrl->zio_alarms |= ZIO_ALARM_LOST_TRIGGER;
	}
	return IRQ_NONE; /* none because we rely on other devices */
}

/*
 * stop_io: called when a trigger need to be aborted and re-armed
 * The function is called in locked context. Here is it only used
 * for the data cset, so we can just return the partial block.
 */
static void ztdc_stop_io(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;

	chan_for_each(chan, cset) { /* we have one channel, but this works */
		block = chan->active_block;
		if (!block)
			continue;
		if (!block->uoff) {/* Empty: just free it */
			zio_buffer_free_block(chan->bi, block);
			chan->active_block = 0;
		} else {
			/* Close up the partial block, and return it */
			chan->current_ctrl->nsamples =
				block->uoff / chan->current_ctrl->ssize;
			block->datalen = block->uoff;
			block->uoff = 0;
		}
	}
	zio_generic_data_done(cset);
}

/* raw_io method */
static int ztdc_input(struct zio_cset *cset)
{
	struct zio_channel *chan = cset->chan; /* our csets have this only */

	chan_for_each(chan, cset) { /* we have one channel, but this works */
		if (!chan->active_block)
			continue;
		/* Check datalen here, as the handler just writes in it */
		if (cset->index == 0 && chan->active_block->datalen == 0)
			return -EINVAL;
	}
	return -EAGAIN; /* Will data_done later */
}

/*
 * The probe function receives a new zio_device, which is different from
 * what we allocated (that one is the "hardwre" device). So save it
 */
static struct zio_device *ztdc_dev;
static int ztdc_probe(struct zio_device *zdev)
{
	ztdc_dev = zdev;
	return 0;
}

static struct zio_cset ztdc_cset[] = {
	{
		ZIO_SET_OBJ_NAME("data-stamps"),
		.raw_io =	ztdc_input,
		.stop_io =	ztdc_stop_io,
		.flags =	ZIO_DIR_INPUT | ZIO_CSET_TYPE_TIME |
					ZIO_CSET_SELF_TIMED,
		.n_chan =	1,
		.ssize =	sizeof(struct timespec),
	},
	{
		ZIO_SET_OBJ_NAME("ctrl-stamps"),
		.raw_io =	ztdc_input,
		.stop_io =	NULL, /* by default the block is freed */
		.flags =	ZIO_DIR_INPUT | ZIO_CSET_TYPE_TIME |
					ZIO_CSET_SELF_TIMED,
		.n_chan =	1,
		.ssize =	0,
	},
};

static struct zio_device ztdc_tmpl = {
	.owner =		THIS_MODULE,
	.cset =			ztdc_cset,
	.n_cset =		ARRAY_SIZE(ztdc_cset),
};

/* The driver uses a table of templates */
static const struct zio_device_id ztdc_table[] = {
	{"ztdc", &ztdc_tmpl},
	{},
};

static struct zio_driver ztdc_zdrv = {
	.driver = {
		.name = "ztdc",
		.owner = THIS_MODULE,
	},
	.id_table = ztdc_table,
	.probe = ztdc_probe,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};

/* Lazily, use a single global device */
static struct zio_device *ztdc_init_dev;

irqreturn_t ztdc_fake_handler(int irq, void *dev_id)
{
	return IRQ_NONE;
}

static int __init ztdc_init(void)
{
	int err;

	if (ztdc_irq < 0) {
		pr_err("%s: please pass interrupt number as irq=\n",
		       KBUILD_MODNAME);
		return -EINVAL;
	}

	/* Try to request the interrupt first, to catch common errors */
	err = request_irq(ztdc_irq, ztdc_fake_handler, IRQF_SHARED,
			  KBUILD_MODNAME, ztdc_init);
	if (err < 0) {
		pr_err("%s: can't request shared irq %i: error %i\n",
		       KBUILD_MODNAME, ztdc_irq, -err);
		return err;
	}
	free_irq(ztdc_irq, ztdc_init);

	if (ztdc_buffer)
		ztdc_tmpl.preferred_buffer = ztdc_buffer;

	err = zio_register_driver(&ztdc_zdrv);
	if (err)
		return err;

	ztdc_init_dev = zio_allocate_device();
	if (IS_ERR(ztdc_init_dev)) {
		err = PTR_ERR(ztdc_init_dev);
		goto out_alloc;
	}
	ztdc_init_dev->owner = THIS_MODULE;
	err = zio_register_device(ztdc_init_dev, "ztdc", 0);
	if (err)
		goto out_register;
	err = request_irq(ztdc_irq, ztdc_handler, IRQF_SHARED,
			  KBUILD_MODNAME, ztdc_dev);
	if (!err)
		return 0;

	/* unlikely: we already did that at the beginning */
	pr_err("%s: can't request shared irq %i: error %i\n",
	       KBUILD_MODNAME, ztdc_irq, -err);

	zio_unregister_device(ztdc_dev);
out_register:
	zio_free_device(ztdc_init_dev);
out_alloc:
	zio_unregister_driver(&ztdc_zdrv);
	return err;
}

static void __exit ztdc_exit(void)
{
	free_irq(ztdc_irq, ztdc_dev);
	zio_unregister_device(ztdc_init_dev);
	zio_free_device(ztdc_init_dev);
	ztdc_dev = NULL;
	zio_unregister_driver(&ztdc_zdrv);
}

module_init(ztdc_init);
module_exit(ztdc_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
