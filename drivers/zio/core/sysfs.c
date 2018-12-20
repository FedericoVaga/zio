/*
 * Copyright 2011 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/sysfs.h>
#include <linux/version.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"


#define ZOBJ_SYSFS_NAME		"name"
#define ZOBJ_SYSFS_ENABLE	"enable"
#define CSET_SYSFS_BUFFER	"current_buffer"
#define CSET_SYSFS_TRIGGER	"current_trigger"

const char zio_zdev_attr_names[_ZIO_DEV_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_GAIN]			= "gain_factor",
	[ZIO_ATTR_OFFSET]		= "offset",
	[ZIO_ATTR_NBITS]		= "resolution-bits",
	[ZIO_ATTR_MAXRATE]		= "max-sample-rate",
	[ZIO_ATTR_VREFTYPE]		= "vref-src",
	[ZIO_ATTR_DEV_VERSION]	= "version",
};
EXPORT_SYMBOL(zio_zdev_attr_names);

const char zio_trig_attr_names[_ZIO_TRG_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_TRIG_N_SHOTS]		= "nshots",
	[ZIO_ATTR_TRIG_PRE_SAMP]	= "pre-samples",
	[ZIO_ATTR_TRIG_POST_SAMP]	= "post-samples",
	[ZIO_ATTR_TRIG_VERSION]		= "version",
};
EXPORT_SYMBOL(zio_trig_attr_names);

const char zio_zbuf_attr_names[_ZIO_BUF_ATTR_STD_NUM][ZIO_NAME_LEN] = {
	[ZIO_ATTR_ZBUF_MAXLEN]	= "max-buffer-len",
	[ZIO_ATTR_ZBUF_MAXKB]	= "max-buffer-kb",
	[ZIO_ATTR_ZBUF_ALLOC_LEN]	= "allocated-buffer-len",
	[ZIO_ATTR_ZBUF_ALLOC_KB]	= "allocated-buffer-kb",
	[ZIO_ATTR_ZBUF_VERSION]	= "version",
};
EXPORT_SYMBOL(zio_zbuf_attr_names);

static struct zio_attribute *__zattr_clone(const struct zio_attribute *src,
		unsigned int n)
{
	struct zio_attribute *dest = NULL;
	unsigned int size;

	if (!src || !n)
		return NULL;
	size = n * sizeof(struct zio_attribute);
	dest = kmalloc(size, GFP_KERNEL);
	if (!dest)
		return NULL;

	dest = memcpy(dest, src, size);

	return dest;
}

static void __zattr_unclone(struct zio_attribute *zattr)
{
	kfree(zattr);
}

/* When touching attributes, we always use the spinlock for the hosting dev */
spinlock_t *__zio_get_dev_spinlock(struct zio_obj_head *head)
{
	spinlock_t *lock;

	switch (head->zobj_type) {
	case ZIO_DEV:
		lock = &to_zio_dev(&head->dev)->lock;
		break;
	case ZIO_CSET:
		lock = &to_zio_cset(&head->dev)->zdev->lock;
		break;
	case ZIO_CHAN:
		lock = &to_zio_chan(&head->dev)->cset->zdev->lock;
		break;
	case ZIO_TI: /* we might not want to take a lock but... */
		lock = &to_zio_ti(&head->dev)->cset->zdev->lock;
		break;
	case ZIO_BI:
		lock = &to_zio_bi(&head->dev)->cset->zdev->lock;
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
		return NULL;
	}
	return lock;
}


/*
 * used to init and update sysfs attribute value into a control.
 * The bit mask is set also during update to make the code simple, but
 * this does not decrease performance
 */
static inline void __zattr_valcpy(struct zio_ctrl_attr *ctrl,
				  struct zio_attribute *zattr)
{
	if ((zattr->flags & ZIO_ATTR_TYPE) == ZIO_ATTR_TYPE_EXT) {
		ctrl->ext_mask |= (1 << zattr->index);
		ctrl->ext_val[zattr->index] = zattr->value;
	} else {
		if (zattr->index == ZIO_ATTR_INDEX_NONE)
			return;
		ctrl->std_mask |= (1 << zattr->index);
		ctrl->std_val[zattr->index] = zattr->value;
	}
}

void __ctrl_update_nsamples(struct zio_ti *ti)
{
	struct zio_cset *cset = ti->cset;

	ti->nsamples = zio_ti_std_val(ti, ZIO_ATTR_TRIG_PRE_SAMP) +
		       zio_ti_std_val(ti, ZIO_ATTR_TRIG_POST_SAMP);

	/*
	 * If a cset is interleaved only or the interleaved channel is enabled,
	 * then, it acquires all channels samples in a single buffer. Because
	 * of interleaving, count only the physical channel (n_chan - 1)
	 */
	if (cset->interleave) {
		if ((cset->flags & ZIO_CSET_INTERLEAVE_ONLY) ||
				!(cset->interleave->flags & ZIO_STATUS))
			ti->nsamples *= (cset->n_chan - 1);
	}
}
void __zio_attr_propagate_value(struct zio_obj_head *head,
			     struct zio_attribute *zattr)
{
	int i, j;
	unsigned long flags;
	struct zio_ti *ti;
	struct zio_device *zdev;
	struct zio_channel *chan;
	struct zio_cset *cset;
	struct zio_control *ctrl;

	if (!(zattr->flags & ZIO_ATTR_CONTROL))
		return; /* the attribute is not in the control */

	switch (head->zobj_type) {
	case ZIO_DEV:
		zdev = to_zio_dev(&head->dev);
		for (i = 0; i < zdev->n_cset; ++i) {
			cset = &zdev->cset[i];
			for (j = 0; j < cset->n_chan; ++j) {
				ctrl = cset->chan[j].current_ctrl;
				__zattr_valcpy(&ctrl->attr_channel, zattr);
			}
		}
		break;
	case ZIO_CSET:
		cset = to_zio_cset(&head->dev);
		for (i = 0; i < cset->n_chan; ++i) {
			ctrl = cset->chan[i].current_ctrl;
			__zattr_valcpy(&ctrl->attr_channel, zattr);
		}
		break;
	case ZIO_CHAN:
		ctrl = to_zio_chan(&head->dev)->current_ctrl;
		__zattr_valcpy(&ctrl->attr_channel, zattr);
		break;
	case ZIO_TI:
		ti = to_zio_ti(&head->dev);
		/*
		 * It is disabled, nobody can enable since that is only
		 * possible through sysfs and we hold the config lock.
		 * So pick the I/O lock to prevent I/O operations and proceed.
		 */
		spin_lock_irqsave(&ti->cset->lock, flags);
		__ctrl_update_nsamples(ti);
		/* Update attributes in all "current_ctrl" struct */
		for (i = 0; i < ti->cset->n_chan; ++i) {
			chan = &ti->cset->chan[i];
			ctrl = chan->current_ctrl;
			__zattr_valcpy(&ctrl->attr_trigger, zattr);
		}
		spin_unlock_irqrestore(&ti->cset->lock, flags);
		break;
	default:
		return;
	}
}

void __zattr_trig_init_ctrl(struct zio_ti *ti, struct zio_control *ctrl)
{
	int i;
	struct zio_ctrl_attr *ctrl_attr_trig = &ctrl->attr_trigger;

	strncpy(ctrl->triggername, ti->cset->trig->head.name, ZIO_OBJ_NAME_LEN);
	ctrl_attr_trig->std_mask = 0;
	ctrl_attr_trig->ext_mask = 0;
	/* Copy trigger value */
	for (i = 0; i < ti->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_trig, &ti->zattr_set.std_zattr[i]);
	for (i = 0; i < ti->zattr_set.n_ext_attr; ++i)
		if (ti->zattr_set.ext_zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_trig,
				       &ti->zattr_set.ext_zattr[i]);
}
static int __zattr_chan_init_ctrl(struct zio_channel *chan, unsigned int start)
{
	struct zio_ctrl_attr *ctrl_attr_chan;
	struct zio_attribute *zattr;
	struct zio_control *ctrl;
	struct zio_device *zdev;
	struct zio_cset *cset;
	int i;

	cset = chan->cset;
	zdev = cset->zdev;
	ctrl = chan->current_ctrl;
	ctrl->addr.dev_id = chan->cset->zdev->dev_id;
	ctrl_attr_chan = &chan->current_ctrl->attr_channel;
	if (!(start + chan->zattr_set.n_ext_attr < 32)) {
		dev_err(&zdev->head.dev, "too many extended attribute");
		return -EINVAL;
	}

	__zattr_trig_init_ctrl(cset->ti, chan->current_ctrl);
	/* Copy standard attributes into the control */
	for (i = 0; i < chan->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &chan->zattr_set.std_zattr[i]);
	for (i = 0; i < cset->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &cset->zattr_set.std_zattr[i]);
	for (i = 0; i < zdev->zattr_set.n_std_attr; ++i)
		__zattr_valcpy(ctrl_attr_chan, &zdev->zattr_set.std_zattr[i]);

	/* Fix and copy attributes within channel */
	zattr = chan->zattr_set.ext_zattr;
	for (i = 0; i < chan->zattr_set.n_ext_attr; ++i) {
		if (zattr[i].flags & ZIO_ATTR_CONTROL) {
			/* Fix channel extended attribute index */
			zattr[i].index = start + i;
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
		} else {
			zattr[i].index = ZIO_ATTR_INDEX_NONE;
		}
	}
	/* Copying attributes from cset */
	zattr = cset->zattr_set.ext_zattr;
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
	/* Copying attributes from zdev */
	zattr = zdev->zattr_set.ext_zattr;
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			__zattr_valcpy(ctrl_attr_chan, &zattr[i]);
	return 0;
}
static int __zattr_cset_init_ctrl(struct zio_cset *cset, unsigned int start)
{
	struct zio_attribute *zattr;
	int i, err, start_c = start;

	/* Fix cset extended attribute index */
	zattr = cset->zattr_set.ext_zattr;
	for (i = 0; i < cset->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			zattr[i].index = start_c++;
		else
			zattr[i].index = ZIO_ATTR_INDEX_NONE;

	zattr = cset->ti->zattr_set.ext_zattr;
	for (i = 0; i < cset->ti->zattr_set.n_ext_attr; ++i)
		if (!(zattr[i].flags & ZIO_ATTR_CONTROL))
			zattr[i].index = ZIO_ATTR_INDEX_NONE;


	for (i = 0; i < cset->n_chan; ++i) {
		err = __zattr_chan_init_ctrl(&cset->chan[i], start_c);
		if (err)
			return err;
	}
	return 0;
}

/*
 * fix the zio attribute index for the extended attribute within device
 * and set the attribute value into the current control of each channel
 */
int __zattr_dev_init_ctrl(struct zio_device *zdev)
{
	struct zio_attribute *zattr;
	int i, err, start = 0;

	/* Device level */
	/* Fix device extended attribute index */
	zattr = zdev->zattr_set.ext_zattr;
	for (i = 0; i < zdev->zattr_set.n_ext_attr; ++i)
		if (zattr[i].flags & ZIO_ATTR_CONTROL)
			zattr[i].index = start++;
		else
			zattr[i].index = ZIO_ATTR_INDEX_NONE;

	for (i = 0; i < zdev->n_cset; ++i) {
		err = __zattr_cset_init_ctrl(&zdev->cset[i], start);
		if (err)
			return err;
	}
	return 0;
}

/**
 * The function perform a post processing of the enable status of a channel
 * when it change. This implements the enable/disable policies when there is
 * an interleaved channel
 *
 * @param chan channel to enable/disable
 * @param enable enable status
 */
static void __chan_enable_interleave(struct zio_channel *chan,
				     unsigned int enable) {
	struct zio_cset *cset = chan->cset;
	int i;

	/*
	 * If the cset is interleave only, then only the interleaved
	 * channel can be enabled/disabled. All the other channels are
	 * always disabled.
	 */
	if (cset->flags & ZIO_CSET_INTERLEAVE_ONLY) {
		/* Normal channel, force disable */
		if (!(chan->flags & ZIO_CSET_CHAN_INTERLEAVE))
			chan->flags |= ZIO_DISABLED;
		return;
	}

	/*
	 * If the cset is not interleave only, then when the
	 * interleaved channel change its status, all the other channel
	 * must be set to the opposite status.
	 */
	if (chan->flags & ZIO_CSET_CHAN_INTERLEAVE) { /* Interleaved channel */
		for (i = 0; i < cset->n_chan - 1; ++i)
			__zio_object_enable(&cset->chan[i].head, !enable);
	} else { /* Normal channel */
		if (!(cset->interleave->flags & ZIO_DISABLED)) {
			/*
			 *  Interleave channel is enabled, then normal channel
			 * cannot be enabled
			 */
			chan->flags |= ZIO_DISABLED;
		}
	}
}


/*
 * enable/disable a zio object
 * must be called while holding the zio_device spinlock
 *
 * @dev: the device to enable/disable
 * @enable: this value come from the user space, 0 for disable and 1 for enable
 *          The enable status on a ZIO device is active low, so this function
 *          must handle this difference.
 */
int __zio_object_enable(struct zio_obj_head *head, unsigned int enable)
{
	unsigned long *zf, flags;
	int i, status;
	struct zio_channel *chan;
	struct zio_device *zdev;
	struct zio_cset *cset;
	struct zio_ti *ti;

	dev_dbg(&head->dev, "enable = %d\n", enable);

	zf = zio_get_from_obj(head, flags);
	status = !((*zf) & ZIO_STATUS);
	/* if the status is not changing */
	if (!(enable ^ status))
		return 0;
	/* change status */
	*zf = (*zf & (~ZIO_STATUS)) | status;

	switch (head->zobj_type) {
	case ZIO_DEV:
		dev_dbg(&head->dev, "(dev)\n");

		zdev = to_zio_dev(&head->dev);
		/* enable/disable all csets */
		for (i = 0; i < zdev->n_cset; ++i)
			__zio_object_enable(&zdev->cset[i].head, enable);

		/* device callback */
		if (zdev->change_flags)
			zdev->change_flags(head, ZIO_STATUS);
		break;
	case ZIO_CSET:
		dev_dbg(&head->dev, "(cset)\n");

		cset = to_zio_cset(&head->dev);
		/* enable/disable trigger instance */
		__zio_object_enable(&cset->ti->head, enable);
		/* enable/disable all channels */
		for (i = 0; i < cset->n_chan; ++i)
			__zio_object_enable(&cset->chan[i].head, enable);

		/* cset callback */
		if (cset->change_flags)
			cset->change_flags(head, ZIO_STATUS);
		break;
	case ZIO_CHAN:
		dev_dbg(&head->dev, "(chan)\n");
		chan = to_zio_chan(&head->dev);

		if (chan->cset->flags & ZIO_CSET_CHAN_INTERLEAVE) {
			__ctrl_update_nsamples(chan->cset->ti);
			__chan_enable_interleave(chan, enable);
		}

		/* channel callback */
		if (chan->change_flags)
			chan->change_flags(head, ZIO_STATUS);
		break;
	case ZIO_TI:
		dev_dbg(&head->dev, "(ti)\n");

		ti = to_zio_ti(&head->dev);
		cset = ti->cset;

		/* The abort_disable() in helpers.c may fail, if hw-busy */
		i = __zio_trigger_abort_disable(ti->cset, 0);
		if (i < 0)
			return i;

		spin_lock_irqsave(&ti->cset->lock, flags);
		if (ti->t_op->change_status)
			ti->t_op->change_status(ti, status);
		spin_unlock_irqrestore(&ti->cset->lock, flags);
		/* A user-forced disable sends POLLERR to waiters */
		for (i = 0; i < cset->n_chan; ++i) {
			chan = cset->chan + i;
			wake_up_interruptible(&chan->bi->q);
		}
		break;

	/* following objects can't be enabled/disabled */
	case ZIO_BI:
		dev_dbg(&head->dev, "(buf)\n");
		/*
		 * The buffer instance cannot be enabled/disabled by sysfs. A
		 * buffer instance is usually enabled, even if its channel is
		 * disabled. This allow users to fill a channel buffer, even if
		 * the channel is disabled. Only ZIO can disable a buffer
		 * instance during flush.
		 */
	case ZIO_BUF:
	case ZIO_TRG:
		break;
	default:
		WARN(1, "ZIO: unknown zio object %i\n", head->zobj_type);
	}
	return 0;
}

/* Print the name of a zio object */
static ssize_t zobj_show_name(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	if (strlen(to_zio_head(dev)->name))
		return sprintf(buf, "%s\n", to_zio_head(dev)->name);
	else
		return sprintf(buf, "%s\n", dev_name(dev));
}
/* Print the name of a zio object */
static ssize_t zobj_show_dev_type(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", dev->type->name);
}
/* Print the current trigger name */
static ssize_t zobj_show_cur_trig(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_cset(dev)->trig->head.name);
}
/* Change the current trigger */
static ssize_t zobj_store_cur_trig(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char buf_tmp[ZIO_OBJ_NAME_LEN];
	int err = 0, ret;

	dev_dbg(dev, "Changing trigger to: %s\n", buf);
	if (strlen(buf) > ZIO_OBJ_NAME_LEN + 1)
		return -EINVAL; /* name too long */
	ret = sscanf(buf, "%s\n", buf_tmp);
	if (ret != 1) {
		dev_err(dev, "cannot extract string from sysfs input buffer");
		return -EINVAL;
	}

	err = zio_change_current_trigger(to_zio_cset(dev), buf_tmp);
	return err ? err : count;
}
/* Print the current buffer name */
static ssize_t zobj_show_cur_zbuf(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", to_zio_cset(dev)->zbuf->head.name);
}
/* Change the current buffer */
static ssize_t zobj_store_cur_zbuf(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	char buf_tmp[ZIO_OBJ_NAME_LEN];
	int err = 0, ret;

	dev_dbg(dev, "Changing buffer to: %s\n", buf);
	if (strlen(buf) > ZIO_OBJ_NAME_LEN + 1)
		return -EINVAL; /* name too long */
	ret = sscanf(buf, "%s\n", buf_tmp);
	if (ret != 1) {
		dev_err(dev, "cannot extract string from sysfs input buffer");
		return -EINVAL;
	}

	err = zio_change_current_buffer(to_zio_cset(dev), buf_tmp);
	return err ? err : count;
}
/* Print the current enable status */
static ssize_t zobj_show_enable(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	unsigned long *flags;
	int status;

	flags = zio_get_from_obj(to_zio_head(dev), flags);
	status = !(*flags & ZIO_DISABLED);
	return sprintf(buf, "%d\n", status);
}
/* Change the current enable status */
static ssize_t zobj_store_enable(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct zio_obj_head *head = to_zio_head(dev);
	long val;
	int err;
	spinlock_t *lock;

	err = kstrtol(buf, 0, &val);
	if (err || val < 0 || val > 1)
		return -EINVAL;

	lock = __zio_get_dev_spinlock(head);
	do {
		spin_lock(lock);
		err = __zio_object_enable(head, val);
		spin_unlock(lock);
		if (err == -EAGAIN)
			msleep(1);
	} while (err  == -EAGAIN);
	return count;
}
/*
 * Zio objects all handle uint32_t values. So the show and store
 * are centralized here, and each device has its own get_info and set_conf
 * which handle binary 32-bit numbers. Both the function are locked to prevent
 * concurrency issue when editing device register.
 */
static ssize_t zattr_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct zio_attribute *zattr = to_zio_zattr(attr);
	ssize_t len = 0;

	if (!zattr->s_op)
		goto out;
	if (zattr->s_op->info_get) {
		struct zio_obj_head *head = to_zio_head(dev);
		spinlock_t *lock;
		int err = 0;

		lock = __zio_get_dev_spinlock(head);
		spin_lock(lock);
		err = zattr->s_op->info_get(dev, zattr, &zattr->value);
		spin_unlock(lock);
		if (err)
			return err;
	}

	dev_dbg(dev, "read value %d from sysfs attribute %s\n",
		zattr->value, attr->attr.name);
out:
	len = sprintf(buf, "%i\n", zattr->value);
	return len;
}

int __zio_conf_set(struct zio_obj_head *head, struct zio_attribute *zattr,
		   uint32_t val)
{
	int err;

	/* device attributes */
	if (!zattr->s_op->conf_set)
		return -EINVAL;

	err = zattr->s_op->conf_set(&head->dev, zattr, val);
	if (err)
	        return err;
	zattr->value = (uint32_t)val;
	__zio_attr_propagate_value(head, zattr);

	return 0;
}

static ssize_t zattr_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct zio_obj_head *head = to_zio_head(dev);
	struct zio_attribute *zattr = to_zio_zattr(attr);
	struct zio_ti *ti = NULL;
	unsigned long tflags = 0;
	spinlock_t *lock;
	long val;
	int err;

	if (kstrtol(buf, 0, &val))
		return -EINVAL;

	/*
	 * If the given value exceed the attribute range, then it's
	 * an invalid value. When 'min == max' it means that the attribute
	 * hasn't a range
	 */
	if (zattr->min != zattr->max &&
	    (val < zattr->min || val > zattr->max)) {
		dev_err(dev, "Value %u exceed range [%u, %u]\n",
			(uint32_t)val, zattr->min, zattr->max);
		return -EINVAL;
	}

	lock = __zio_get_dev_spinlock(head);
	spin_lock(lock);

	/* cannot modify trigger's attributes while is armed */
	if (head->zobj_type == ZIO_TI) {
		ti = to_zio_ti(&head->dev);
		tflags = zio_trigger_abort_disable(ti->cset, 1);
	}

	/* Configure the attribute */
	err = __zio_conf_set(head, zattr, (uint32_t)val);

	if (head->zobj_type == ZIO_TI) {
		/* restore trigger status */
		if (ti && ((tflags & ZIO_STATUS) == ZIO_ENABLED))
			ti->flags = (ti->flags & ~ZIO_STATUS) | ZIO_ENABLED;
		if (ti && (tflags & ZIO_TI_ARMED))
			zio_arm_trigger(ti);
	}

	spin_unlock(lock);

	return err ? err : count;
}

/*
 * zobj_show_devname
 *
 * The function return the address of the different ZIO object in the device
 * hierarchy.
 *
 * FIXME with interface, this function call an interface operation which returns
 * the address. In this way, pf-zio and cdev can return their own devname
 */
static ssize_t zobj_show_devname(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct zio_obj_head *head = to_zio_head(dev);
	struct zio_channel *chan;
	struct zio_cset *cset;
	struct zio_ti *ti;
	struct zio_bi *bi;
	char *mask;

	switch (head->zobj_type) {
	case ZIO_DEV:
		return sprintf(buf, "%s\n", dev_name(dev));
	case ZIO_CSET:
		cset = to_zio_cset(dev);
		return sprintf(buf, "%s-%i\n",
			       dev_name(&cset->zdev->head.dev), cset->index);
	case ZIO_CHAN:
		chan = to_zio_chan(dev);
		mask = chan->flags & ZIO_CSET_CHAN_INTERLEAVE ? "%s-%i-i\n" :
								"%s-%i-%i\n";
		return sprintf(buf, mask, dev_name(&chan->cset->zdev->head.dev),
			       chan->cset->index, chan->index);
	case ZIO_TI:
		ti = to_zio_ti(dev);
		cset = ti->cset;
		return sprintf(buf, "%s-%i-t\n",
			       dev_name(&cset->zdev->head.dev), cset->index);
	case ZIO_BI:
		bi = to_zio_bi(dev);
		chan = bi->chan;
		mask = chan->flags & ZIO_CSET_CHAN_INTERLEAVE ? "%s-%i-i-b\n" :
								"%s-%i-%i-b\n";
		return sprintf(buf, mask, dev_name(&chan->cset->zdev->head.dev),
			       chan->cset->index, chan->index);
	default:
		WARN(1, "ZIO: unknown zio object %i for address\n",
			head->zobj_type);
		return -EINVAL;
	}
	return 0;

}

/*
 * zobj_show_alarm
 * It get alarm status from the current control of the channel and it return
 * the value to sysfs
 */
static ssize_t zio_show_alarm(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct zio_channel *chan = to_zio_chan(dev);
	struct zio_control *ctrl = chan->current_ctrl;

	return sprintf(buf, "%d %d\n", ctrl->zio_alarms, ctrl->drv_alarms);
}

/*
 * zio_store_alarm
 * It get from the user the alarm to clear, then it clears the alarm in the
 * curren control of the channel
 */
static ssize_t zio_store_alarm(struct device *dev,
			       struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct zio_channel *chan = to_zio_chan(dev);
	struct zio_control *ctrl = chan->current_ctrl;
	unsigned int v1, v2;

	switch (sscanf(buf, "%i %i", &v1, &v2)) {
	case 2:
		ctrl->drv_alarms &= (~v2);
	case 1:
		ctrl->zio_alarms &= (~v1);
		break;
	default:
		return -EINVAL;
	}
	return count;
}

/**
 * The function is associated to the sysfs buffer's attribute 'flush'. On
 * write, it flushes all blocks from the given buffer. While the buffer
 * is flushing, nor store nor retrieve operations are allowed on it. Because
 * of this, if the user perform a flush while the trigger is running he
 * may loose some blocks. The flashing operation can disable the trigger but it
 * does not do it; it leaves the user application free to enable/disable the
 * trigger during flush
 */
static ssize_t zio_buf_flush(struct device *dev,
			    struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct zio_bi *bi = to_zio_bi(dev);
	struct zio_block *block;
	unsigned long flags;
	struct zio_ti *ti;
	int n = 0;

	ti = bi->cset->ti;
	if (!(ti->flags & ZIO_DISABLED))
		dev_warn(dev, "Flushing while trigger is active\n");

	/* Disable buffer. No store/retrieve are allowed */
	spin_lock_irqsave(&bi->lock, flags);
	bi->flags |= ZIO_DISABLED;
	spin_unlock_irqrestore(&bi->lock, flags);

	/* Flushing blocks. Buffer helpers may use locks, so we not use helpers
	   here to prevent deadlock */
	while ((block = bi->b_op->retr_block(bi))) {
		dev_dbg(dev, "Flushing ... (%d)\n", n++);
		bi->b_op->free_block(bi, block);
	}

	/* Flushing is over. Enable buffer */
	spin_lock_irqsave(&bi->lock, flags);
	bi->flags &= ~ZIO_DISABLED;
	spin_unlock_irqrestore(&bi->lock, flags);

	return count;
}


/**
 * It returns the data direction of a given channel set
 */
static ssize_t zio_show_dire(struct device *dev,
			      struct device_attribute *attr, char *buf)
{
	struct zio_cset *cset = to_zio_cset(dev);

	return sprintf(buf, "%s\n", cset->flags & ZIO_DIR ? "output" : "input");
}

/**
 * It configures the buffer preference:
 * 0 - it will keep the oldest block when the buffer is full
 * 1 - it will remove old blocks in order to store the new ones
 */
static ssize_t zio_store_pref(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct zio_bi *bi = to_zio_bi(dev);
	unsigned long flags;

	spin_lock_irqsave(&bi->lock, flags);
	if (buf[0] == '0')
		bi->flags &= ~ZIO_BI_PREF_NEW;
	else
		bi->flags |= ZIO_BI_PREF_NEW;
	spin_unlock_irqrestore(&bi->lock, flags);

	return count;
}
static ssize_t zio_show_pref(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct zio_bi *bi = to_zio_bi(dev);

	return sprintf(buf, "%d\n", !!(bi->flags & ZIO_BI_PREF_NEW));
}


static ssize_t zio_show_inte(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct zio_channel *chan = to_zio_chan(dev);

	return sprintf(buf, "%d\n", !!(chan->flags & ZIO_CSET_CHAN_INTERLEAVE));
}

#if ZIO_HAS_BINARY_CONTROL
/*
 * zobj_read_cur_ctrl
 * it returns the current control to userspace through binary sysfs file
 */
ssize_t zobj_read_cur_ctrl(struct file *file, struct kobject *kobj,
			   struct bin_attribute *bin_attr,
			   char *buf, loff_t off, size_t count)
{
	struct zio_channel *chan;

	if (off >= bin_attr->size)
		return 0;

	/* This file must be read entirely */
	if (off != 0)
		return -ESPIPE; /* Illegal seek */
	if (count < bin_attr->size)
		return -EINVAL;
	if (count > bin_attr->size)
		count = bin_attr->size;

	chan = to_zio_chan(container_of(kobj, struct device, kobj));
	memcpy(buf, chan->current_ctrl, count); /* FIXME: lock */

	return count;
}

static ssize_t zobj_show_address(struct file *file, struct kobject *kobj,
				 struct bin_attribute *bin_attr,
				 char *buf, loff_t off, size_t count)
{
	struct zio_channel *chan;

	if (off >= bin_attr->size)
		return 0;

	/* This file must be read entirely */
	if (off != 0)
		return -ESPIPE; /* Illegal seek */
	if (count < bin_attr->size)
		return -EINVAL;
	if (count > bin_attr->size)
		count = bin_attr->size;

	chan = to_zio_chan(container_of(kobj, struct device, kobj));
	memcpy(buf, &chan->current_ctrl->addr, count); /* FIXME: lock */

	return count;

}

struct bin_attribute zio_bin_attr[] = {
	[ZIO_BIN_CTRL] = {
		.attr = { .name = "current-control", .mode = ZIO_RO_PERM, },
		.size = __ZIO_CONTROL_SIZE, /* changed at runtime for TLV */
		.read = zobj_read_cur_ctrl,
	},
	[ZIO_BIN_ADDR] = {
		.attr = { .name = "address", .mode = ZIO_RO_PERM, },
		.size = sizeof(struct zio_addr),
		.read = zobj_show_address,
	}
};

#endif /* ZIO_HAS_BINARY_CONTROL */

/* This is only for internal use (DAN == default attribute name) */
enum zio_default_attribute_numeration {
	ZIO_DAN_NAME = 0,	/* name */
	ZIO_DAN_ENAB,	/* enable */
	ZIO_DAN_CTRI,	/* current_trigger */
	ZIO_DAN_CBUF,	/* current_buffer */
	ZIO_DAN_DNAM,	/* devname */
	ZIO_DAN_TYPE,	/* devtype */
	ZIO_DAN_FLUS,	/* flush */
	ZIO_DAN_ALAR,	/* alarms */
	ZIO_DAN_DIRE,   /* direction */
	ZIO_DAN_PREF,	/* prefer-new */
	ZIO_DAN_INTE,	/* interleave */
};

/* default zio attributes */
static struct device_attribute zio_default_attributes[] = {
	[ZIO_DAN_NAME] = __ATTR(name, ZIO_RO_PERM,
				zobj_show_name, NULL),
	[ZIO_DAN_ENAB] = __ATTR(enable, ZIO_RW_PERM,
				zobj_show_enable, zobj_store_enable),
	[ZIO_DAN_CTRI] = __ATTR(current_trigger, ZIO_RW_PERM,
				zobj_show_cur_trig, zobj_store_cur_trig),
	[ZIO_DAN_CBUF] = __ATTR(current_buffer, ZIO_RW_PERM,
				zobj_show_cur_zbuf, zobj_store_cur_zbuf),
	[ZIO_DAN_DNAM] = __ATTR(devname, ZIO_RO_PERM,
				zobj_show_devname, NULL),
	[ZIO_DAN_TYPE] = __ATTR(devtype, ZIO_RO_PERM,
				zobj_show_dev_type, NULL),
	[ZIO_DAN_FLUS] = __ATTR(flush, ZIO_WO_PERM,
				NULL, zio_buf_flush),
	[ZIO_DAN_ALAR] = __ATTR(alarms, ZIO_RW_PERM,
				zio_show_alarm, zio_store_alarm),
	[ZIO_DAN_DIRE] = __ATTR(direction, ZIO_RO_PERM,
				zio_show_dire, NULL),
	[ZIO_DAN_PREF] = __ATTR(prefer-new, ZIO_RW_PERM,
				zio_show_pref, zio_store_pref),
	[ZIO_DAN_INTE] = __ATTR(interleave, ZIO_RW_PERM,
				zio_show_inte, NULL),
	__ATTR_NULL,
};
/* default attributes for most of the zio objects */
static struct attribute *def_obj_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_DNAM].attr,
	&zio_default_attributes[ZIO_DAN_NAME].attr,
	&zio_default_attributes[ZIO_DAN_ENAB].attr,
	&zio_default_attributes[ZIO_DAN_TYPE].attr,
	NULL,
};
/* default attributes for hierachy components (dev/cset/chan) */
static struct attribute *def_hie_attrs_ptr[] = {
	NULL,
};
/* default attributes for channel set */
static struct attribute *def_cset_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_CTRI].attr,
	&zio_default_attributes[ZIO_DAN_CBUF].attr,
	&zio_default_attributes[ZIO_DAN_DIRE].attr,
	NULL,
};
/* default attributes for channel */
static struct attribute *def_chan_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_ALAR].attr,
	&zio_default_attributes[ZIO_DAN_INTE].attr,
	NULL,
};
/* default attributes for buffer instance */
static struct attribute *def_bi_attrs_ptr[] = {
	&zio_default_attributes[ZIO_DAN_DNAM].attr,
	&zio_default_attributes[ZIO_DAN_NAME].attr,
	&zio_default_attributes[ZIO_DAN_TYPE].attr,
	&zio_default_attributes[ZIO_DAN_FLUS].attr,
	&zio_default_attributes[ZIO_DAN_PREF].attr,
	NULL,
};

/* This is only for internal use (DAG = default attribute group) */
enum zio_default_attribute_group_enumeration {
	ZIO_DAG_ALL = 0,	/* Group valid for any ZIO object*/
	ZIO_DAG_CSET,	/* Only for cset */
	ZIO_DAG_CHAN,	/* Only for channel */
	ZIO_DAG_BI,	/* Only for buffer instance */
	ZIO_DAG_HIE,	/* Only within device hierarchy (dev, cset, chan) */
};

/* default zio groups */
static const struct attribute_group zio_groups[] = {
	[ZIO_DAG_ALL] = {	/* group for all zio object*/
		.attrs = def_obj_attrs_ptr,
	},
	[ZIO_DAG_CSET] = {	/* cset only group */
		.attrs = def_cset_attrs_ptr,
	},
	[ZIO_DAG_CHAN] = {
		.attrs = def_chan_attrs_ptr,
	},
	[ZIO_DAG_BI] = {	/* bi only group */
		.attrs = def_bi_attrs_ptr,
	},
	[ZIO_DAG_HIE] = {	/* group for all device hierarchy objects */
		.attrs = def_hie_attrs_ptr,
	},
};
/* default groups for whole-device */
const struct attribute_group *def_zdev_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	&zio_groups[ZIO_DAG_HIE],
	NULL,
};
/* default groups for channel set */
const struct attribute_group *def_cset_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	&zio_groups[ZIO_DAG_CSET],
	&zio_groups[ZIO_DAG_HIE],
	NULL,
};
/* default groups for channel */
const struct attribute_group *def_chan_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	&zio_groups[ZIO_DAG_CHAN],
	&zio_groups[ZIO_DAG_HIE],
	NULL,
};
/* default groups for trigger instance */
const struct attribute_group *def_ti_groups_ptr[] = {
	&zio_groups[ZIO_DAG_ALL],
	NULL,
};
/* default groups for buffer instance */
const struct attribute_group *def_bi_groups_ptr[] = {
	&zio_groups[ZIO_DAG_BI],
	NULL,
};


static ssize_t zio_show_attr_version(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct zio_attribute *zattr = to_zio_zattr(attr);
	unsigned int major, minor, flags;

	major = (zattr->value & 0xFF000000) >> 24;
	minor = (zattr->value & 0x00FF0000) >> 16;
	flags = (zattr->value & 0xFFFF);
	if (flags)
		return sprintf(buf, "%d.%d 0x%04x\n", major, minor, flags);
	else
		return sprintf(buf, "%d.%d\n", major, minor);
}


int __check_dev_zattr(struct zio_attribute_set *parent,
		      struct zio_attribute_set *this)
{
	int i, j;

	pr_debug("%s %d\n", __func__, this->n_std_attr);
	/* verify standard attribute */
	for (i = 0; i < this->n_std_attr; ++i) {
		if (this->std_zattr[i].index == ZIO_ATTR_INDEX_NONE)
			continue; /* next attribute */
		for (j = 0; j < parent->n_std_attr; ++j) {
			/*
			 * a standard attribute must be unique from a child
			 * to the root. This allow to create a consistent
			 * vector of value in control structure
			 */
			if (this->std_zattr[i].index ==
						parent->std_zattr[j].index) {
				pr_err("ZIO: attribute conflict for %s\n",
				       this->std_zattr[i].attr.attr.name);
				return -EINVAL;
			}
		}
	}
	return 0;
}
static int __check_attr(struct attribute *attr,
			const struct zio_sysfs_operations *s_op)
{
	/* check name*/
	if (!attr->name)
		return -EINVAL;

	/* check mode */
	if ((attr->mode & S_IWUGO) == S_IWUGO && !s_op->conf_set) {
		pr_err("ZIO: %s: attribute %s has write permission but "
			"no write function\n", __func__, attr->name);
		return -ENOSYS;
	}
	return 0;
}


/* create a set of zio attributes: the standard one and the extended one */
static int zattr_set_create(struct zio_obj_head *head,
		     const struct zio_sysfs_operations *s_op)
{
	int i, err, a_count = 0, n_attr;
	const struct attribute_group **groups;
	struct attribute_group *group;
	struct zio_attribute_set *zattr_set;
	struct zio_attribute *zattr;
	struct attribute *attr;

	zattr_set = zio_get_from_obj(head, zattr_set);
	if (!zattr_set)
		return -EINVAL; /* message already printed */

	if (!(zattr_set->std_zattr && zattr_set->n_std_attr))
		zattr_set->n_std_attr = 0;
	if (!(zattr_set->ext_zattr && zattr_set->n_ext_attr))
		zattr_set->n_ext_attr = 0;
	n_attr = zattr_set->n_std_attr + zattr_set->n_ext_attr;
	if (!n_attr)
		goto out;


	/* Allocate needed groups. dev->groups is null ended */
	groups = kzalloc(sizeof(struct attribute_group *) * 2, GFP_KERNEL);
	if (!groups)
		return -ENOMEM;
	group = kzalloc(sizeof(struct attribute_group), GFP_KERNEL);
	if (!group) {
	        err = -ENOMEM;
		goto err_grp;
	}
	group->attrs = kzalloc(sizeof(struct attribute) * n_attr, GFP_KERNEL);
	if (!group->attrs) {
	        err = -ENOMEM;
		goto err_attrs;
	}


	if (!zattr_set->std_zattr || !zattr_set->n_std_attr)
		goto ext; /* Continue with extended attributes */
	/* Fill attribute group with standard attributes */
	for (i = 0; i < zattr_set->n_std_attr; ++i) {
		zattr = &zattr_set->std_zattr[i];
		attr = &zattr->attr.attr;
		err = __check_attr(attr, s_op);
		dev_vdbg(&head->dev, "%s(std): %s %d %s\n", __func__,
			head->name, i, attr->name);
		switch (err) {
		case 0:
			/* valid attribute */
			group->attrs[a_count++] = attr;
			if (i == ZIO_ATTR_VERSION) {
				zattr->attr.show = zio_show_attr_version;
			} else { /* All other attributes */
				zattr->attr.show = zattr_show;
				zattr->attr.store = zattr_store;
				zattr->s_op = s_op;
			}
			zattr->index = i;
			zattr->parent = head;
			break;
		case -EINVAL: /* unused std attribute */
			zattr->index = ZIO_ATTR_INDEX_NONE;
			break;
		default:
			return err;
		}
	}
ext:
	if (!zattr_set->ext_zattr || !zattr_set->n_ext_attr)
		goto out_assign; /* Continue to the assignment */
	/* Fill attribute group with extended attributes */
	for (i = 0; i < zattr_set->n_ext_attr; ++i) {
		zattr = &zattr_set->ext_zattr[i];
		attr = &zattr->attr.attr;
		err = __check_attr(attr, s_op);
		if (err)
		        goto err_ext;
		dev_vdbg(&head->dev, "%s(ext): %s %d %s\n", __func__,
			head->name, i, attr->name);
		/* valid attribute */
		group->attrs[a_count++] = attr;
		zattr->attr.show = zattr_show;
		zattr->attr.store = zattr_store;
		zattr->s_op = s_op;
		zattr->index = i;
		zattr->parent = head;
		zattr->flags |= ZIO_ATTR_TYPE_EXT;
	}

out_assign:
	groups[0] = group;
	groups[1] = NULL;
	head->dev.groups = groups;
out:
	return 0;
err_ext:
	kfree(group->attrs);
err_attrs:
	kfree(group);
err_grp:
	kfree(groups);
	return err;
}
/* Remove an existent set of attributes */
static void zattr_set_remove(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;
	int i;

	zattr_set = zio_get_from_obj(head, zattr_set);
	if (!zattr_set)
		return;
	if (!head->dev.groups)
		return;
	for (i = 0; head->dev.groups[i]; ++i) {
		kfree(head->dev.groups[i]->attrs);
		kfree(head->dev.groups[i]);
	}
	kfree(head->dev.groups);
}

/*
 * zio_create_attributes
 * @head: the head of the ZIO object where creates attributes
 * @s_op: the sysfs operations to associte do the attributes
 * @zattr_set_tmpl: the attribute template to use
 *
 * This function copies a set of attributes from a given template and then
 * it creates the attribute group for a given ZIO object.
 */
int zio_create_attributes(struct zio_obj_head *head,
			  const struct zio_sysfs_operations *s_op,
			  struct zio_attribute_set *zattr_set_tmpl)
{
	struct zio_attribute_set *zattr_set;
	int err;

	zattr_set = zio_get_from_obj(head, zattr_set);
	if (!zattr_set)
		return -EINVAL; /* message already printed */

	if (zattr_set_tmpl) {
		/* Copy sysfs attribute from template to ZIO object */
		zattr_set->n_std_attr = zattr_set_tmpl->n_std_attr;
		zattr_set->n_ext_attr = zattr_set_tmpl->n_ext_attr;
		zattr_set->std_zattr = __zattr_clone(zattr_set_tmpl->std_zattr,
						     zattr_set->n_std_attr);
		zattr_set->ext_zattr = __zattr_clone(zattr_set_tmpl->ext_zattr,
						     zattr_set->n_ext_attr);
	}

	/* Create attributes */
	err = zattr_set_create(head, s_op);
	if (err) {
		__zattr_unclone(zattr_set->ext_zattr);
		__zattr_unclone(zattr_set->std_zattr);
		return err;
	}

	return 0;
}

/*
 * zio_destroy_attributes
 * @head: the head of the ZIO object where destroy attributes
 *
 * This function removes attributes from a ZIO object and it releases memory.
 */
void zio_destroy_attributes(struct zio_obj_head *head)
{
	struct zio_attribute_set *zattr_set;

	zattr_set = zio_get_from_obj(head, zattr_set);
	if (!zattr_set)
		return; /* message already printed */

	/* Remove zio attribute from the zio object */
	zattr_set_remove(head);
	/* Release attributes from memory */
	__zattr_unclone(zattr_set->ext_zattr);
	__zattr_unclone(zattr_set->std_zattr);
}
