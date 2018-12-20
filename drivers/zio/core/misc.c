/*
 * Copyright 2011 CERN
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * GNU GPLv2 or later
 */

/*
 * This file includes misc functionality, which is not actually ZIO-specific,
 * but it not available, to our knowledge, from the kernel proper
 */
#include <linux/types.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/zio.h>

/*
 * First-fit allocator. The cells are double-linked as usual, but
 * the head is outside of the linked list. So we can easily move
 * the head around and merge cells. list_for_each thus cannot be
 * used (it doesn't work on the first one).
 *
 * The cells are free (zero) or otherwise busy. Currently only one
 * busy type exists, but it's easily extended with alloc(size, type).
 * There's always at least one cell in the list.
 *
 * Since we merge the list after allocation, we only support free_s().
 * This can be changed, but in the typical use the caller knows the size.
 */

#undef CONFIG_TRACE_FFA

#ifdef CONFIG_TRACE_FFA
#define TRACE_FFA(ffa, name) ({pr_info(name "\n"); zio_ffa_dump(ffa); })
#else
#define TRACE_FFA(ffa, name)
#endif

struct ffa_cell {
	struct list_head list;
	unsigned long begin;
	unsigned long end; /* first invalid value */
	int status;
};

enum FFA_STATUS {
	FFA_FREE = 0,
	FFA_BUSY,
};

struct zio_ffa {
	spinlock_t lock;
	struct ffa_cell *cell;
};

/* The iterator starts at head, and stops when we fall at head again */
static inline struct ffa_cell *__ffa_next(struct ffa_cell *c,
					  struct zio_ffa *ffa)
{
	if (c->list.next == &ffa->cell->list)
		return NULL;
	return container_of(c->list.next, struct ffa_cell, list);
}

#define cell_for_each(c, ffa) \
	for (c = (ffa)->cell; c; c = __ffa_next(c, (ffa)))


/* The create and destroy must be called in non-atomic context and don't lock */
struct zio_ffa *zio_ffa_create(unsigned long begin, unsigned long end)
{
	struct zio_ffa *ffa;
	struct ffa_cell *c;

	ffa = kzalloc(sizeof(*ffa), GFP_KERNEL);
	c =  kzalloc(sizeof(*c), GFP_KERNEL);
	if (!ffa || !c) {
		kfree(ffa);
		kfree(c);
		return NULL;
	}
	spin_lock_init(&ffa->lock);
	INIT_LIST_HEAD(&c->list);
	c->begin = begin;
	c->end = end;
	c->status = FFA_FREE;
	ffa->cell = c;
	return ffa;
}
EXPORT_SYMBOL(zio_ffa_create);

void zio_ffa_destroy(struct zio_ffa *ffa)
{
	struct ffa_cell *c;
	struct list_head *l, *n;

	if (!ffa)
		return;

	c = ffa->cell;
	list_for_each_safe(l, n, &c->list) {
		list_del(l);
		kfree(container_of(l, struct ffa_cell, list));
	}
	kfree(c);
	kfree(ffa);
}
EXPORT_SYMBOL(zio_ffa_destroy);

/* dump doesn't lock, caller must be careful */
void zio_ffa_dump(struct zio_ffa *ffa)
{
	struct ffa_cell *c;

	pr_info("%s: ffa = %p\n", __func__, ffa);

	cell_for_each(c, ffa)
		pr_info("    0x%08lx-0x%08lx: %i (%li - %li)\n",
		       c->begin, c->end, c->status, c->begin, c->end);
}
EXPORT_SYMBOL(zio_ffa_dump);

/* merge the cell, whether free or busy space. Called in locked context */
static void ffa_merge(struct zio_ffa *ffa, struct ffa_cell *cell)
{
	struct ffa_cell *next, *prev;

	/* merge the next if not at wrap position */
	if (list_empty(&cell->list))
		return;
	next = container_of(cell->list.next, typeof(*next), list);
	if (cell->status == next->status && cell->end == next->begin) {
		list_del(&next->list);
		cell->end = next->end;
		if (ffa->cell == next)
			ffa->cell = cell;
		kfree(next);
	}

	/* merge the previous if not at wrap position */
	if (list_empty(&cell->list))
		return;
	prev = container_of(cell->list.prev, typeof(*prev), list);
	if (cell->status == prev->status && cell->begin == prev->end) {
		list_del(&cell->list);
		prev->end = cell->end;
		if (ffa->cell == cell)
			ffa->cell = prev;
		kfree(cell);
	}
}

/* alloc can be called from atomic context, thus gfp_t */
unsigned long zio_ffa_alloc(struct zio_ffa *ffa, size_t size, gfp_t gfp)
{
	struct ffa_cell *c, *new;
	unsigned long flags, ret;

	spin_lock_irqsave(&ffa->lock, flags);
	TRACE_FFA(ffa, "before alloc");

	cell_for_each(c, ffa) {
		if (c->status != FFA_FREE)
			continue;
		if (c->end - c->begin < size)
			continue;
		goto found;
	}
	spin_unlock_irqrestore(&ffa->lock, flags);
	return ZIO_FFA_NOSPACE;
found:
	ffa->cell = c;
	if (unlikely(c->end - c->begin == size)) {
		/* eactly right size */
		c->status = FFA_BUSY;
		ret = c->begin;
		spin_unlock_irqrestore(&ffa->lock, flags);
		return ret;
	}
	/* split the cell: "new" is the busy head, ffa still points to c */
	new = kzalloc(sizeof(*new), gfp);
	new->begin = c->begin;
	new->end = new->begin + size;
	c->begin = new->end;
	new->status = FFA_BUSY;
	list_add_tail(&new->list, &c->list);
	ret = new->begin;
	ffa_merge(ffa, new);
	TRACE_FFA(ffa, "after alloc");
	spin_unlock_irqrestore(&ffa->lock, flags);
	return ret;
}
EXPORT_SYMBOL(zio_ffa_alloc);

/* free can be called from atomic context, but I'd better not ask for gfp_t */
void zio_ffa_free_s(struct zio_ffa *ffa, unsigned long addr, size_t size)
{
	struct ffa_cell *c, *prev = NULL, *next = NULL;
	unsigned long flags, end = addr + size;

	spin_lock_irqsave(&ffa->lock, flags);

	TRACE_FFA(ffa, "before free");
	cell_for_each(c, ffa)
		if (c->begin <= addr && c->end >= addr + size)
			break;
	BUG_ON(!c);
	BUG_ON(c->status == FFA_FREE);
	if (c->begin != addr) {
		/* add a busy cell before us */
		prev = kzalloc(sizeof(*prev), GFP_ATOMIC);
		prev->begin = c->begin;
		prev->end = addr;
		prev->status = c->status;
		c->begin = addr;
		list_add_tail(&prev->list, &c->list);
	}
	if (c->end != addr + size) {
		/* add a busy cell after us */
		next = kzalloc(sizeof(*next), GFP_ATOMIC);
		next->begin = end;
		next->end = c->end;
		next->status = c->status;
		c->end = end;
		list_add(&next->list, &c->list);
	}
	c->status = FFA_FREE;
	if (prev)
		ffa_merge(ffa, prev);
	if (next)
		ffa_merge(ffa, next);
	if (!prev || !next)
		ffa_merge(ffa, c);

	TRACE_FFA(ffa, "after free");
	spin_unlock_irqrestore(&ffa->lock, flags);
}
EXPORT_SYMBOL(zio_ffa_free_s);


/* move the current pointer to the beginning */
void zio_ffa_reset(struct zio_ffa *ffa)
{
	struct ffa_cell *c;
	unsigned long flags;

	spin_lock_irqsave(&ffa->lock, flags);
	TRACE_FFA(ffa, "before reset");

	cell_for_each(c, ffa)
		if (c->begin < ffa->cell->begin)
			break;
	if (c)
		ffa->cell = c;

	TRACE_FFA(ffa, "after reset");
	spin_unlock_irqrestore(&ffa->lock, flags);
}
EXPORT_SYMBOL(zio_ffa_reset);
