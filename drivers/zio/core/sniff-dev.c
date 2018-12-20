/*
 * Copyright 2011 CERN
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * GNU GPLv2 or later
 */
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/version.h>
#if KERNEL_VERSION(4, 11, 0) > LINUX_VERSION_CODE
#include <linux/sched.h>
#else
#include <linux/sched/signal.h>
#endif
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/miscdevice.h>
#include <linux/spinlock.h>

#include <linux/zio.h>
#include <linux/zio-user.h>

#define ZSD_MAX_LEN 1024 /* Half a meg at most. Static by now */

/* There is one such things for each reader. This hosts a list of controls */
struct zio_sniffdev_file {
	struct list_head list;
	spinlock_t lock;
	wait_queue_head_t q;
	struct list_head controls;
	int len;
	uint8_t zio_alarms; /* Either 0 or ZIO_ALARM_LOST_SNIFF */
};
static LIST_HEAD(zio_sniffdev_files);
static DEFINE_MUTEX(zio_sniffdev_mtx);

/* Each control in the list "controls" above is one of these */
struct zio_sniffdev_ctrl {
	struct list_head list;
	struct zio_control *ctrl;
	int size;
};

/* Add a new control to all files. Can be called in atomic context */
void zio_sniffdev_add(struct zio_control *ctrl)
{
	int size = zio_control_size(NULL); /* oops when ch->ctrlsize */
	struct zio_sniffdev_file *f;
	struct zio_sniffdev_ctrl *c;
	struct zio_control *newctrl;
	unsigned long flags;

	list_for_each_entry(f, &zio_sniffdev_files, list) {
		if (f->len >= ZSD_MAX_LEN) {
			f->zio_alarms |= ZIO_ALARM_LOST_SNIFF;
			continue;
		}
		newctrl = kmemdup(ctrl, size, GFP_ATOMIC);
		c = kmalloc(sizeof(*c), GFP_ATOMIC);
		if (!newctrl || !c) {
			f->zio_alarms |= ZIO_ALARM_LOST_SNIFF;
			kfree(c);
			kfree(newctrl);
			continue;
		}
		newctrl->zio_alarms |= f->zio_alarms;
		c->ctrl = newctrl;
		c->size = size;
		spin_lock_irqsave(&f->lock, flags);
		list_add_tail(&c->list, &f->controls);
		f->len++;
		spin_unlock_irqrestore(&f->lock, flags);
		wake_up_interruptible(&f->q);
	}
}

int zio_sniffdev_open (struct inode *ino, struct file *file)
{
	struct zio_sniffdev_file *f;

	f = kzalloc(sizeof(*f), GFP_USER);
	if (!f)
		return -ENOMEM;

	spin_lock_init(&f->lock);
	init_waitqueue_head(&f->q);
	INIT_LIST_HEAD(&f->controls);
	mutex_lock(&zio_sniffdev_mtx);
	list_add(&f->list, &zio_sniffdev_files);
	mutex_unlock(&zio_sniffdev_mtx);

	file->private_data = f;
	return 0;
}

int zio_sniffdev_release(struct inode *ino, struct file *file)
{
	struct zio_sniffdev_file *f = file->private_data;
	struct zio_sniffdev_ctrl *c;
	struct list_head *l, *n;

	mutex_lock(&zio_sniffdev_mtx);
	list_del(&f->list);
	mutex_unlock(&zio_sniffdev_mtx);

	list_for_each_safe(l, n, &f->controls) {
		c = list_entry(l, struct zio_sniffdev_ctrl, list);
		list_del(&c->list);
		kfree(c->ctrl);
		kfree(c);
	}
	return 0;
}

static inline int __zio_sniffdev_empty(struct zio_sniffdev_file *f )
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&f->lock, flags);
	ret = f->len == 0;
	spin_unlock_irqrestore(&f->lock, flags);
	return ret;
}

ssize_t zio_sniffdev_read(struct file *file, char __user *buf, size_t count,
			  loff_t *offp)
{
	struct zio_sniffdev_file *f = file->private_data;
	struct zio_sniffdev_ctrl *c = NULL;
	unsigned long flags;

again:
	while (__zio_sniffdev_empty(f)) {
		if (file->f_flags & O_NONBLOCK)
			return -EAGAIN;
		wait_event_interruptible(f->q, !__zio_sniffdev_empty(f));
		if (signal_pending(current))
			return -ERESTARTSYS;
	}
	/* Lock again: child and parent may contend */
	spin_lock_irqsave(&f->lock, flags);
	if (f->len) {
		f->len--;
		c = list_entry(f->controls.next, struct zio_sniffdev_ctrl,
			       list);
		list_del(&c->list);
	}
	spin_unlock_irqrestore(&f->lock, flags);
	if (!c)
		goto again;
	if (count < c->size)
		return -EINVAL;
	count = c->size;

	if (copy_to_user(buf, c->ctrl, count))
		return -EFAULT;
	*offp += count;
	kfree(c->ctrl);
	kfree(c);
	return count;
}

unsigned int zio_sniffdev_poll(struct file *file, struct poll_table_struct *w)
{
	struct zio_sniffdev_file *f = file->private_data;

	poll_wait(file, &f->q, w);
	if (__zio_sniffdev_empty(f))
		return 0;
	return POLLIN | POLLRDNORM;
}

struct file_operations zio_sniffdev_fops = {
	.owner=		THIS_MODULE,
	.open =		zio_sniffdev_open,
	.release =	zio_sniffdev_release,
	.read =		zio_sniffdev_read,
	.poll =		zio_sniffdev_poll,
	.llseek =	no_llseek,
};

struct miscdevice zio_sniffdev_misc = {
	.minor =	MISC_DYNAMIC_MINOR,
	.fops =		&zio_sniffdev_fops,
	.name =		"zio-sniff.ctrl",
};

int zio_sniffdev_init(void)
{
	/* This will fail when TLV is there, as the code above needs fixing */
	BUILD_BUG_ON(zio_control_size(NULL) != __ZIO_CONTROL_SIZE);

	return misc_register(&zio_sniffdev_misc);
}

void zio_sniffdev_exit(void)
{
	misc_deregister(&zio_sniffdev_misc);
}
