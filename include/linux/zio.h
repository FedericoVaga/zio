
/* Federico Vaga for CERN, 2011, GNU GPLv2 or later */
#ifndef __ZIO_H__
#define __ZIO_H__

#include <linux/zio-user.h>

#ifdef __KERNEL__ /* Nothing more is for user space */

extern const uint32_t zio_version;

/*
 * ZIO_NAME_LEN is the full name length used in the head structures.
 * It is sometimes built at run time, for example buffer instances
 * have composite names. Also, all attributes names are this long.
 */
#define ZIO_NAME_LEN 32 /* full name */

#include <linux/sysfs.h>
#include <linux/device.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/string.h>
#include <linux/spinlock.h>

#include <linux/zio-sysfs.h>

#define ZIO_NR_MINORS  (1<<16) /* Ask for 64k minors: no harm done... */

/* Name the data structures */
struct zio_device; /* both type (a.k.a. driver) and instance (a.k.a. device) */
struct zio_channel; struct zio_cset;
struct zio_buffer_type; struct zio_bi; struct zio_block;
struct zio_trigger_type; struct zio_ti;

struct zio_device_operations;
struct zio_buffer_operations;
struct zio_trigger_operations;


/* zio_obj_head is for internal use only, as explained above */
struct zio_obj_head {
	struct device		dev;
	enum zio_object_type	zobj_type;
	char			name[ZIO_NAME_LEN];
};
#define to_zio_head(_dev) container_of(_dev, struct zio_obj_head, dev)
#define to_zio_dev(_dev) container_of(_dev, struct zio_device, head.dev)
#define to_zio_cset(_dev) container_of(_dev, struct zio_cset, head.dev)
#define to_zio_chan(_dev) container_of(_dev, struct zio_channel, head.dev)

/*
 * __get_from_zobj: is used to get a zio object element that can be (with the
 *                  same name) in different zio object.
 * _zhead: zio_obj_header pointer
 * member: which member return from the correspondent zio_object
 */
#define zio_get_from_obj(_head, member) ({				\
	typeof(to_zio_dev(&_head->dev)->member) (*el) = NULL;		\
	switch (_head->zobj_type) {					\
	case ZIO_DEV:							\
		el = &to_zio_dev(&_head->dev)->member;			\
		break;							\
	case ZIO_CSET:							\
		el = &to_zio_cset(&_head->dev)->member;			\
		break;							\
	case ZIO_CHAN:							\
		el = &to_zio_chan(&_head->dev)->member;			\
		break;							\
	case ZIO_BUF:							\
		el = &to_zio_buf(&_head->dev)->member;			\
		break;							\
	case ZIO_TRG:							\
		el = &to_zio_trig(&_head->dev)->member;			\
		break;							\
	case ZIO_TI:							\
		el = &to_zio_ti(&_head->dev)->member;			\
		break;							\
	case ZIO_BI:							\
		el = &to_zio_bi(&_head->dev)->member;			\
		break;							\
	default:							\
		WARN(1, "ZIO: unknown zio object %i\n", _head->zobj_type);\
	} el;								\
})

static inline enum zio_object_type zio_get_object_type(struct device *dev)
{
	return to_zio_head(dev)->zobj_type;
}

/* Bits 0..3 are reserved for use in all objects. By now only bit 1 is used */
enum zio_obj_flags {
	ZIO_STATUS		= 0x1,	/* 0 (default) is enabled */
	ZIO_ENABLED		= 0x0,
	ZIO_DISABLED		= 0x1,
	ZIO_DIR			= 0x2,	/* 0 is input  - 1 is output*/
	ZIO_DIR_INPUT		= 0x0,
	ZIO_DIR_OUTPUT		= 0x2,
};

/*
 * zio_device_id -- struct use to match driver with device
 */
struct zio_device_id {
	char			name[ZIO_OBJ_NAME_LEN];
	struct zio_device	*template;
};
/*
 * zio_driver -- the struct driver for zio
 */
struct zio_driver {
	const struct zio_device_id	*id_table;
	int (*probe)(struct zio_device *dev);
	int (*remove)(struct zio_device *dev);
	struct device_driver		driver;
	uint32_t min_version; /**< minimum version required to load it */
};
#define to_zio_drv(_drv) container_of(_drv, struct zio_driver, driver)
extern struct bus_type zio_bus_type;
int zio_register_driver(struct zio_driver *zdrv);
void zio_unregister_driver(struct zio_driver *zdrv);
/*
 * zio_device -- the top-level hardware description
 */
struct zio_device {
	struct zio_obj_head			head;
	uint32_t				dev_id; /* Driver-specific id */
	struct module				*owner;
	spinlock_t				lock; /* for all attr ops */
	unsigned long				flags;
	struct zio_attribute_set		zattr_set;
	const struct zio_sysfs_operations	*s_op;

	/* The full device is an array of csets */
	struct zio_cset			*cset;
	unsigned int			n_cset;

	/* We can state what its preferred buffer and trigger are (NULL ok) */
	char *preferred_buffer;
	char *preferred_trigger;
	void *priv_d;

	void			(*change_flags)(struct zio_obj_head *head,
						unsigned long mask);
};
struct zio_device *zio_allocate_device(void);
void zio_free_device(struct zio_device *dev);
int __must_check zio_register_device(struct zio_device *zdev, const char *name,
				    uint32_t dev_id);
void zio_unregister_device(struct zio_device *zdev);
struct zio_device *zio_find_device(char *name, uint32_t dev_id);

/*
 * zio_cset -- channel set: a group of channels with the same features
 */
struct zio_cset {
	struct zio_obj_head	head;
	struct zio_device	*zdev;		/* parent zio device */
	struct zio_buffer_type	*zbuf;		/* buffer type for bi */
	struct zio_trigger_type *trig;		/* trigger type for ti*/
	struct zio_ti		*ti;		/* trigger instance */
	int			(*raw_io)(struct zio_cset *cset);
	void			(*stop_io)(struct zio_cset *cset);
	void			(*change_flags)(struct zio_obj_head *head,
						unsigned long mask);
	spinlock_t		lock;		 /* for flags and triggers */

	unsigned		ssize;		/* sample size (bytes) */
	unsigned		index;		/* index within parent */
	unsigned long		flags;
	struct zio_attribute_set zattr_set;

	struct zio_channel	*chan_template;
	/* Interleaved channel template */
	struct zio_channel	*interleave;
	/* The cset is an array of channels */
	struct zio_channel	*chan;
	unsigned int		n_chan;

	void			*priv_d;	/* private for the device */

	struct list_head	list_cset;	/* for cset global list */
	int			minor, maxminor;
	char			*default_zbuf;
	char			*default_trig;

	struct zio_attribute	*cset_attrs;
};

/* first 4bit are reserved for zio object universal flags */
enum zio_cset_flags {
	ZIO_CSET_TYPE		=  0x70,	/* digital, analog, time, ... */
	ZIO_CSET_TYPE_DIGITAL	=  0x00,
	ZIO_CSET_TYPE_ANALOG	=  0x10,
	ZIO_CSET_TYPE_TIME	=  0x20,
	ZIO_CSET_TYPE_RAW	=  0x30,
	ZIO_CSET_CHAN_TEMPLATE	=  0x80, /* 1 if channels from template */
	ZIO_CSET_SELF_TIMED	= 0x100, /* for trigger use (see docs) */
	ZIO_CSET_CHAN_INTERLEAVE= 0x200, /* 1 if cset can interleave */
	ZIO_CSET_INTERLEAVE_ONLY= 0x400, /* 1 if interleave only */
	ZIO_CSET_HW_BUSY	= 0x800, /* set by driver, delays abort */
};

/* Check the flags so we know whether to arm immediately or not */
static inline int zio_cset_early_arm(struct zio_cset *cset)
{
	unsigned long flags = cset->flags;

	if ((flags & ZIO_DIR) == ZIO_DIR_OUTPUT)
		return 0;
	if (flags & ZIO_CSET_SELF_TIMED)
		return 1;
	return 0;
}

/*
 * zio_channel -- an individual channel within the cset
 */

struct zio_channel {
	struct zio_obj_head	head;
	struct zio_cset		*cset;		/* parent cset */
	struct zio_ti		*ti;		/* cset trigger instance */
	struct zio_bi		*bi;		/* buffer instance */
	unsigned int		index;		/* index within parent */
	unsigned long		flags;
	struct zio_attribute_set zattr_set;

	struct device		*ctrl_dev;	/* control char device */
	struct device		*data_dev;	/* data char device */

	void			*priv_d;	/* private for the device */
	void			*priv_t;	/* private for the trigger */

	struct zio_control	*current_ctrl;	/* the active one */
	struct zio_block	*user_block;	/* being transferred w/ user */
	struct mutex		user_lock;
	struct zio_block	*active_block;	/* being managed by hardware */

	void			(*change_flags)(struct zio_obj_head *head,
						unsigned long mask);
};

/* first 4bit are reserved for zio object universal flags */
enum zio_chan_flags {
	ZIO_CHAN_POLAR		= 0x10,	/* 0 is positive - 1 is negative*/
	ZIO_CHAN_POLAR_POSITIVE	= 0x00,
	ZIO_CHAN_POLAR_NEGATIVE	= 0x10,
};

/* get each channel from cset */
static inline struct zio_channel *zio_first_enabled_chan(struct zio_cset *cset,
						struct zio_channel *chan)
{
	if (unlikely(chan - cset->chan >= cset->n_chan))
		return NULL;
	while (1) {
		if (!(chan->flags & ZIO_DISABLED))
			return chan; /* if is enabled, use this */
		if (chan->index+1 == cset->n_chan)
			return NULL; /* no more channels */
		chan++;
	}
}
#define chan_for_each(cptr, cset)				\
		for (cptr = cset->chan;				\
		     (cptr = zio_first_enabled_chan(cset, cptr));	\
		     cptr++)

/* Use this in defining csets */
#define ZIO_SET_OBJ_NAME(_name) .head = {.name = _name}

/*
 * Return the number of enabled channel on a cset. Be careful: device
 * spinlock must be taken before invoke this function and it can be released
 * after the complete consumption of the information provided by this function
 */
static inline unsigned int zio_get_n_chan_enabled(struct zio_cset *cset) {
	struct zio_channel *chan;
	unsigned int n_chan = 0;

	chan_for_each(chan, cset)
		++n_chan;
	return n_chan;
}

/* We suggest all drivers have these options */
#define ZIO_PARAM_TRIGGER(_name) \
	char *_name; \
	module_param_named(trigger, _name, charp, 0444)
#define ZIO_PARAM_BUFFER(_name) \
	char *_name; \
	module_param_named(buffer, _name, charp, 0444)

/* The block is the basic data item being transferred */
struct zio_block {
	unsigned long		ctrl_flags;
	void			*data;
	size_t			datalen;
	size_t			uoff;
};


/**
 * Mark the cset as 'busy'
 * @param cset the cset to set busy
 * @param locked '1' if you want to protect the operatio, '0' if you will
 *               protect the operation by yourself
 */
static inline void zio_cset_busy_set(struct zio_cset *cset, int locked)
{
	unsigned long flags;

	if (locked)
		spin_lock_irqsave(&cset->lock, flags);
	cset->flags |= ZIO_CSET_HW_BUSY;
	if (locked)
		spin_unlock_irqrestore(&cset->lock, flags);
}

/**
 * Mark the cset as 'not busy'
 * @param cset the cset to set busy
 * @param locked '1' if you want to protect the operatio, '0' if you will
 *               protect the operation by yourself
 */
static inline void zio_cset_busy_clear(struct zio_cset *cset, int locked)
{
	unsigned long flags;

	if (locked)
		spin_lock_irqsave(&cset->lock, flags);
	cset->flags &= ~ZIO_CSET_HW_BUSY;
	if (locked)
		spin_unlock_irqrestore(&cset->lock, flags);
}

/**
 * Check if the cset is marked as 'busy'
 * @param cset the cset to check
 */
static inline int zio_cset_is_busy(struct zio_cset *cset)
{
	return !!(cset->flags & ZIO_CSET_HW_BUSY);
}

/*
 * We must know whether the ctrl block has been filled/read or not: "cdone"
 * No "set_ctrl" or "clr_cdone" are needed, as cdone starts 0 and is only set
 */
#define zio_get_ctrl(block) ((struct zio_control *)((block)->ctrl_flags & ~1))
#define zio_set_ctrl(block, ctrl) ((block)->ctrl_flags = (unsigned long)(ctrl))
#define zio_is_cdone(block)  ((block)->ctrl_flags & 1)
#define zio_set_cdone(block)  ((block)->ctrl_flags |= 1)

/*
 * It returns the size of the control associated to a channel.
 * When TLV is implemented, the value will be channel-dependent.
 */
static inline unsigned int zio_control_size(struct zio_channel *chan)
{
	return __ZIO_CONTROL_SIZE;
}

/* We have an optional misc device that returns all control blocks */
int zio_sniffdev_init(void);
void zio_sniffdev_exit(void);
void zio_sniffdev_add(struct zio_control *ctrl);

/*
 * Misc library-like code, from zio-misc.c
 */

/* first-fit allocator */
struct zio_ffa *zio_ffa_create(unsigned long begin, unsigned long end);
void zio_ffa_destroy(struct zio_ffa *ffa);
#define ZIO_FFA_NOSPACE ((unsigned long)-1) /* caller ensures -1 is invalid */
unsigned long zio_ffa_alloc(struct zio_ffa *ffa, size_t size, gfp_t gfp);
void zio_ffa_free_s(struct zio_ffa *ffa, unsigned long addr, size_t size);
void zio_ffa_dump(struct zio_ffa *ffa); /* diagnostics */
void zio_ffa_reset(struct zio_ffa *ffa);

#endif /* __KERNEL__ */
#endif /* __ZIO_H__ */
