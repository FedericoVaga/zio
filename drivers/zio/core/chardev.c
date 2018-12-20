/*
 * Copyright 2011 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/poll.h>
#include <linux/version.h>
#if KERNEL_VERSION(4, 11, 0) > LINUX_VERSION_CODE
#include <linux/sched.h>
#else
#include <linux/sched/signal.h>
#endif
#include <linux/uaccess.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include "zio-internal.h"

static DEFINE_MUTEX(zmutex);
static struct zio_status *zstat = &zio_global_status; /* Always use ptr */

static int zio_dev_uevent(struct device *dev, struct kobj_uevent_env *env)
{
	unsigned long *flags;

	flags = dev_get_drvdata(dev);
	add_uevent_var(env, "DEVMODE=%#o", (*flags & ZIO_DIR ? 0220 : 0440));

	return 0;
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(3,2,0)
static char *zio_devnode(struct device *dev, umode_t *mode)
#else
static char *zio_devnode(struct device *dev, mode_t *mode)
#endif
{
	return kasprintf(GFP_KERNEL, "zio/%s", dev_name(dev));
}

/*
 * zio_cdev_class: don't use class_create to create class, as it doesn't permit
 * to insert a set of class attributes. This structure is the exact
 * reproduction of what class_create does but with some additional settings.
 */
static struct class zio_cdev_class = {
	.name		= "zio-cdev",
	.owner		= THIS_MODULE,
	.dev_uevent	= zio_dev_uevent,
	.devnode	= zio_devnode,
};

/* Retrieve a channel from one of its minors */
static struct zio_channel *zio_minor_to_chan(int minor)
{
	struct zio_cset *zcset;
	int chindex, found = 0;

	list_for_each_entry(zcset, &zstat->list_cset, list_cset) {
		if (minor >= zcset->minor && minor <= zcset->maxminor) {
			found = 1;
			break;
		}
	}
	if (!found)
		return NULL;
	chindex = (minor - zcset->minor) / 2;
	return zcset->chan + chindex;
}

static inline int zio_channel_get(struct zio_channel *chan)
{
	return try_module_get(chan->cset->zdev->owner);
	/* bi use count is done in the caller */
}
static inline void zio_channel_put(struct zio_channel *chan)
{
	atomic_dec(&chan->bi->use_count);
	module_put(chan->cset->zdev->owner);
}

static int zio_f_open(struct inode *ino, struct file *f)
{
	struct zio_f_priv *priv = NULL;
	struct zio_channel *chan;
	struct zio_buffer_type *zbuf;
	const struct file_operations *old_fops, *new_fops;
	unsigned long flags;
	int err, minor;

	minor = iminor(ino);
	chan = zio_minor_to_chan(minor);

	if (!chan || !chan->bi || !zio_channel_get(chan)) {
		pr_err("%s: no channel or no buffer for minor %i\n",
			__func__, minor);
		return -ENODEV;
	}

	/* Take the cset lock to protect against a cset-wide buffer change */
	spin_lock_irqsave(&chan->cset->lock, flags);
	atomic_inc(&chan->bi->use_count);
	err = (chan->bi->flags & ZIO_STATUS) == ZIO_DISABLED ? -EAGAIN : 0;
	spin_unlock_irqrestore(&chan->cset->lock, flags);
	if (err)
		goto out;

	priv = kzalloc(sizeof(struct zio_f_priv), GFP_KERNEL);
	if (!priv) {
		err = -ENOMEM;
		goto out;
	}
	priv->chan = chan;

	/* even number is control, odd number is data */
	if (minor & 0x1)
		priv->type = ZIO_CDEV_DATA;
	else
		priv->type = ZIO_CDEV_CTRL;

	/* Change the file operations, locking the status structure */
	mutex_lock(&zmutex);
	old_fops = f->f_op;
	zbuf = chan->cset->zbuf;
	new_fops = fops_get(zbuf->f_op);
	err = 0;
	if (new_fops->open)
		err = new_fops->open(ino, f);
	if (err) {
		fops_put(zbuf->f_op);
		mutex_unlock(&zmutex);
		goto out;
	}
	fops_put(old_fops);
	f->f_op = new_fops;
	mutex_unlock(&zmutex);

	f->private_data = priv;
	return 0;

out:
	kfree(priv);
	zio_channel_put(chan);
	return err;
}

static const struct file_operations zfops = {
	.owner = THIS_MODULE,
	.open = zio_f_open,
};

/* set the base minor for a cset*/
int zio_minorbase_get(struct zio_cset *zcset)
{
	unsigned long i;
	int nminors = zcset->n_chan * 2;

	zio_ffa_reset(zstat->minors); /* always start from zero */
	i = zio_ffa_alloc(zstat->minors, nminors, GFP_ATOMIC);
	if (i == ZIO_FFA_NOSPACE)
		return -ENOMEM;
	zcset->minor = i;
	zcset->maxminor = i + nminors - 1;
	return 0;
}
void zio_minorbase_put(struct zio_cset *zcset)
{
	int nminors = zcset->n_chan * 2;

	zio_ffa_free_s(zstat->minors, zcset->minor, nminors);
}

/*
 * create control and data char devices for a channel. The even minor
 * is for control, the odd one for data.
 */
int zio_create_chan_devices(struct zio_channel *chan)
{
	int err;
	dev_t devt_c, devt_d;
	char *mask;

	/*
	 * if the cset is interleave only and this is not the interleaved
	 * channel, return immediately and don't create char devices
	 */
	if ((chan->cset->flags & ZIO_CSET_INTERLEAVE_ONLY) &&
	    !(chan->flags & ZIO_CSET_CHAN_INTERLEAVE))
		return 0;

	devt_c = zstat->basedev + chan->cset->minor + chan->index * 2;
	mask = chan->flags & ZIO_CSET_CHAN_INTERLEAVE ? "%s-%i-i-ctrl" :
							"%s-%i-%i-ctrl";
	chan->ctrl_dev = device_create(&zio_cdev_class, &chan->head.dev, devt_c,
			&chan->flags, mask,
			dev_name(&chan->cset->zdev->head.dev),
			chan->cset->index,
			chan->index); /* ignored on interleave */
	if (IS_ERR(&chan->ctrl_dev)) {
		err = PTR_ERR(&chan->ctrl_dev);
		goto out;
	}

	devt_d = devt_c + 1;
	mask = chan->flags & ZIO_CSET_CHAN_INTERLEAVE ? "%s-%i-i-data" :
							"%s-%i-%i-data";
	chan->data_dev = device_create(&zio_cdev_class, &chan->head.dev, devt_d,
			&chan->flags, mask,
			dev_name(&chan->cset->zdev->head.dev),
			chan->cset->index,
			chan->index); /* ignored on interleave */
	if (IS_ERR(&chan->data_dev)) {
		err = PTR_ERR(&chan->data_dev);
		goto out_data;
	}

	return 0;

out_data:
	device_destroy(&zio_cdev_class, chan->ctrl_dev->devt);
out:
	return err;
}

void zio_destroy_chan_devices(struct zio_channel *chan)
{
	pr_debug("%s\n", __func__);
	if ((chan->cset->flags & ZIO_CSET_INTERLEAVE_ONLY) &&
	    !(chan->flags & ZIO_CSET_CHAN_INTERLEAVE))
		return;

	device_destroy(&zio_cdev_class, chan->data_dev->devt);
	device_destroy(&zio_cdev_class, chan->ctrl_dev->devt);
}

int zio_register_cdev()
{
	int err;

	err = class_register(&zio_cdev_class);
	if (err) {
		pr_err("%s: unable to register class\n", __func__);
		goto out;
	}
	/* alloc to zio the maximum number of minors usable in ZIO */
	zstat->minors = zio_ffa_create(0, ZIO_NR_MINORS);
	err = alloc_chrdev_region(&zstat->basedev, 0, ZIO_NR_MINORS, "zio");
	if (err) {
		pr_err("%s: unable to allocate region for %i minors\n",
		       __func__, ZIO_NR_MINORS);
		goto out;
	}

	cdev_init(&zstat->chrdev, &zfops);
	zstat->chrdev.owner = THIS_MODULE;
	err = cdev_add(&zstat->chrdev, zstat->basedev, ZIO_NR_MINORS);
	if (err)
		goto out_cdev;
	INIT_LIST_HEAD(&zstat->list_cset);
	return 0;
out_cdev:
	unregister_chrdev_region(zstat->basedev, ZIO_NR_MINORS);
out:
	class_unregister(&zio_cdev_class);
	zio_ffa_destroy(zstat->minors);

	return err;
}
void zio_unregister_cdev()
{
	cdev_del(&zstat->chrdev);
	unregister_chrdev_region(zstat->basedev, ZIO_NR_MINORS);
	class_unregister(&zio_cdev_class);
	zio_ffa_destroy(zstat->minors);
}


/*
 * Helper functions to check whether read and write would block. The
 * return value is a poll(2) mask, so the poll method just calls them.
 * We need locking, so to avoid hairy ifs we split read/write and ctrl/data
 * Both functions return with a user block if read/write can happen.
 */

static int zio_can_r_ctrl(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	const int ret_ok =  POLLIN | POLLRDNORM;
	int ret;

	dev_dbg(&bi->head.dev, "%s: channel %d in cset %d", __func__,
		bi->chan->index, bi->chan->cset->index);

	/* If we want to read control, we discard any trailing data */
	mutex_lock(&chan->user_lock);

	/* Control: if not yet done, we can read */
	if (chan->user_block) {
		if (zio_is_cdone(chan->user_block)) {
			zio_buffer_free_block(bi, chan->user_block);
			chan->user_block = NULL;
		} else{
			mutex_unlock(&chan->user_lock);
			return ret_ok;
		}
	}

	/* We want to re-read control. Get a new block */
	chan->user_block = zio_buffer_retr_block(bi);
	ret = 0;
	if (chan->user_block)
		ret = ret_ok;
	else if (unlikely(bi->cset->ti->flags & ZIO_DISABLED))
		ret = POLLERR;

	mutex_unlock(&chan->user_lock);
	return ret;
}

static int zio_can_r_data(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_block *block;
	struct zio_bi *bi = chan->bi;
	const int ret_ok =  POLLIN | POLLRDNORM;

	if (!chan->cset->ssize)
		return 0;

	mutex_lock(&chan->user_lock);
	block = chan->user_block;
	if (block) {
		mutex_unlock(&chan->user_lock);
		return ret_ok;
	}
	block = chan->user_block = zio_buffer_retr_block(bi);
	mutex_unlock(&chan->user_lock);
	if (block)
		return ret_ok;
	return 0;
}

static struct zio_block *__zio_write_allocblock(struct zio_bi *bi)
{
	struct zio_cset *cset = bi->chan->cset;
	size_t datalen;

	datalen = cset->ssize * cset->ti->nsamples;
	return zio_buffer_alloc_block(bi, datalen, GFP_KERNEL);
}

static int zio_can_w_ctrl(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;
	struct zio_control *ctrl;
	const int ret_ok = POLLOUT | POLLWRNORM;
	int ret;

	/*
	 * A control can always be written. Writing a control means a
	 * new block is being created, so store the previous one.
	 *
	 * FIXME: shall we pick the nsamples from this control?
	 * We currently obey trigger configuration and ignore the control.
	 */
	mutex_lock(&chan->user_lock);
	block = chan->user_block;
	if (block && block->uoff) {
		/* store a partial block */
		ctrl = zio_get_ctrl(block);
		ctrl->nsamples = block->uoff / chan->cset->ssize;
		if (ctrl->nsamples)
			zio_buffer_store_block(bi, block);
		else
		        zio_buffer_free_block(chan->bi, block);
		block = NULL;
	}
	/* if no block is there, get a new one */
	if (!block)
		block = chan->user_block = __zio_write_allocblock(bi);
	ret = 0;
	if (block)
		ret = ret_ok;
	else if (unlikely(bi->cset->ti->flags & ZIO_DISABLED))
		ret = POLLERR;
	mutex_unlock(&chan->user_lock);
	return ret;
}

static int zio_can_w_data(struct zio_f_priv *priv)
{
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;
	const int ret_ok = POLLOUT | POLLWRNORM;

	if (!chan->cset->ssize)
		return 0;

	mutex_lock(&chan->user_lock);
	block = chan->user_block;
	if (!block)
		block = chan->user_block = __zio_write_allocblock(bi);
	mutex_unlock(&chan->user_lock);
	return block ? ret_ok : 0;
}

/*
 * The following "generic" read and write (and poll and so on) should
 * work for most buffer types, and are exported for use in their
 * buffer operations.
 */
static ssize_t zio_generic_read(struct file *f, char __user *ubuf,
			 size_t count, loff_t *offp)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;
	int (*can_read)(struct zio_f_priv *);
	int fault, rflags;

	dev_dbg(&bi->head.dev, "%s:%d type %s\n", __func__, __LINE__,
		priv->type == ZIO_CDEV_CTRL ? "ctrl" : "data");

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT)
		return -EINVAL;

	can_read = zio_can_r_data;
	if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
		if (count < zio_control_size(chan))
			return -EINVAL;
		count = zio_control_size(chan);
		can_read = zio_can_r_ctrl;
	}

	while (1) {
		rflags = can_read(priv);
		if (rflags == 0 || rflags == POLLERR) {
			if (f->f_flags & O_NONBLOCK)
				return -EAGAIN;
			wait_event_interruptible(bi->q, can_read(priv));
			if (signal_pending(current))
				return -ERESTARTSYS;
		}

		/* So, it has been readable, at least for a little while */
		mutex_lock(&chan->user_lock);
		block = chan->user_block;
		if (!block) {
			mutex_unlock(&chan->user_lock);
			continue;
		}

		if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
			if (zio_is_cdone(block)) {
				mutex_unlock(&chan->user_lock);
				continue;
			}
			fault = copy_to_user(ubuf, zio_get_ctrl(block), count);
			mutex_unlock(&chan->user_lock);
			if (fault)
				return -EFAULT;
			zio_set_cdone(block);
			*offp += count;
			return count;
		}

		/* data */
		if (count > block->datalen - block->uoff)
			count = block->datalen - block->uoff;
		fault = copy_to_user(ubuf, block->data + block->uoff, count);
		if (!fault) {
			block->uoff += count;
			if (block->uoff == block->datalen) {
				chan->user_block = NULL;
				zio_buffer_free_block(bi, block);
			}
		}
		mutex_unlock(&chan->user_lock);
		if (fault)
			return -EFAULT;
		*offp += count;
		return count;
	}
}

static ssize_t zio_generic_write(struct file *f, const char __user *ubuf,
			  size_t count, loff_t *offp)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_bi *bi = chan->bi;
	struct zio_block *block;
	int (*can_write)(struct zio_f_priv *);
	int fault, wflags;

	dev_dbg(&bi->head.dev, "%s:%d type %s\n", __func__, __LINE__,
		priv->type == ZIO_CDEV_CTRL ? "ctrl" : "data");

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT)
		return -EINVAL;

	can_write = zio_can_w_data;
	if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
		if (count < zio_control_size(chan))
			return -EINVAL;
		can_write = zio_can_w_ctrl;
	}

	while (1) {
		wflags = can_write(priv);
		if (wflags == 0 || wflags == POLLERR) {
			if (f->f_flags & O_NONBLOCK)
				return -EAGAIN;
			wait_event_interruptible(bi->q, can_write(priv));
			if (signal_pending(current))
				return -ERESTARTSYS;
		}

		/* So, it has been writeable, at least for a little while */
		mutex_lock(&chan->user_lock);
		block = chan->user_block;
		if (!block) {
			mutex_unlock(&chan->user_lock);
			continue;
		}

		if (unlikely(priv->type == ZIO_CDEV_CTRL)) {
			count = zio_control_size(chan);
			/*
			 * FIXME: what shall we do for already-filled data?
			 * we are currently discarding it
			 */
			block->uoff = 0;
			fault = copy_from_user(zio_get_ctrl(block), ubuf,
					       count);
			/* FIXME: preserve some fields in the output ctrl */
			if (!fault && !chan->cset->ssize) {
				zio_buffer_store_block(bi, block); /* 0-size */
				chan->user_block = NULL;
			}
			mutex_unlock(&chan->user_lock);
			if (fault)
				return -EFAULT;
			*offp += count;
			return count;
		}

		/* data */
		if (count > block->datalen - block->uoff)
			count =  block->datalen - block->uoff;
		fault = copy_from_user(block->data + block->uoff, ubuf, count);
		if (!fault) {
			block->uoff += count;
			if (block->uoff == block->datalen) {
				zio_buffer_store_block(bi, block);
				chan->user_block = NULL;
			}
		}
		mutex_unlock(&chan->user_lock);
		if (fault)
			return -EFAULT;
		*offp += count;
		return count;
	}
}

static int zio_generic_mmap(struct file *f, struct vm_area_struct *vma)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;
	const struct vm_operations_struct *v_op = bi->v_op;
	unsigned long flags;
	int ret;

	dev_dbg(&bi->head.dev, "%s: channel %d in cset %d", __func__,
		bi->chan->index, bi->chan->cset->index);
	if (!v_op)
		return -ENODEV; /* according to man page */
	spin_lock_irqsave(&bi->lock, flags);
	if (bi->flags & ZIO_DISABLED) {
		ret = -EBUSY;
	} else {
		ret = 0;
		/* The buffer instance may usecount, so notifiy it */
		vma->vm_ops = v_op;
		if (v_op->open)
			v_op->open(vma); /* returns void */
	}
	spin_unlock_irqrestore(&bi->lock, flags);
	return ret;
}

static unsigned int zio_generic_poll(struct file *f,
				     struct poll_table_struct *w)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;

	dev_dbg(&bi->head.dev, "%s: channel %d in cset %d", __func__,
		bi->chan->index, bi->chan->cset->index);
	poll_wait(f, &bi->q, w);

	if ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT) {
		if (unlikely(priv->type == ZIO_CDEV_CTRL))
			return zio_can_w_ctrl(priv);
		return zio_can_w_data(priv);
	}
	if (unlikely(priv->type == ZIO_CDEV_CTRL))
		return zio_can_r_ctrl(priv);
	return zio_can_r_data(priv);
}

static int zio_generic_release(struct inode *inode, struct file *f)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_channel *chan = priv->chan;
	struct zio_block *block = chan->user_block;

	mutex_lock(&chan->user_lock);
	if (atomic_read(&chan->bi->use_count) == 1 && chan->user_block) {
		zio_buffer_free_block(chan->bi, block);
		chan->user_block = NULL;
	}
	mutex_unlock(&chan->user_lock);
	zio_channel_put(chan);
	/* priv is allocated by zio_f_open, must be freed */
	kfree(priv);
	return 0;
}

const struct file_operations zio_generic_file_operations = {
	/* no owner: this template is copied over */
	.read =		zio_generic_read,
	.write =	zio_generic_write,
	.poll =		zio_generic_poll,
	.mmap =		zio_generic_mmap,
	.release =	zio_generic_release,
};
/* Export, so buffers can use it or internal function */
EXPORT_SYMBOL(zio_generic_file_operations);

/* Currently, this is a "all or nothing" choice */
int zio_init_buffer_fops(struct zio_buffer_type *zbuf)
{
	struct file_operations *ops;

	/* Current fops may be NULL (buffer for in-kernel data handling */
	if (!zbuf->f_op)
		return 0;
	if (zbuf->f_op != &zio_generic_file_operations)
		return 0;

	/* If it's the generic ones, we must clone to fit the owner field */
	ops = kzalloc(sizeof(*ops), GFP_KERNEL);
	if (!ops)
		return -ENOMEM;
	zbuf->flags |= ZIO_BUF_FLAG_ALLOC_FOPS;
	*ops = zio_generic_file_operations;
	ops->owner = zbuf->owner;
	zbuf->f_op = ops;
	return 0;
}

int zio_fini_buffer_fops(struct zio_buffer_type *zbuf)
{
	if (!(zbuf->flags & ZIO_BUF_FLAG_ALLOC_FOPS))
		return 0;
	zbuf->flags &= ~ZIO_BUF_FLAG_ALLOC_FOPS;
	kfree(zbuf->f_op);
	zbuf->f_op = &zio_generic_file_operations;
	return 0;
}
