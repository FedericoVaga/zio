/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a kmalloc-based buffer for the ZIO framework. It is used both
 * as a default when no buffer is selected by applications and as an
 * example about our our structures and methods are used.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>
#include <linux/zio-sysfs.h>

/* This is an instance of a buffer, associated to two cdevs */
struct zbk_instance {
	struct zio_bi bi;
	int nitem;		/* allocated item */
	struct list_head list; /* items list and lock */
};
#define to_zbki(bi) container_of(bi, struct zbk_instance, bi)

static struct kmem_cache *zbk_slab;


/* The list in the structure above collects a bunch of these */
struct zbk_item {
	struct zio_block block;
	struct list_head list;	/* item list */
	struct zbk_instance *instance;
	size_t len; /* block.datalen may change, so save this */
};
#define to_item(block) container_of(block, struct zbk_item, block)

static ZIO_ATTR_DEFINE_STD(ZIO_BUF, zbk_std_zattr) = {
	ZIO_ATTR(zbuf, ZIO_ATTR_ZBUF_MAXLEN, ZIO_RW_PERM,
		 ZIO_ATTR_ZBUF_MAXLEN, 16),
	ZIO_ATTR(zbuf, ZIO_ATTR_ZBUF_ALLOC_LEN, ZIO_RO_PERM,
		 ZIO_ATTR_ZBUF_ALLOC_LEN, 0),
};

static int zbk_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	struct zio_bi *bi = to_zio_bi(dev);

	/* If somebody is sleeping for write and we increase the size... */
	wake_up_interruptible(&bi->q);

	return 0;
}
static int zbk_info_get(struct device *dev, struct zio_attribute *zattr,
			 uint32_t *usr_val)
{
	struct zio_bi *bi = to_zio_bi(dev);
	struct zbk_instance *zbki = to_zbki(bi);

	switch (zattr->id) {
	case ZIO_ATTR_ZBUF_ALLOC_LEN:
		*usr_val = zbki->nitem;
		break;
	case ZIO_ATTR_ZBUF_MAXLEN:
	default:
		break;
	}

	return 0;
}
struct zio_sysfs_operations zbk_sysfs_ops = {
	.conf_set = zbk_conf_set,
	.info_get = zbk_info_get,
};


/* Alloc is called by the trigger (for input) or by f->write (for output) */
static struct zio_block *zbk_alloc_block(struct zio_bi *bi,
					 size_t datalen, gfp_t gfp)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zbk_item *item;
	struct zio_control *ctrl;
	unsigned long flags;
	void *data;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* alloc fails if we overflow the buffer size */
	spin_lock_irqsave(&bi->lock, flags);
	if (zbki->nitem >= zio_bi_std_val(bi, ZIO_ATTR_ZBUF_MAXLEN)) {
		bi->flags |= ZIO_BI_NOSPACE;
		goto out_unlock;
	}
	zbki->nitem++;
	spin_unlock_irqrestore(&bi->lock, flags);

	/* alloc item and data. Control remains null at this point */
	item = kmem_cache_alloc(zbk_slab, gfp);
	data = kmalloc(datalen, gfp);
	ctrl = zio_alloc_control(gfp);
	if (!item || !data || !ctrl)
		goto out_free;
	memset(item, 0, sizeof(*item));
	item->block.data = data;
	item->block.datalen = datalen;
	item->len = datalen;
	item->instance = zbki;
	zio_set_ctrl(&item->block, ctrl);
	return &item->block;

out_free:
	kfree(data);
	kmem_cache_free(zbk_slab, item);
	zio_free_control(ctrl);
	spin_lock_irqsave(&bi->lock, flags);
	zbki->nitem--;
out_unlock:
	spin_unlock_irqrestore(&bi->lock, flags);
	return NULL;
}

/* Free is called by f->read (for input) or by the trigger (for output) */
static void zbk_free_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_item *item;
	struct zbk_instance *zbki;
	unsigned long flags;
	int awake = 0;

	pr_debug("%s:%d\n", __func__, __LINE__);

	item = to_item(block);
	zbki = item->instance;

	if (bi->flags & ZIO_BI_PUSHING) {
		/* freed while pushing: we hold the bi lock already */
		zbki->nitem--;
		goto out_free;
	}

	spin_lock_irqsave(&bi->lock, flags);

	if (((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT) &&
	     zbki->nitem < zio_bi_std_val(bi, ZIO_ATTR_ZBUF_MAXLEN))
		awake = 1;
	bi->flags &= ~ZIO_BI_NOSPACE;
	zbki->nitem--;
	spin_unlock_irqrestore(&bi->lock, flags);

out_free:
	kfree(block->data);
	zio_free_control(zio_get_ctrl(block));
	kmem_cache_free(zbk_slab, item);
	if (awake)
		wake_up_interruptible(&bi->q);
}

/* Store is called by the trigger (for input) or by f->write (for output) */
static int zbk_store_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zio_channel *chan = bi->chan;
	struct zbk_item *item = to_item(block);
	unsigned long flags;
	int awake = 0, pushed = 0, isempty;
	int output = (bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT;

	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, block);

	/* add to the buffer instance or push to the trigger */
	spin_lock_irqsave(&bi->lock, flags);
	isempty = list_empty(&zbki->list);
	if (isempty) {
		if (unlikely(output))
			pushed = zio_trigger_try_push(bi, chan, block);
		else
			awake = 1;
	}
	if (!pushed)
		list_add_tail(&item->list, &zbki->list);

	spin_unlock_irqrestore(&bi->lock, flags);

	/* if first input, awake user space */
	if (awake)
		wake_up_interruptible(&bi->q);
	return 0;
}

/* Retr is called by f->read (for input) or by the trigger (for output) */
static struct zio_block *zbk_retr_block(struct zio_bi *bi)
{
	struct zbk_item *item;
	struct zbk_instance *zbki;
	struct zio_ti *ti;
	struct list_head *first;
	unsigned long flags;

	zbki = to_zbki(bi);

	/* PUSHING is only active temporarily during locked context */
	if (bi->flags & ZIO_BI_PUSHING)
		return NULL;

	/* There is no trig->push in our call trace, proceed to get the lock */
	spin_lock_irqsave(&bi->lock, flags);
	if (list_empty(&zbki->list))
		goto out_unlock;
	first = zbki->list.next;
	item = list_entry(first, struct zbk_item, list);
	list_del(&item->list);
	spin_unlock_irqrestore(&bi->lock, flags);

	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, item);
	return &item->block;

out_unlock:
	spin_unlock_irqrestore(&bi->lock, flags);
	/* There is no data in buffer, and we may pull to have data soon */
	ti = bi->cset->ti;
	if ((bi->flags & ZIO_DIR) == ZIO_DIR_INPUT && ti->t_op->pull_block) {
		/* chek if trigger is disabled */
		if (unlikely((ti->flags & ZIO_STATUS) == ZIO_DISABLED))
			return NULL;
		ti->t_op->pull_block(ti, bi->chan);
	}
	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, NULL);
	return NULL;
}

/* Create is called by zio for each channel electing to use this buffer type */
static struct zio_bi *zbk_create(struct zio_buffer_type *zbuf,
				 struct zio_channel *chan)
{
	struct zbk_instance *zbki;

	pr_debug("%s:%d\n", __func__, __LINE__);

	zbki = kzalloc(sizeof(*zbki), GFP_ATOMIC);
	if (!zbki)
		return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&zbki->list);

	/* all the fields of zio_bi are initialied by the caller */
	return &zbki->bi;
}

/* destroy is called by zio on channel removal or if it changes buffer type */
static void zbk_destroy(struct zio_bi *bi)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zbk_item *item;
	struct list_head *pos, *tmp;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* no need to lock here, zio ensures we are not active */
	list_for_each_safe(pos, tmp, &zbki->list) {
		item = list_entry(pos, struct zbk_item, list);
		zbk_free_block(&zbki->bi, &item->block);
	}
	kfree(zbki);
}

static const struct zio_buffer_operations zbk_buffer_ops = {
	.alloc_block =	zbk_alloc_block,
	.free_block =	zbk_free_block,
	.store_block =	zbk_store_block,
	.retr_block =	zbk_retr_block,
	.create =	zbk_create,
	.destroy =	zbk_destroy,
};

static struct zio_buffer_type zbk_buffer = {
	.owner =	THIS_MODULE,
	.zattr_set = {
		.std_zattr = zbk_std_zattr,
	},
	.s_op = &zbk_sysfs_ops,
	.b_op = &zbk_buffer_ops,
	.f_op = &zio_generic_file_operations,
};

static int __init zbk_init(void)
{
	int ret;

	/* Can't use "zbk_item" as name and KMEM_CACHE_NAMED is not there */
	zbk_slab = kmem_cache_create("zio-kmalloc", sizeof(struct zbk_item),
				     __alignof__(struct zbk_item), 0, NULL);
	if (!zbk_slab)
		return -ENOMEM;
	ret = zio_register_buf(&zbk_buffer, "kmalloc");
	if (ret < 0)
		kmem_cache_destroy(zbk_slab);
	return ret;

}

static void __exit zbk_exit(void)
{
	zio_unregister_buf(&zbk_buffer);
	kmem_cache_destroy(zbk_slab);
}

/* This is the default buffer, and is part of zio-core: no module init/exit */
int __init __attribute__((alias("zbk_init"))) zio_default_buffer_init(void);
void __exit __attribute__((alias("zbk_exit"))) zio_default_buffer_exit(void);
