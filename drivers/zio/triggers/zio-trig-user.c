/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a transparent trigger, driven by user requests. It is best
 * for streaming devices or one-shot transfers. It implements the pull
 * method to request data to hardware if none is queued and the push
 * method to pass data to the device.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

#define ZTU_DEFAULT_BLOCK_SIZE 16

static ZIO_ATTR_DEFINE_STD(ZIO_TRG, ztu_std_attr) = {
	ZIO_ATTR(trig, ZIO_ATTR_TRIG_POST_SAMP, ZIO_RW_PERM,
		 0 /* no addr needed */, ZTU_DEFAULT_BLOCK_SIZE),
};

int ztu_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	zattr->value = usr_val;
	return 0;
}

struct zio_sysfs_operations ztu_s_ops = {
	.conf_set = ztu_conf_set,
};

/*
 * zio_all_block_ready
 * It returns 1 if all channels have an active_block, otherwise it returns 0
 */
static inline unsigned int zio_all_block_ready(struct zio_cset *cset)
{
	struct zio_channel *chan;

	chan_for_each(chan, cset)
		if (!chan->active_block)
			return 0;
	return 1;
}

static int ztu_data_done(struct zio_cset *cset)
{
	int rearm;

	rearm = zio_generic_data_done(cset);

	/* if it is self timed, return immediately and force re-arming */
	if (rearm)
		return rearm;

	/* If it is input, do not force re-arm (it will be done by the user) */
	if ((cset->flags & ZIO_DIR) == ZIO_DIR_INPUT)
		return 0;

	/* If it is output and all blocks are ready, we must force re-arming */
	return zio_all_block_ready(cset);
}

/* The buffer pushes a block if it has none queued and one is written */
static int ztu_push_block(struct zio_ti *ti, struct zio_channel *chan,
			  struct zio_block *block)
{
	struct zio_cset *cset = chan->cset;
	int err;

	pr_debug("%s:%d\n", __func__, __LINE__);
	err = zio_generic_push_block(ti, chan, block);
	if (err)
		return err;

	/* If all enabled channels are ready, tell hardware we are ready */
	if (zio_all_block_ready(cset))
		zio_arm_trigger(ti);
	return 0;
}

/* The buffer pulls when a user reads and it has nothing yet */
static void ztu_pull_block(struct zio_ti *ti, struct zio_channel *chan)
{
	pr_debug("%s:%d\n", __func__, __LINE__);

	/* For self-timed devices, we have no pull, as it's already armed */
	if (zio_cset_early_arm(ti->cset))
		return;
	/* Otherwise, the user sets the input timing by reading */
	zio_arm_trigger(ti);
}

static int ztu_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	pr_debug("%s:%d\n", __func__, __LINE__);

	return 0;
}

static struct zio_ti *ztu_create(struct zio_trigger_type *trig,
				 struct zio_cset *cset,
				 struct zio_control *ctrl, fmode_t flags)
{
	struct zio_ti *ti;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ti = kzalloc(sizeof(*ti), GFP_ATOMIC);
	if (!ti)
		return ERR_PTR(-ENOMEM);
	ti->flags = ZIO_DISABLED;
	ti->cset = cset;

	return ti;
}

static void ztu_destroy(struct zio_ti *ti)
{
	kfree(ti);
}

static const struct zio_trigger_operations ztu_trigger_ops = {
	.data_done = ztu_data_done,
	.push_block = ztu_push_block,
	.pull_block = ztu_pull_block,
	.config = ztu_config,
	.create = ztu_create,
	.destroy = ztu_destroy,
};

static struct zio_trigger_type ztu_trigger = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = ztu_std_attr,
	},
	.s_op = &ztu_s_ops,
	.t_op = &ztu_trigger_ops,
};

/*
 * init and exit
 */
static int __init ztu_init(void)
{
	return zio_register_trig(&ztu_trigger, "user");
}

static void __exit ztu_exit(void)
{
	zio_unregister_trig(&ztu_trigger);
}


/* This is the default trigger, and is part of zio-core: no module init/exit */
int __init __attribute__((alias("ztu_init"))) zio_default_trigger_init(void);
void __exit __attribute__((alias("ztu_exit"))) zio_default_trigger_exit(void);
