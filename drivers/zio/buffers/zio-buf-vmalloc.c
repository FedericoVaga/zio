/* Alessandro Rubini for CERN, 2012, GNU GPLv2 or later */

/*
 * This is a vmalloc-based buffer for the ZIO framework. It supports
 * mmap from user space and can be used as basis for dma-capable I/O.
 * The prefix of all local code/data is till "zbk_" so it's easier
 * for our users to use "diff" among the two implementations and see
 * what changes.
 */

#include <linux/version.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/list.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/vmalloc.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4,10,0))
#define	address_of(vmf)	((void*)(vmf)->address)
#else
#define	address_of(vmf)	((vmf)->virtual_address)
#endif

/*
 * We export a linear buffer to user space, for a single mmap call.
 * The circular buffer is managed by the ZIO first-fit allocator
 */
struct zbk_instance {
	struct zio_bi bi;
	struct list_head list; /* items, one per block */
	struct zio_ffa *ffa;
	void *data;
	atomic_t map_count;
	unsigned long size;
	unsigned long alloc_size; /* allocated size */
	unsigned long flags;
};
#define to_zbki(bi) container_of(bi, struct zbk_instance, bi)

#define ZBK_FLAG_MERGE_DATA	1

static struct kmem_cache *zbk_slab;


/* The list in the structure above collects a bunch of these */
struct zbk_item {
	struct zio_block block;
	struct list_head list;	/* item list */
	struct zbk_instance *instance;
	unsigned long begin;
	size_t len; /* block.datalen may change, so save this */
};
#define to_item(block) container_of(block, struct zbk_item, block);

enum {
	ZBK_ATTR_MERGE_DATA = ZIO_MAX_STD_ATTR,
};

static ZIO_ATTR_DEFINE_STD(ZIO_BUF, zbk_std_zattr) = {
	ZIO_ATTR(zbuf, ZIO_ATTR_ZBUF_MAXKB, ZIO_RW_PERM,
		 ZIO_ATTR_ZBUF_MAXKB /* ID for the switch below */, 128),
	ZIO_ATTR(zbuf, ZIO_ATTR_ZBUF_ALLOC_KB, ZIO_RO_PERM,
		 ZIO_ATTR_ZBUF_ALLOC_KB, 0),
};

static struct zio_attribute zbk_ext_attr[] = {
	ZIO_ATTR_EXT("merge-data", ZIO_RW_PERM,
		     ZBK_ATTR_MERGE_DATA, 0),
};

static int zbk_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	struct zio_bi *bi = to_zio_bi(dev);
	struct zio_ti *ti = NULL;
	struct zbk_instance *zbki = to_zbki(bi);
	struct zio_block *block;
	unsigned long flags, bflags, tflags;
	void *data;
	int ret = 0;

	switch (zattr->id) {
	case ZIO_ATTR_ZBUF_MAXKB:
		if (usr_val == zattr->value)
			return 0; /* nothing to do */
		/* Lock and disable */
		spin_lock_irqsave(&bi->lock, flags);
		if (atomic_read(&zbki->map_count)) {
			spin_unlock_irqrestore(&bi->lock, flags);
			return -EBUSY;
		}
		bflags = bi->flags;
		bi->flags |= ZIO_DISABLED;
		spin_unlock_irqrestore(&bi->lock, flags);

		/*
		 * Disable trigger while resizing buffer to avoid
		 * problems with blocks that point to a different
		 * vmalloc() area
		 */
		ti = bi->cset->ti;
		tflags = zio_trigger_abort_disable(ti->cset, 1);

		/* Flush the buffer */
		while ((block = bi->b_op->retr_block(bi)))
			bi->b_op->free_block(bi, block);

		/* Change size */
		data = vmalloc(usr_val * 1024);
		if (data) {
			vfree(zbki->data);
			zio_ffa_destroy(zbki->ffa);
			zbki->ffa = zio_ffa_create(0, usr_val * 1024);
			/* FIXME: what if this malloc failed? */
			zbki->size = usr_val * 1024;
			zbki->data = data;
		} else {
			ret = -ENOMEM;
		}

		/* Lock and restore flags */
		spin_lock_irqsave(&bi->lock, flags);
		bi->flags = bflags;
		spin_unlock_irqrestore(&bi->lock, flags);

		/* Restore trigger */
		if (ti && ((tflags & ZIO_STATUS) == ZIO_ENABLED))
			ti->flags = (ti->flags & ~ZIO_STATUS) | ZIO_ENABLED;
		if (ti && (tflags & ZIO_TI_ARMED))
			zio_arm_trigger(ti);

		return ret;

	case ZBK_ATTR_MERGE_DATA:
		if (usr_val)
			zbki->flags |= ZBK_FLAG_MERGE_DATA;
		else
			zbki->flags &= ~ZBK_FLAG_MERGE_DATA;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int zbk_info_get(struct device *dev, struct zio_attribute *zattr,
			 uint32_t *usr_val)
{
	struct zio_bi *bi = to_zio_bi(dev);
	struct zbk_instance *zbki = to_zbki(bi);

	switch (zattr->id) {
	case ZIO_ATTR_ZBUF_ALLOC_KB:
		*usr_val = zbki->alloc_size / 1024;
		break;
	case ZIO_ATTR_ZBUF_MAXKB:
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
	unsigned long offset, flags;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* alloc item and data. Control remains null at this point */
	item = kmem_cache_alloc(zbk_slab, gfp);
	offset = zio_ffa_alloc(zbki->ffa, datalen, gfp);
	ctrl = zio_alloc_control(gfp);
	if (!item || !ctrl || offset == ZIO_FFA_NOSPACE)
		goto out_free;
	memset(item, 0, sizeof(*item));
	item->begin = offset;
	item->len = datalen;
	item->block.data = zbki->data + offset;
	item->block.datalen = datalen;
	item->instance = zbki;

	spin_lock_irqsave(&bi->lock, flags);
	zbki->alloc_size += item->len;
	spin_unlock_irqrestore(&bi->lock, flags);
	/* mem_offset in current_ctrl is the last allocated */
	bi->chan->current_ctrl->mem_offset = offset;
	zio_set_ctrl(&item->block, ctrl);
	return &item->block;

out_free:
	if (offset != ZIO_FFA_NOSPACE) {
		zio_ffa_free_s(zbki->ffa, offset, datalen);
	} else {
		/* NOSPACE means that the buffer is 'full', there is
		 * no space for the requested datalen */
		spin_lock_irqsave(&bi->lock, flags);
	  	bi->flags |= ZIO_BI_NOSPACE;
		spin_unlock_irqrestore(&bi->lock, flags);
	}
	kmem_cache_free(zbk_slab, item);
	zio_free_control(ctrl);
	return NULL;
}

/* Free is called by f->read (for input) or by the trigger (for output) */
static void zbk_free_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_item *item;
	struct zbk_instance *zbki;
	struct zio_control *ctrl;
	unsigned long flags;

	pr_debug("%s:%d\n", __func__, __LINE__);
	ctrl = zio_get_ctrl(block);
	item = to_item(block);
	zbki = item->instance;

	if (bi->flags & ZIO_BI_PUSHING) {
		/* freed while pushing: we hold the bi lock already */
		zbki->alloc_size -= item->len;
		goto out_free;
	}

	spin_lock_irqsave(&bi->lock, flags);
	zbki->alloc_size -= item->len;
	bi->flags &= ~ZIO_BI_NOSPACE;
	spin_unlock_irqrestore(&bi->lock, flags);

out_free:
	zio_ffa_free_s(zbki->ffa, item->begin, item->len);
	zio_free_control(ctrl);
	kmem_cache_free(zbk_slab, item);
}

/* An helper for store_block() if we are trying to merge data runs */
static void zbk_try_merge(struct zbk_instance *zbki, struct zbk_item *item)
{
	struct zbk_item *prev;
	struct zio_control *ctrl, *prevc;

	/* Called while locked and already part of the list */
	prev = list_entry(item->list.prev, struct zbk_item, list);
	if (prev->begin + prev->len != item->begin)
		return; /* no, thanks */

	/* merge: remove from list, fix prev block, remove new control */
	list_del(&item->list);
	ctrl = zio_get_ctrl(&item->block);
	prevc = zio_get_ctrl(&prev->block);

	prev->len += item->len;				/* for the allocator */
	prev->block.datalen += item->block.datalen;	/* for copying */
	prevc->nsamples += ctrl->nsamples;		/* meta information */

	zio_free_control(ctrl);
	kmem_cache_free(zbk_slab, item);
}

/* Store is called by the trigger (for input) or by f->write (for output) */
static int zbk_store_block(struct zio_bi *bi, struct zio_block *block)
{
	struct zbk_instance *zbki = to_zbki(bi);
	struct zio_channel *chan = bi->chan;
	struct zbk_item *item;
	unsigned long flags;
	int awake = 0, pushed = 0, output, first;

	pr_debug("%s:%d (%p, %p)\n", __func__, __LINE__, bi, block);

	item = to_item(block);
	zio_get_ctrl(block)->mem_offset = item->begin;

	output = (bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT;

	/* add to the buffer instance or push to the trigger */
	spin_lock_irqsave(&bi->lock, flags);
	first = list_empty(&zbki->list);
	if (first) {
		if (unlikely(output))
			pushed = zio_trigger_try_push(bi, chan, block);
		else
			awake = 1;
	}
	if (!pushed)
		list_add_tail(&item->list, &zbki->list);

	if (!first && zbki->flags & ZBK_FLAG_MERGE_DATA)
		zbk_try_merge(zbki, item);
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
	int awake = 0;

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
	awake = 1;
	spin_unlock_irqrestore(&bi->lock, flags);

	if (awake && ((bi->flags & ZIO_DIR) == ZIO_DIR_OUTPUT))
		wake_up_interruptible(&bi->q);
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
	struct zio_ffa *ffa;
	void *data;
	size_t size;

	pr_debug("%s:%d\n", __func__, __LINE__);

	/* zero-sized blocks can't use this buffer type */
	if (chan->cset->ssize == 0)
		return ERR_PTR(-EINVAL);

	size = 1024 * zbuf->zattr_set.std_zattr[ZIO_ATTR_ZBUF_MAXKB].value;

	zbki = kzalloc(sizeof(*zbki), GFP_ATOMIC);
	ffa = zio_ffa_create(0, size);
	data = vmalloc(size);
	if (!zbki || !ffa || !data)
		goto out_nomem;
	zbki->size = size;
	zbki->ffa = ffa;
	zbki->data = data;
	INIT_LIST_HEAD(&zbki->list);

	/* all the fields of zio_bi are initialied by the caller */
	return &zbki->bi;
out_nomem:
	kfree(zbki);
	zio_ffa_destroy(ffa);
	if (data)
		vfree(data);
	return ERR_PTR(-ENOMEM);
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
	vfree(zbki->data);
	zio_ffa_destroy(zbki->ffa);
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

/*
 * To support mmap we implement the vm operations. We'll need
 * refcounting later, to safely change the buffer size (which we
 * refuse by now)
 */
static void zbk_open(struct vm_area_struct *vma)
{
	struct file *f = vma->vm_file;
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;
	struct zbk_instance *zbki = to_zbki(bi);

	atomic_inc(&zbki->map_count);
}

static void zbk_close(struct vm_area_struct *vma)
{
	struct file *f = vma->vm_file;
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;
	struct zbk_instance *zbki = to_zbki(bi);

	atomic_dec(&zbki->map_count);
}
static int __zbk_fault(struct vm_fault *vmf, struct file *f)
{
	struct zio_f_priv *priv = f->private_data;
	struct zio_bi *bi = priv->chan->bi;
	struct zbk_instance *zbki = to_zbki(bi);
	long off = vmf->pgoff * PAGE_SIZE;
	struct page *p;
	void *addr;

	if (priv->type == ZIO_CDEV_CTRL)
		return VM_FAULT_SIGBUS;

	pr_debug("%s: fault at %li (size %li)\n", __func__, off, zbki->size);
	if (off > zbki->size)
		return VM_FAULT_SIGBUS;

	addr = zbki->data + off;
	pr_debug("%s: uaddr %p, off %li: kaddr %p\n", __func__,
		 address_of(vmf), off, addr);
	p = vmalloc_to_page(addr);
	get_page(p);
	vmf->page = p;
	return 0;
}

#if KERNEL_VERSION(4, 11, 0) > LINUX_VERSION_CODE
static int zbk_fault(struct vm_area_struct *vma, struct vm_fault *vmf)
{
	return __zbk_fault(vmf, vma->vm_file);
}
#else
#if KERNEL_VERSION(4, 17, 0) > LINUX_VERSION_CODE
static int zbk_fault(struct vm_fault *vmf)
{
	return __zbk_fault(vmf, vmf->vma->vm_file);
}
#else
static vm_fault_t zbk_fault(struct vm_fault *vmf)
{
	return __zbk_fault(vmf, vmf->vma->vm_file);
}
#endif
#endif

static struct vm_operations_struct zbk_vma_ops = {
	.open = zbk_open,
	.close = zbk_close,
	.fault = zbk_fault,
};

static struct zio_buffer_type zbk_buffer = {
	.owner =	THIS_MODULE,
	.zattr_set = {
		.std_zattr = zbk_std_zattr,
		.ext_zattr = zbk_ext_attr,
		.n_ext_attr = ARRAY_SIZE(zbk_ext_attr),
	},
	.s_op = &zbk_sysfs_ops,
	.b_op = &zbk_buffer_ops,
	.v_op = &zbk_vma_ops,
	.f_op = &zio_generic_file_operations,
};

static int __init zbk_init(void)
{
	int ret;

	/* Can't use "zbk_item" as name and KMEM_CACHE_NAMED is not there */
	zbk_slab = kmem_cache_create("zio-vmalloc", sizeof(struct zbk_item),
				     __alignof__(struct zbk_item), 0, NULL);
	if (!zbk_slab)
		return -ENOMEM;
	ret = zio_register_buf(&zbk_buffer, "vmalloc");
	if (ret < 0)
		kmem_cache_destroy(zbk_slab);
	return ret;

}

static void __exit zbk_exit(void)
{
	zio_unregister_buf(&zbk_buffer);
	kmem_cache_destroy(zbk_slab);
}

module_init(zbk_init);
module_exit(zbk_exit);
MODULE_AUTHOR("Alessandro Rubini");
MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
