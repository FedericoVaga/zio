/* Alessandro Rubini for CERN, 2013, GNU GPLv2 or later */

/*
 * This is a simple DTC (digital to time converter). It just sends out
 * a printk when the event fires. The event is just the timestamp in
 * the control block, and is managed by a high-resolution timer.
 */
#define DEBUG
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/ktime.h>
#include <linux/hrtimer.h>

#include <linux/zio.h>
#include <linux/zio-trigger.h>

/* One device, one cset, one timer (one channel too). So one lazy structure */
static struct {
	struct hrtimer timer;
	struct zio_cset *cset;
	struct timespec ts;
} zdtc;

#define ZDTC_SLACK 1000 /* 1us */

static enum hrtimer_restart zdtc_fn(struct hrtimer *timer)
{
	struct timespec ts;

	getnstimeofday(&ts);
	dev_dbg(&zdtc.cset->head.dev, "%s: %9li.%09li\n", __func__,
		ts.tv_sec, ts.tv_nsec);
	zio_trigger_data_done(zdtc.cset);
	return HRTIMER_NORESTART;
}

/*
 * stop_io: called when a trigger needs to be aborted and re-armed
 * The function is called in locked context. Here is it only used
 * for the data cset, so we can just return the partial block.
 */
static void zdtc_stop_io(struct zio_cset *cset)
{
	dev_dbg(&cset->head.dev, "%s\n", __func__);
	hrtimer_cancel(&zdtc.timer);
	zio_generic_data_done(cset);
}

/* raw_io method: arm the timer with the currently requested time */
static int zdtc_raw_io(struct zio_cset *cset)
{
	struct timespec ts;
	struct zio_control *ctrl;
	ktime_t ktime;

	/* We cannot be armed if there's no block. Wait for next push */
	if (!cset->chan->active_block)
		return -EIO;
	ctrl = zio_get_ctrl(cset->chan->active_block);
	zdtc.cset = cset;

	/* If sec is less than one hour, it is relative to the current sec. */
	getnstimeofday(&ts);
	ts.tv_nsec = ctrl->tstamp.ticks;
	if (ctrl->tstamp.secs > 3600)
		ts.tv_sec = ctrl->tstamp.secs;
	else
		ts.tv_sec += ctrl->tstamp.secs;
	ktime = timespec_to_ktime(ts);
	zdtc.ts = ts;
	hrtimer_start_range_ns(&zdtc.timer, ktime, ZDTC_SLACK,
			       HRTIMER_MODE_ABS);
	return -EAGAIN; /* Will data_done later */
}

static int zdtc_probe(struct zio_device *zdev)
{
	return 0;
}

static struct zio_cset zdtc_cset[] = {
	{
		ZIO_SET_OBJ_NAME("dtc"),
		.raw_io =	zdtc_raw_io,
		.stop_io =	zdtc_stop_io,
		.flags =	ZIO_DIR_OUTPUT | ZIO_CSET_TYPE_TIME |
					ZIO_CSET_SELF_TIMED,
		.n_chan =	1,
		.ssize =	0,
	},
};

static struct zio_device zdtc_tmpl = {
	.owner =		THIS_MODULE,
	.cset =			zdtc_cset,
	.n_cset =		ARRAY_SIZE(zdtc_cset),
};

/* The driver uses a table of templates */
static const struct zio_device_id zdtc_table[] = {
	{"zdtc", &zdtc_tmpl},
	{},
};

static struct zio_driver zdtc_zdrv = {
	.driver = {
		.name = "zdtc",
		.owner = THIS_MODULE,
	},
	.id_table = zdtc_table,
	.probe = zdtc_probe,
	/* All drivers compiled within the ZIO projects are compatibile
	   with the last version */
	.min_version = ZIO_VERSION(1, 1, 0),
};

/* Lazily, use a single global device */
static struct zio_device *zdtc_init_dev;

static int __init zdtc_init(void)
{
	int err;

	err = zio_register_driver(&zdtc_zdrv);
	if (err)
		return err;

	zdtc_init_dev = zio_allocate_device();
	if (IS_ERR(zdtc_init_dev)) {
		err = PTR_ERR(zdtc_init_dev);
		goto out_alloc;
	}
	zdtc_init_dev->owner = THIS_MODULE;
	err = zio_register_device(zdtc_init_dev, "zdtc", 0);
	if (err)
		goto out_register;

	hrtimer_init(&zdtc.timer, CLOCK_REALTIME, HRTIMER_MODE_ABS);
	zdtc.timer.function = zdtc_fn;
	return 0;

out_register:
	zio_free_device(zdtc_init_dev);
out_alloc:
	zio_unregister_driver(&zdtc_zdrv);
	return err;
}

static void __exit zdtc_exit(void)
{
	zio_unregister_device(zdtc_init_dev);
	zio_free_device(zdtc_init_dev);
	zio_unregister_driver(&zdtc_zdrv);
}

module_init(zdtc_init);
module_exit(zdtc_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
