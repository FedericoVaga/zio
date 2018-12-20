/*
 * Copyright 2011 CERN
 * Author: Alessandro Rubini <rubini@gnudd.com>
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/delay.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"

static void __zio_internal_abort_free(struct zio_cset *cset)
{
	struct zio_channel *chan;
	struct zio_block *block;

	chan_for_each(chan, cset) {
		block = chan->active_block;
		zio_buffer_free_block(chan->bi, block);
		chan->active_block = NULL;
	}
}

/*
 * zio_trigger_abort
 * This is a ZIO helper to invoke the abort function. This must be used when
 * something is going wrong during the acquisition or an armed trigger
 * must be modified. If so requested, the trigger is disabled too.
 * The function returns the previous flags value, or -EAGAIN if it cannot
 * disable because of hardware-busy status.
 */
int __zio_trigger_abort_disable(struct zio_cset *cset, int disable)
{
	struct zio_ti *ti = cset->ti;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&cset->lock, flags);

	/* If we are hardware-busy, cannot abort (the caller may retry) */
	if (cset->flags & ZIO_CSET_HW_BUSY) {
		spin_unlock_irqrestore(&cset->lock, flags);
		return -EAGAIN;
	}

	/* Save previous flags status */
	ret = ti->flags;
	/*
	 * If the trigger is running (ZIO_TI_ARMED), then abort it.
	 * Since the whole data_done procedure happens in locked context,
	 * there is no concurrency with an already-completing trigger event.
	 */
	if (ti->flags & ZIO_TI_ARMED) {
		if (ti->t_op->abort)
			ti->t_op->abort(ti);
		else if (ti->cset->stop_io)
			ti->cset->stop_io(ti->cset);
		else
			__zio_internal_abort_free(cset);
		ti->flags &= (~ZIO_TI_ARMED);
	}
	if (disable)
		ti->flags |= ZIO_DISABLED;
	spin_unlock_irqrestore(&cset->lock, flags);
	return ret;
}
EXPORT_SYMBOL(__zio_trigger_abort_disable);

static int __zio_arm_input_trigger(struct zio_ti *ti)
{
	struct zio_buffer_type *zbuf;
	struct zio_block *block;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_channel *chan;
	struct zio_control *ctrl;
	int i, datalen;

	cset = ti->cset;
	zdev = cset->zdev;
	zbuf = cset->zbuf;

	/* Allocate the buffer for the incoming sample, in active channels */
	chan_for_each(chan, cset) {
		ctrl = chan->current_ctrl;
		ctrl->nsamples = ti->nsamples;
		datalen = ctrl->ssize * ti->nsamples;
		block = zio_buffer_alloc_block(chan->bi, datalen, GFP_ATOMIC);
		/* If alloc error, it is reported at data_done time */
		chan->active_block = block;
	}
	i = cset->raw_io(cset);

	return i;
}

static int __zio_arm_output_trigger(struct zio_ti *ti)
{
	struct zio_cset *cset = ti->cset;
	int i;

	/* We are expected to already have a block in active channels */
	i = cset->raw_io(cset);

	return i;
}

static int __zio_trigger_data_done(struct zio_cset *cset);

/*
 * When a software trigger fires, it should call this function. It
 * used to be called zio_fire_trigger, but actually it only arms the trigger.
 * When hardware is self-timed, the actual trigger fires later.
 */
void zio_arm_trigger(struct zio_ti *ti)
{
	struct zio_channel *chan;
	unsigned long flags;
	int ret;

	do {
		/* if trigger is disabled or already pending, return */
		spin_lock_irqsave(&ti->cset->lock, flags);
		if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED ||
			     (ti->flags & ZIO_TI_ARMED))) {
			spin_unlock_irqrestore(&ti->cset->lock, flags);
			return;
		}
		ti->flags |= ZIO_TI_ARMED;
		getnstimeofday(&ti->tstamp);
		spin_unlock_irqrestore(&ti->cset->lock, flags);

		if (ti->t_op->arm)
			ret = ti->t_op->arm(ti);
		else if (likely((ti->flags & ZIO_DIR) == ZIO_DIR_INPUT))
			ret = __zio_arm_input_trigger(ti);
		else
			ret = __zio_arm_output_trigger(ti);

		/* If arm fails release all active_blocks */
		if (ret && ret != -EAGAIN) {
			/* Error: Free blocks */
			dev_err(&ti->head.dev,
				"raw_io failed (%i), cannot arm trigger\n",
				ret);
			chan_for_each(chan, ti->cset) {
				zio_buffer_free_block(chan->bi,
						      chan->active_block);
				chan->active_block = NULL;
				chan->current_ctrl->zio_alarms |=
							ZIO_ALARM_LOST_TRIGGER;
			}
		}

		/* error or -EGAINA */
		if (ret)
			break;

	} while (__zio_trigger_data_done(ti->cset));

	if (ret == -EAGAIN)
		return;

	/* real error: un-arm */
	spin_lock_irqsave(&ti->cset->lock, flags);
	ti->flags &= ~ZIO_TI_ARMED;
	spin_unlock_irqrestore(&ti->cset->lock, flags);
}
EXPORT_SYMBOL(zio_arm_trigger);

/*
 * zio_trigger_data_done
 * This is a ZIO helper to invoke the data_done trigger operation when a data
 * transfer is over and we need to complete the operation. The trigger
 * is in "ARMED" state when this is called, and is not any more when
 * the function returns.
 *
 * The data_done trigger operation returns an integer [0, 1] which tells if the
 * trigger must be re-armed. The value it is returned to the caller
 * to notify if the trigger was rearmed or not.
 */

/* Internal version, doesn't rearm. Called by zio_fire_trigger() above */
static int __zio_trigger_data_done(struct zio_cset *cset)
{
	unsigned long flags;
	int must_rearm;

	spin_lock_irqsave(&cset->lock, flags);
	if (unlikely(!(cset->ti->flags & ZIO_TI_ARMED)))
		dev_dbg(&cset->head.dev, "data-done: un-armed trigger\n");

	if (cset->ti->t_op->data_done)
		must_rearm = cset->ti->t_op->data_done(cset);
	else
		must_rearm = zio_generic_data_done(cset);

	cset->ti->flags &= ~ZIO_TI_ARMED;
	spin_unlock_irqrestore(&cset->lock, flags);

	return must_rearm;
}

int zio_trigger_data_done(struct zio_cset *cset)
{
	int must_rearm = __zio_trigger_data_done(cset);

	if (must_rearm)
		zio_arm_trigger(cset->ti);

	return must_rearm; /* Actually, "already_rearmed" */
}
EXPORT_SYMBOL(zio_trigger_data_done);


int zio_generic_push_block(struct zio_ti *ti,
			   struct zio_channel *chan,
			   struct zio_block *block)
{
	if (chan->active_block)
		return -EBUSY;
	chan->active_block = block;

	return 0;
}
EXPORT_SYMBOL(zio_generic_push_block);
