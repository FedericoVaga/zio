/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a timer-based trigger for the ZIO framework. It is not
 * specific to a low-level device (every device can use it) and clearly
 * multi-instance.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/jiffies.h>
#include <linux/version.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

struct ztt_instance {
	struct zio_ti ti;
	struct timer_list timer;
	unsigned long next_run;
	unsigned long period; /* internal: jiffies */
	unsigned long phase; /* internal: jiffies */
};
#define to_ztt_instance(ti) container_of(ti, struct ztt_instance, ti)

enum ztt_attrs { /* names for the "addr" value of sw parameters */
	ZTT_ATTR_NSAMPLES = 0,
	ZTT_ATTR_PERIOD,
	ZTT_ATTR_PHASE,
};

static ZIO_ATTR_DEFINE_STD(ZIO_TRG, ztt_std_attr) = {
	ZIO_ATTR(trig, ZIO_ATTR_TRIG_POST_SAMP, ZIO_RW_PERM,
		 ZTT_ATTR_NSAMPLES, 16),
};

static struct zio_attribute ztt_ext_attr[] = {
	ZIO_ATTR_EXT("ms-period", ZIO_RW_PERM, ZTT_ATTR_PERIOD, 2000),
	ZIO_ATTR_EXT("ms-phase", ZIO_RW_PERM, ZTT_ATTR_PHASE, 0),
};

/* This recalculates next_run according to period and phase */
static void ztt_resync(struct ztt_instance *ztt)
{
	unsigned long next_run = ztt->next_run;
	unsigned long this_phase = next_run % ztt->period;

	if (this_phase == ztt->phase)
		return; /* current expire time ok, and in the future */
	next_run -= this_phase;
	next_run += ztt->phase;

	/* select the first expiration */
	next_run -= ztt->period;
	while (next_run <= jiffies)
		next_run += ztt->period;
	ztt->next_run = next_run;
	mod_timer(&ztt->timer, ztt->next_run);
}

static int ztt_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	struct zio_ti *ti = to_zio_ti(dev);
	struct ztt_instance *ztt;
	unsigned long jval;

	pr_debug("%s:%d\n", __func__, __LINE__);
	ztt = to_ztt_instance(ti);
	switch (zattr->id) {
	case ZTT_ATTR_PERIOD:
		/*
		 * Writing the period doesn't force a resync,
		 * in order to allow for a slowly-changing rate
		 */
		jval =  msecs_to_jiffies(usr_val);
		if ((signed)jval < 1)
			return -EINVAL;
		ztt->period = jval;
		break;
	case ZTT_ATTR_PHASE:
		/*
		 * Writing the phase forces a resync. This results
		 * in a glitch if you already changed the period.
		 * For finer control please use the hrt trigger
		 */
		jval =  msecs_to_jiffies(usr_val);
		if ((signed)jval < 0)
			return -EINVAL;
		ztt->phase = jval % ztt->period;
		ztt_resync(ztt);
		break;
	case ZTT_ATTR_NSAMPLES:
		/* Nothing to do */
		break;
	default:
		pr_err("%s: unknown \"addr\" 0x%lx for configuration\n",
				__func__, zattr->id);
		return -EINVAL;
	}
	return 0;
}

static struct zio_sysfs_operations ztt_s_ops = {
	.conf_set = ztt_conf_set,
};

/* This runs when the timer expires */
static void __ztt_fn(struct zio_ti *ti)
{
	struct ztt_instance *ztt;

	zio_arm_trigger(ti);

	ztt = to_ztt_instance(ti);
	if (!ztt->period)
		return; /* one-shot */

	ztt->next_run += ztt->period;
	mod_timer(&ztt->timer, ztt->next_run);
}

#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
static void ztt_fn(unsigned long arg)
{
	struct zio_ti *ti = (void *)arg;
	__ztt_fn(ti);
}
#else
static void ztt_fn(struct timer_list *t)
{
	struct ztt_instance *ztt = from_timer(ztt, t, timer);
	__ztt_fn(&ztt->ti);
}
#endif

/*
 * The trigger operations are the core of a trigger type
 */
static int ztt_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	/* FIXME: config is not supported yet */

	pr_debug("%s:%d\n", __func__, __LINE__);
	return 0;
}
static void ztt_start_timer(struct ztt_instance *ztt)
{
	ztt->next_run = jiffies + 1;
	ztt_resync(ztt);
	mod_timer(&ztt->timer, ztt->next_run);
}
static struct zio_ti *ztt_create(struct zio_trigger_type *trig,
				 struct zio_cset *cset,
				 struct zio_control *ctrl, fmode_t flags)
{
	struct ztt_instance *ztt;
	struct zio_ti *ti;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ztt = kzalloc(sizeof(struct ztt_instance), GFP_ATOMIC);
	if (!ztt)
		return ERR_PTR(-ENOMEM);
	ti = &ztt->ti;
	ti->flags = ZIO_DISABLED;
	ti->cset = cset;

	/* Fill own fields */
#if KERNEL_VERSION(4, 15, 0) > LINUX_VERSION_CODE
	setup_timer(&ztt->timer, ztt_fn,
		    (unsigned long)(&ztt->ti));
#else
	timer_setup(&ztt->timer, ztt_fn, 0);
#endif
	ztt->period = msecs_to_jiffies(ztt_ext_attr[0].value);
	ztt->phase = msecs_to_jiffies(ztt_ext_attr[1].value);
	ztt_start_timer(ztt);

	return ti;
}

static void ztt_destroy(struct zio_ti *ti)
{
	struct ztt_instance *ztt;

	pr_debug("%s:%d\n", __func__, __LINE__);
	ztt = to_ztt_instance(ti);
	del_timer_sync(&ztt->timer);
	kfree(ztt);
}

static void ztt_change_status(struct zio_ti *ti, unsigned int status)
{
	struct ztt_instance *ztt;

	pr_debug("%s:%d status=%d\n", __func__, __LINE__, status);
	ztt = to_ztt_instance(ti);

	if (!status) {	/* enable */
		ztt_start_timer(ztt);
	} else {	/* disable */
		del_timer(&ztt->timer);
	}
}

static const struct zio_trigger_operations ztt_trigger_ops = {
	.push_block = zio_generic_push_block,
	.pull_block = NULL,
	.config = ztt_config,
	.create = ztt_create,
	.destroy = ztt_destroy,
	.change_status = ztt_change_status,
};

static struct zio_trigger_type ztt_trigger = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = ztt_std_attr,
		.ext_zattr = ztt_ext_attr,
		.n_ext_attr = ARRAY_SIZE(ztt_ext_attr),
	},
	.s_op = &ztt_s_ops,
	.t_op = &ztt_trigger_ops,
};

/*
 * init and exit
 */
static int __init ztt_init(void)
{
	return zio_register_trig(&ztt_trigger, "timer");
}

static void __exit ztt_exit(void)
{
	zio_unregister_trig(&ztt_trigger);
}

module_init(ztt_init);
module_exit(ztt_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
