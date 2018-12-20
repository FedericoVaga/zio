/* Alessandro Rubini for CERN, 2011, GNU GPLv2 or later */

/*
 * This is a trigger based on an external IRQ. You can specify the IRQ
 * number or the GPIO number -- then the associated IRQ is used
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/gpio.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include <linux/zio-buffer.h>
#include <linux/zio-trigger.h>

static int zti_irq = -1;
static int zti_gpio = -1;
module_param_named(irq, zti_irq, int, 0444);
module_param_named(gpio, zti_gpio, int, 0444);

enum zti_attrs {
	ZTI_ATTR_NSAMPLES = 0,
	ZTI_ATTR_IRQ,
	ZTI_ATTR_GPIO,
};

static ZIO_ATTR_DEFINE_STD(ZIO_TRG, zti_std_attr) = {
	ZIO_ATTR(trig, ZIO_ATTR_TRIG_POST_SAMP, ZIO_RW_PERM,
		 ZTI_ATTR_NSAMPLES, 16),
};

static struct zio_attribute zti_ext_attr[] = {
	ZIO_ATTR_EXT("irq", ZIO_RO_PERM, ZTI_ATTR_IRQ, -1),
	ZIO_ATTR_EXT("gpio", ZIO_RO_PERM, ZTI_ATTR_GPIO, -1),
};
static int zti_conf_set(struct device *dev, struct zio_attribute *zattr,
		uint32_t  usr_val)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	zattr->value = usr_val;

	return 0;
}

static struct zio_sysfs_operations zti_s_ops = {
	.conf_set = zti_conf_set,
};

static irqreturn_t zti_handler(int irq, void *dev_id)
{
	struct zio_ti *ti = dev_id;

	zio_arm_trigger(ti);
	return IRQ_HANDLED;
}

/*
 * The trigger operations are the core of a trigger type
 */

static int zti_config(struct zio_ti *ti, struct zio_control *ctrl)
{
	/* FIXME: config is not supported yet */

	pr_debug("%s:%d\n", __func__, __LINE__);
	return 0;
}

static struct zio_ti *zti_create(struct zio_trigger_type *trig,
				 struct zio_cset *cset,
				 struct zio_control *ctrl, fmode_t flags)
{
	struct zio_ti *ti;
	int edges[] = {
		IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING,
		IRQF_TRIGGER_FALLING,
		IRQF_TRIGGER_RISING,
		0
	};
	int i,  ret;

	pr_debug("%s:%d\n", __func__, __LINE__);

	ti = kzalloc(sizeof(*ti), GFP_ATOMIC);
	if (!ti)
		return ERR_PTR(-ENOMEM);
	ti->flags = ZIO_DISABLED;
	ti->cset = cset;

	/* Try all edge settings (gpio stuff prefers edges, but pci wants 0) */
	for (i = 0; i < ARRAY_SIZE(edges); i++) {
		ret = request_irq(zti_irq, zti_handler, IRQF_SHARED | edges[i],
			  KBUILD_MODNAME, ti);
		if (ret == -EBUSY)
			continue;
		break; /* success or other error */
	}
	if (ret < 0) {
		kfree(ti);
		return ERR_PTR(ret);
	}
	return ti;
}

static void zti_destroy(struct zio_ti *ti)
{
	pr_debug("%s:%d\n", __func__, __LINE__);
	free_irq(zti_irq, ti);
	kfree(ti);
}

static const struct zio_trigger_operations zti_trigger_ops = {
	.push_block = zio_generic_push_block,
	.pull_block = NULL,
	.config = zti_config,
	.create = zti_create,
	.destroy = zti_destroy,
};

static struct zio_trigger_type zti_trigger = {
	.owner = THIS_MODULE,
	.zattr_set = {
		.std_zattr = zti_std_attr,
		.ext_zattr = zti_ext_attr,
		.n_ext_attr = ARRAY_SIZE(zti_ext_attr),
	},
	.s_op = &zti_s_ops,
	.t_op = &zti_trigger_ops,
};

/*
 * A validation function, called at insmod and at parameter change
 */
static int zti_validate(int irq, int gpio)
{
	int ret = 0;

	if (irq != -1 && gpio != -1) {
		pr_err("%s: only set irq or gpio, not both\n", KBUILD_MODNAME);
		return -EINVAL;
	}
	if (irq == -1 && gpio == -1) {
		pr_err("%s: please set irq or gpio\n", KBUILD_MODNAME);
		return -EINVAL;
	}
	if (gpio != -1) {
		irq = gpio_to_irq(gpio);
		if (irq >= 0)
			ret  = gpio_request(gpio, KBUILD_MODNAME);
		else
			ret = irq;
	}
	if (ret < 0) {
		pr_err("%s: invalid irq/gpio (%i/%i)\n", KBUILD_MODNAME,
		       gpio, irq);
		return ret;
	}
	zti_irq = irq; /* used at trigger_create time */
	return 0;
}

/*
 * init and exit
 */
static int __init zti_init(void)
{
	int ret = zti_validate(zti_irq, zti_gpio);
	if (ret)
		return ret;
	return zio_register_trig(&zti_trigger, "irq");
}

static void __exit zti_exit(void)
{
	zio_unregister_trig(&zti_trigger);
	if (zti_gpio != -1)
		gpio_free(zti_gpio);
}

module_init(zti_init);
module_exit(zti_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Alessandro Rubini");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
