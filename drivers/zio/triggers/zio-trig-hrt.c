/* Alessandro Rubini for CERN, 2012, GNU GPLv2 or later */

/*
 * This is a high-resolution-timer trigger for the ZIO framework. It is not
 * specific to a low-level device (every device can use it) and it clearly is
 * multi-instance. The code is based on zio-trig-timer even if the
 * behaviour is different: the timer trigger is only periodic while this one
 * is basically one-shot, with periodic operation as an option for input.
 * For output, the time stamp can received in the control block.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#define ZTT_FLAGS_PENDING (1 << 0)

struct ztt_instance {
	struct zio_ti		ti;
	struct hrtimer		timer;
	struct timespec		ts;
	uint64_t		scalar;
	uint32_t		scalar_l; /* temporary storage */
	uint32_t		slack;
	uint32_t		period;
	unsigned long		flags;
};
#define to_ztt_instance(ti) container_of(ti, struct ztt_instance, ti)

enum ztt_attrs { /* names for the "addr" value of sw parameters */
	ZTT_ATTR_NSAMPLES = 0,
	ZTT_ATTR_SLACK_NS,
	ZTT_ATTR_PERIOD,	/* 0 to disable periodic mode */
	/* Further attributes are "expire time" */
	ZTT_ATTR_EXP_NSEC,
	ZTT_ATTR_EXP_SEC,	/* Writing "sec" activates the timer */
	ZTT_ATTR_EXP_SCALAR_L,
	ZTT_ATTR_EXP_SCALAR_H,	/* Writing "scalar-h" activates the timer */
};

static ZIO_ATTR_DEFINE_STD(ZIO_TRG, ztt_std_attr) = {
	ZIO_ATTR(trig, ZIO_ATTR_TRIG_POST_SAMP, ZIO_RW_PERM,
		  ZTT_ATTR_NSAMPLES, 16),
};

static struct zio_attribute ztt_ext_attr[] = {
	ZIO_ATTR_EXT("slack-ns", ZIO_RW_PERM,
		      ZTT_ATTR_SLACK_NS, 1000*1000 /* 1 ms */),
	ZIO_ATTR_EXT("period-ns", ZIO_RW_PERM,
		      ZTT_ATTR_PERIOD, 0 /* not periodic */),
	/* Setting sec/nsec is effective when writing the sec field */
	ZIO_ATTR_EXT("exp-nsec", ZIO_RW_PERM,
		      ZTT_ATTR_EXP_NSEC, 0 /* off */),
	ZIO_ATTR_EXT("exp-sec", ZIO_RW_PERM,
		      ZTT_ATTR_EXP_SEC, 0 /* off */),
	/* Setting the scalar value is effective when writing high half */
	ZIO_ATTR_EXT("exp-scalar-l", ZIO_RW_PERM,
		      ZTT_ATTR_EXP_SCALAR_L, 0 /* off */),
	ZIO_ATTR_EXT("exp-scalar-h", ZIO_RW_PERM,
		      ZTT_ATTR_EXP_SCALAR_H, 0 /* off */),
};

static int ztt_conf_set(struct device *dev, struct zio_attribute *zattr,
			uint32_t  usr_val)
{
	struct zio_ti *ti = to_zio_ti(dev);
	struct ztt_instance *ztt = to_ztt_instance(ti);
	struct timespec now;
	ktime_t ktime;

	pr_debug("%s:%d\n", __func__, __LINE__);
	switch (zattr->id) {
	case ZTT_ATTR_NSAMPLES:
		/* Nothing to do */
		break;
	case ZTT_ATTR_SLACK_NS:
		ztt->slack = usr_val;
		break;
	case ZTT_ATTR_PERIOD:
		ztt->period = usr_val;
		break;

	/*
	 * To set a timer using sec+nsec, write nsec then sec.
	 * If sec is less than one hour, it is relative to the current sec.
	 */
	case ZTT_ATTR_EXP_NSEC:
		ztt->ts.tv_nsec = usr_val;
		break;
	case ZTT_ATTR_EXP_SEC:
		if (usr_val < 10) {
			getnstimeofday(&now);
			usr_val += now.tv_sec;
		}
		ztt->ts.tv_sec = usr_val;
		ztt->scalar = ztt->ts.tv_sec * NSEC_PER_SEC + ztt->ts.tv_nsec;
		ktime = timespec_to_ktime(ztt->ts),
		ztt->flags |= ZTT_FLAGS_PENDING;
		hrtimer_start_range_ns(&ztt->timer, ktime, ztt->slack,
				       HRTIMER_MODE_ABS);
		break;

	/*
	 * To set a scalar timer, write low, then high. If high is 0,
	 * then it is relative to now.
	 */
	case ZTT_ATTR_EXP_SCALAR_L:
		ztt->scalar_l = usr_val;
		break;
	case ZTT_ATTR_EXP_SCALAR_H:
		ztt->scalar = (uint64_t)usr_val << 32 | ztt->scalar_l;
		if (!usr_val) {
			getnstimeofday(&now);
			ztt->scalar += ktime_to_ns(timespec_to_ktime(now));
		}
		ktime = ns_to_ktime(ztt->scalar);
		ztt->flags |= ZTT_FLAGS_PENDING;
		hrtimer_start_range_ns(&ztt->timer, ktime, ztt->slack,
				       HRTIMER_MODE_ABS);
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
static enum hrtimer_restart ztt_fn(struct hrtimer *timer)
{
	struct ztt_instance *ztt;
	struct zio_ti *ti;

	ztt = container_of(timer, struct ztt_instance, timer);
	ti = &ztt->ti;

	/* FIXME: fill the trigger attributes too */

	ztt = to_ztt_instance(ti);
	zio_arm_trigger(ti);

	if (ztt->period) {
		hrtimer_add_expires_ns(&ztt->timer, ztt->period);
		return HRTIMER_RESTART;
	}

	ztt->flags &= ~ZTT_FLAGS_PENDING;
	return HRTIMER_NORESTART;
}

/*
 * The trigger operations are the core of a trigger type
 */
static int ztt_push_block(struct zio_ti *ti, struct zio_channel *chan,
			  struct zio_block *block)
{
	struct ztt_instance *ztt = to_ztt_instance(ti);
	struct zio_control *ctrl;
	ktime_t ktime;
	int err;

	pr_debug("%s:%d\n", __func__, __LINE__);
	err = zio_generic_push_block(ti, chan, block);
	if (err)
		return err;

	/* If it is already pending, we are done */
	if (hrtimer_is_queued(&ztt->timer))
		return 0;

	/* If no timestamp provided in this control: we are done */
	ctrl = zio_get_ctrl(block);
	if (!ctrl->tstamp.secs && !ctrl->tstamp.ticks)
		return 0;

	/*
	 * Fire a new HR timer based on the stamp in this control block. For
	 * multi-channel cset, software is responsible for stamp consistency.
	 */
	if (ctrl->tstamp.secs) {
		struct timespec ts = {ctrl->tstamp.secs, ctrl->tstamp.ticks};
		ktime = timespec_to_ktime(ts);
	} else {
		ktime = ns_to_ktime(ctrl->tstamp.ticks);
	}
	hrtimer_start_range_ns(&ztt->timer, ktime, ztt->slack,
			       HRTIMER_MODE_ABS);
	return 0;
}

static int ztt_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	/* FIXME: config is not supported yet */

	pr_debug("%s:%d\n", __func__, __LINE__);
	return 0;
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
	hrtimer_init(&ztt->timer, CLOCK_REALTIME, HRTIMER_MODE_ABS);
	ztt->timer.function = ztt_fn;

	return ti;
}

static void ztt_destroy(struct zio_ti *ti)
{
	struct ztt_instance *ztt;

	pr_debug("%s:%d\n", __func__, __LINE__);
	ztt = to_ztt_instance(ti);
	hrtimer_cancel(&ztt->timer);
	kfree(ztt);
}

static void ztt_change_status(struct zio_ti *ti, unsigned int status)
{
	struct ztt_instance *ztt;

	pr_debug("%s:%d status=%d\n", __func__, __LINE__, status);
	ztt = to_ztt_instance(ti);

	if (!status) {
		/* enable: it may be already configured and pending. */
		if (ztt->flags & ZTT_FLAGS_PENDING)
			hrtimer_restart(&ztt->timer);
	} else {	/* disable */
		hrtimer_cancel(&ztt->timer);
	}
}
static const struct zio_trigger_operations ztt_trigger_ops = {
	.push_block = ztt_push_block,
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
	return zio_register_trig(&ztt_trigger, "hrt");
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
