/* Alessandro Rubini for CERN, 2012, GNU GPLv2 or later */

/*
 * Simple loop device, that allows testing with the I/O flows, triggers etc.
 *
 * cset 0:  two output channels, that send to cset 1 (0 == O == Output)
 * cset 1:  two input channels, that receive from cset 0 (1 == I == Input)
 * cset 2:  one output channel, appears as data-only to a char device
 * cset 4:  one output channel, like 2 but char device gets control+data
 *
 * cset 3 (not implemented): input channel, returns data from a char device
 * cset 5 (not implemented): input, receives ctrl and data from chardev
 */
#define DEBUG
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/version.h>
#if KERNEL_VERSION(4, 11, 0) > LINUX_VERSION_CODE
#include <linux/sched.h>
#else
#include <linux/sched/signal.h>
#endif
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/stringify.h>
#if KERNEL_VERSION(4, 14, 0) > LINUX_VERSION_CODE
#include <asm/uaccess.h>
#else
#include <linux/uaccess.h>
#endif

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

ZIO_PARAM_TRIGGER(zloop_trigger);
ZIO_PARAM_BUFFER(zloop_buffer);

/* Name the csets. Use defines so as to stringify them */
#define ZLOOP_CSET_OUT_LOOP		0
#define ZLOOP_CSET_IN_LOOP		1
#define ZLOOP_CSET_OUT_DATA		2
#define ZLOOP_CSET_IN_DATA		3
#define ZLOOP_CSET_OUT_CTRLDATA		4

/* The char devices are identified by type */
enum {
	ZLOOP_TYPE_READ_DATA,
	ZLOOP_TYPE_WRITE_DATA,
	ZLOOP_TYPE_READ_CTRLDATA,
};

/* The char devices manage one block each, but must keep global offset */
struct zloop_cdev_data {
	int type;
	int busy; /* 1-flag flags word */
	struct zio_cset *cset;
	unsigned offset, ctrl_offset;
	spinlock_t lock;
	wait_queue_head_t q;
};

static struct zloop_cdev_data zloop_cdata[] = {
	[ZLOOP_TYPE_READ_DATA] =	{.type = ZLOOP_TYPE_READ_DATA},
	[ZLOOP_TYPE_WRITE_DATA] =	{.type = ZLOOP_TYPE_WRITE_DATA},
	[ZLOOP_TYPE_READ_CTRLDATA] =	{.type = ZLOOP_TYPE_READ_CTRLDATA},
};

/*
 * This is a pretty standard zio driver, with several csets.  Cset 2
 * and later act on the previously-defined data, to communicate with
 * char devs. Cset 0 and 1 work like this: the reader or writer waits
 * until both transfers are pending. When they are, data is exchanged.
 */
static int zloop_wr_pending, zloop_rd_pending;
DEFINE_SPINLOCK(zloop_lock);

static int zloop_complete(struct zio_device *zdev, int cset_index)
{
	struct zio_cset *cset_out = zdev->cset + ZLOOP_CSET_OUT_LOOP;
	struct zio_cset *cset_in = zdev->cset + ZLOOP_CSET_IN_LOOP;
	struct zio_channel *ch_in, *ch_out;
	struct zio_control *ctrl_in, *ctrl_out;
	int isize, osize;
	int i;

	pr_debug("%s: pending is (%i,%i), cset %i\n", __func__,
		 zloop_rd_pending, zloop_wr_pending, cset_index);
	/* both are waiting, so clear the bits without locking*/
	zloop_rd_pending = zloop_wr_pending = 0;

	/* copy data from the input to the output. Can't use cset_for_each */
	for (i = 0; i < cset_in->n_chan; i++) {
		ch_in = cset_in->chan + i;
		ch_out = cset_out->chan + i;

		/*
		 * Hot point: if an output channel is disabled and
		 * the input is not, the input will be filled with
		 * zeroes. If the input is disabled, the associated
		 * output is discarded.
		 */
		if (!ch_in->active_block)
			continue;
		if (!ch_out->active_block) {
			memset(ch_in->active_block->data, 0,
			       ch_in->active_block->datalen);
			continue;
		}
		/*
		 * Hot point: if data size mismatches, the output is
		 * truncated (with data loss) or the input nsamples is
		 * padded with zeroes. Samplesize is 1 byte for sure.
		 * For output nsamples is in the active block, for
		 * input it's in the current_ctrl (the active block
		 * receives a copy of current_ctrl at the end of it all).
		 */
		ctrl_in = ch_in->current_ctrl;
		ctrl_out = zio_get_ctrl(ch_out->active_block);
		isize = ctrl_in->nsamples;
		osize = ctrl_out->nsamples;
		if (osize > isize)
			osize = isize;
		memcpy(ch_in->active_block->data, ch_out->active_block->data,
		       osize);
		if (osize < isize)
			memset(ch_in->active_block->data + osize, 0,
			       isize - osize);
	}

	/* One is calling us, the other one must be notified */
	if (cset_index == cset_out->index)
		zio_trigger_data_done(cset_in);
	else
		zio_trigger_data_done(cset_out);
	return 0; /* done now, for the caller */
}

static int zloop_try_complete(struct zio_device *zdev, int cset_index)
{
	int ret = -EAGAIN;

	pr_debug("%s -- cset %i\n", __func__, cset_index);
	spin_lock(&zloop_lock);
	if (cset_index == ZLOOP_CSET_OUT_LOOP) {
		if (zloop_wr_pending)
			goto out_warn;
		zloop_wr_pending++;
		if (zloop_rd_pending) {
			spin_unlock(&zloop_lock);
			return zloop_complete(zdev, cset_index);
		}
	} else {
		if (zloop_rd_pending)
			goto out_warn;
		zloop_rd_pending++;
		if (zloop_wr_pending) {
			spin_unlock(&zloop_lock);
			return zloop_complete(zdev, cset_index);
		}
	}
	spin_unlock(&zloop_lock);
	return ret;
out_warn:
	spin_unlock(&zloop_lock);
	WARN(1, "%s: counter corruption\n", __func__);
	return ret;
}

static int zloop_raw_output(struct zio_cset *cset)
{
	struct zloop_cdev_data *data;
	int index = -1;

	pr_debug("%s -- cset %i\n", __func__, cset->index);

	switch (cset->index) {
	case ZLOOP_CSET_OUT_LOOP:
		return zloop_try_complete(cset->zdev, cset->index);

	case ZLOOP_CSET_OUT_DATA:
		index = ZLOOP_TYPE_READ_DATA;
	case ZLOOP_CSET_OUT_CTRLDATA:
		if (index < 0)
			index = ZLOOP_TYPE_READ_CTRLDATA;
		/*
		 * Zio wrote a block on this single-channel cset, so
		 * we must make it available to the char device.
		 */
		data = zloop_cdata + index;
		wake_up_interruptible(&data->q);
		return -EAGAIN;
	}
	return -EOPNOTSUPP; /* never */
}

static int zloop_raw_input(struct zio_cset *cset)
{
	pr_debug("%s -- cset %i\n", __func__, cset->index);

	switch (cset->index) {
	case ZLOOP_CSET_IN_LOOP:
		return zloop_try_complete(cset->zdev, cset->index);
	case ZLOOP_CSET_IN_DATA:
		return -EAGAIN; /* FIXME */
	}
	return -EOPNOTSUPP; /* never */
}

/*
 * The probe function is called when the device is created.  Csets are
 * not present yet (we first must accept or refuse the device, so just
 * save the device for later.
 */
static struct zio_device *zloop_probed_dev;
static int zloop_probe(struct zio_device *zdev)
{
	zloop_probed_dev = zdev;
	return 0;
}


/* I want the cset id it be part of the name, to ease the user */
#define SET_OBJ_NAME_NUM(_name, _num) \
	ZIO_SET_OBJ_NAME(_name  "-"  __stringify(_num))

/* The list of csets: TYPE_DIGITAL is default (0) so it's not written */
static struct zio_cset zloop_cset[] = {
	[ZLOOP_CSET_OUT_LOOP] = {
		SET_OBJ_NAME_NUM("out-loop", ZLOOP_CSET_OUT_LOOP),
		.raw_io =	zloop_raw_output,
		.flags =	ZIO_DIR_OUTPUT,
		.n_chan =	2,
		.ssize =	1,
	},
	[ZLOOP_CSET_IN_LOOP] = {
		SET_OBJ_NAME_NUM("in-loop", ZLOOP_CSET_IN_LOOP),
		.raw_io =	zloop_raw_input,
		.flags =	ZIO_DIR_INPUT,
		.n_chan =	2,
		.ssize =	1,
	},
	[ZLOOP_CSET_OUT_DATA] = {
		SET_OBJ_NAME_NUM("out-data", ZLOOP_CSET_OUT_DATA),
		.raw_io =	zloop_raw_output,
		.flags =	ZIO_DIR_OUTPUT,
		.n_chan =	1,
		.ssize =	1,
	},
	[ZLOOP_CSET_IN_DATA] = {
		SET_OBJ_NAME_NUM("in-data", ZLOOP_CSET_IN_DATA),
		.raw_io =	zloop_raw_input,
		.flags =	ZIO_DIR_INPUT,
		.n_chan =	1,
		.ssize =	1,
	},
	[ZLOOP_CSET_OUT_CTRLDATA] = {
		SET_OBJ_NAME_NUM("out-ctrldata", ZLOOP_CSET_OUT_CTRLDATA),
		.raw_io =	zloop_raw_output,
		.flags =	ZIO_DIR_OUTPUT,
		.n_chan =	1,
		.ssize =	1,
	}
};

static struct zio_device zloop_tmpl = {
	.owner =		THIS_MODULE,
	.cset =			zloop_cset,
	.n_cset =		ARRAY_SIZE(zloop_cset),
};

static const struct zio_device_id zloop_table[] = {
	{"zloop", &zloop_tmpl},
	{},
};

static struct zio_driver zloop_zdrv = {
	.driver = {
		.name = "zloop",
		.owner = THIS_MODULE,
	},
	.id_table = zloop_table,
	.probe = zloop_probe,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};

/*
 * What follows is several misc devices, that work on the
 * array of data structures defined above (zio acts on them too
 */

static unsigned int zloop_poll(struct file *f, struct poll_table_struct *wait)
{
	/* FIXME poll */
	return 0;
}

static ssize_t zloop_read(struct file *f, char __user *buf, size_t count,
			  loff_t *offp)
{
	struct zloop_cdev_data *data = f->private_data;
	struct zio_cset *cset = data->cset;
	struct zio_channel *chan = cset->chan;
	struct zio_block *block;
	int trailing;
	int ccnt = 0;
	int ret = -EFAULT;

	/* FIXME: we should use a semaphore here, not a spinlock+busy */
	spin_lock(&data->lock);
	while (!chan->active_block || data->busy) {
		spin_unlock(&data->lock);
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(data->q,
					 chan->active_block && !data->busy);
		if (signal_pending(current))
			return -ERESTARTSYS;
		spin_lock(&data->lock);
	}
	data->busy = 1;
	spin_unlock(&data->lock);

	/* We are unlocked here, because readers are stopped by "busy" */
	block = chan->active_block;
	if (data->type == ZLOOP_TYPE_READ_CTRLDATA &&
	    data->ctrl_offset < zio_control_size(chan)) {
		/* return the control first, and then the data */
		ccnt = zio_control_size(chan) - data->ctrl_offset;
		if (ccnt > count)
			ccnt = count;
		if (copy_to_user(buf, zio_get_ctrl(block) + data->ctrl_offset,
				 ccnt))
			goto out;
		data->ctrl_offset += zio_control_size(chan);
		count -= ccnt;
		buf += ccnt;
		*offp += ccnt;
		/* fall through, we can copy data in the same system call */
	}
	trailing = block->datalen - data->offset;
	if (count > trailing)
		count = trailing;
	if (count)
		if (copy_to_user(buf, block->data + data->offset, count))
			goto out;
	*offp += count;
	data->offset += count;
	ret = ccnt + count;
	if (data->offset < block->datalen)
		goto out;
	/* the block is over: let data_done awake any other reader */
	data->offset = data->ctrl_offset = 0;
	data->busy = 0;
	zio_trigger_data_done(cset);
	return ret;
out:
	data->busy = 0;
	wake_up_interruptible(&data->q);
	return ret;
}

static ssize_t zloop_write(struct file *f, const char __user *buf,
			   size_t count, loff_t *offp)
{
	/* FIXME write */
	return -ENOSYS;
}

/* We need 3 different open calls, to make private data point to each data */
static int zloop_open_r_data(struct inode *ino, struct file *f)
{
	struct zloop_cdev_data *data;

	data = zloop_cdata + ZLOOP_TYPE_READ_DATA;
	data->cset = zloop_probed_dev->cset + ZLOOP_CSET_OUT_DATA;
	f->private_data = data;
	return 0;
}

static int zloop_open_w_data(struct inode *ino, struct file *f)
{
	struct zloop_cdev_data *data;

	data = zloop_cdata + ZLOOP_TYPE_WRITE_DATA;
	data->cset = zloop_probed_dev->cset + ZLOOP_CSET_IN_DATA;
	f->private_data = data;
	return 0;
}

static int zloop_open_r_ctrldata(struct inode *ino, struct file *f)
{
	struct zloop_cdev_data *data;

	data = zloop_cdata + ZLOOP_TYPE_READ_CTRLDATA;
	data->cset = zloop_probed_dev->cset + ZLOOP_CSET_OUT_CTRLDATA;
	f->private_data = data;
	return 0;
}


/* Three different fops, so we can't differentiate at open time */
static struct file_operations zloop_fops_r_data = {
	.owner = THIS_MODULE,
	.read = zloop_read,
	.poll = zloop_poll,
	.open = zloop_open_r_data,
	.llseek = no_llseek,
};

static struct file_operations zloop_fops_w_data = {
	.owner = THIS_MODULE,
	.write = zloop_write,
	.poll = zloop_poll,
	.open = zloop_open_w_data,
	.llseek = no_llseek,
};

static struct file_operations zloop_fops_r_ctrldata = {
	.owner = THIS_MODULE,
	.read = zloop_read,
	.poll = zloop_poll,
	.open = zloop_open_r_ctrldata,
	.llseek = no_llseek,
};


/* Three different misc devices */
static struct miscdevice zloop_misc[] = {
	{
		.minor = MISC_DYNAMIC_MINOR,
		.name = "zio-loop-2-r-data",
		.fops = &zloop_fops_r_data,
	}, {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "zio-loop-3-w-data",
		.fops = &zloop_fops_w_data,
	}, {
		.minor = MISC_DYNAMIC_MINOR,
		.name = "zio-loop-4-r-ctrldata",
		.fops = &zloop_fops_r_ctrldata,
	},
};

/*
 * Last chapter of the file: registering and unregistering
 */
static int zloop_register_miscdevs(void)
{
	int i, err;

	for (i = 0; i < ARRAY_SIZE(zloop_misc); i++) {
		err = misc_register(zloop_misc + i);
		if (err < 0)
			break;
	}
	if (err == 0)
		return 0;
	for (i--; i >= 0; i--)
		misc_deregister(zloop_misc + i);

	return err;
}

static void zloop_unregister_miscdevs(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(zloop_misc); i++)
		misc_deregister(zloop_misc + i);
}

static struct zio_device *zloop_hwdev;

static int __init zloop_init(void)
{
	int err, i;

	if (zloop_trigger)
		zloop_tmpl.preferred_trigger = zloop_trigger;
	if (zloop_buffer)
		zloop_tmpl.preferred_buffer = zloop_buffer;

	for (i = 0; i < ARRAY_SIZE(zloop_cdata); i++) {
		spin_lock_init(&zloop_cdata[i].lock);
		init_waitqueue_head(&zloop_cdata[i].q);
	}

	err = zio_register_driver(&zloop_zdrv);
	if (err)
		return err;
	zloop_hwdev = zio_allocate_device();
	if (IS_ERR(zloop_hwdev)) {
		err = PTR_ERR(zloop_hwdev);
		goto out_alloc;
	}
	zloop_hwdev->owner = THIS_MODULE;
	err = zio_register_device(zloop_hwdev, "zloop", 0);
	if (err)
		goto out_dev;
	err = zloop_register_miscdevs();
	if (err)
		goto out_misc;
	return 0;
out_misc:
	zio_unregister_device(zloop_hwdev);
out_dev:
	zio_free_device(zloop_hwdev);
out_alloc:
	zio_unregister_driver(&zloop_zdrv);
	return err;
}

static void __exit zloop_exit(void)
{
	zloop_unregister_miscdevs();
	zio_unregister_device(zloop_hwdev);
	zio_free_device(zloop_hwdev);
	zio_unregister_driver(&zloop_zdrv);
}

module_init(zloop_init);
module_exit(zloop_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Alessandro Rubini <rubini@.com>");
MODULE_DESCRIPTION("A zio driver which loops back to zio or a chardev");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
