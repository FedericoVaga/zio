/*
 * Copyright 2011 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * GNU GPLv2 or later
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/zio.h>
#include <linux/zio-buffer.h>
#include "zio-internal.h"

const uint32_t zio_version = ZIO_VERSION(__ZIO_MAJOR_VERSION,
					 __ZIO_MINOR_VERSION,
					 __ZIO_PATCH_VERSION);
struct zio_status zio_global_status;
static struct zio_status *zstat = &zio_global_status; /* Always use ptr */
/*
 * We use a local slab for control structures.
 */
static struct kmem_cache *zio_ctrl_slab;

struct zio_control *zio_alloc_control(gfp_t gfp)
{
	struct zio_control *ctrl;

	ctrl = kmem_cache_zalloc(zio_ctrl_slab, gfp);
	if (!ctrl)
		return NULL;

	ctrl->major_version = zio_version_major(zio_version);
	ctrl->minor_version = zio_version_minor(zio_version);
	if (ntohl(1) == 1)
		ctrl->flags |= ZIO_CONTROL_BIG_ENDIAN;
	else
		ctrl->flags |= ZIO_CONTROL_LITTLE_ENDIAN;
	return ctrl;
}
EXPORT_SYMBOL(zio_alloc_control);

/* At control release time, we can copy it to sniffers, if configured so */
void __weak zio_sniffdev_add(struct zio_control *ctrl)
{
}

void zio_free_control(struct zio_control *ctrl)
{
	zio_sniffdev_add(ctrl);
	kmem_cache_free(zio_ctrl_slab, ctrl);
}
EXPORT_SYMBOL(zio_free_control);

int __init zio_slab_init(void)
{
	zio_ctrl_slab = KMEM_CACHE(zio_control, 0);
	if (!zio_ctrl_slab)
		return -ENOMEM;
	return 0;
}

void zio_slab_exit(void) /* not __exit: called from zio_init on failures */
{
	if (zio_ctrl_slab)
		kmem_cache_destroy(zio_ctrl_slab);
	return;
}

struct zio_device *zio_find_device(char *name, uint32_t dev_id)
{
	struct zio_object_list_item *cur;
	struct zio_device *zdev;

	if (!name)
		return NULL;
	list_for_each_entry(cur, &zstat->all_devices.list, list) {
		zdev = to_zio_dev(&cur->obj_head->dev);
		if (strcmp(cur->name, name) == 0 && zdev->dev_id == dev_id)
			return zdev; /* found */
	}
	return NULL;
}
EXPORT_SYMBOL(zio_find_device);

/* if CONFIG_ZIO_SNIFF_DEV code in sniff-dev.c overrides the following two */
int __weak zio_sniffdev_init(void)
{
	return 0;
}
void __weak zio_sniffdev_exit(void)
{
	return;
}

/* Oerall init and exit */
static int __init zio_init(void)
{
	int err;

	/* The standard attributes must be less than ZIO_MAX_STD_ATTR */
	BUILD_BUG_ON(_ZIO_DEV_ATTR_STD_NUM > ZIO_MAX_STD_ATTR);
	BUILD_BUG_ON(_ZIO_TRG_ATTR_STD_NUM > ZIO_MAX_STD_ATTR);
	BUILD_BUG_ON(_ZIO_BUF_ATTR_STD_NUM > ZIO_MAX_STD_ATTR);

	/* The attribute 'version' must be the last attributes */
	BUILD_BUG_ON(_ZIO_DEV_ATTR_STD_NUM != ZIO_ATTR_VERSION + 1);
	BUILD_BUG_ON(_ZIO_TRG_ATTR_STD_NUM != ZIO_ATTR_VERSION + 1);
	BUILD_BUG_ON(_ZIO_BUF_ATTR_STD_NUM != ZIO_ATTR_VERSION + 1);

	/* Some compile-time checks, so developers are free to hack around */
	BUILD_BUG_ON(_ZIO_DEV_ATTR_STD_NUM != ARRAY_SIZE(zio_zdev_attr_names));
	BUILD_BUG_ON(_ZIO_BUF_ATTR_STD_NUM != ARRAY_SIZE(zio_zbuf_attr_names));
	BUILD_BUG_ON(_ZIO_TRG_ATTR_STD_NUM != ARRAY_SIZE(zio_trig_attr_names));
	BUILD_BUG_ON(ZIO_NR_MINORS > MINORMASK + 1);

	err = zio_slab_init();
	if (err)
		return err;
	/* Register ZIO bus */
	err = bus_register(&zio_bus_type);
	if (err)
		goto out;
	/* Initialize char device */
	err = zio_register_cdev();
	if (err)
		goto out_cdev;

	spin_lock_init(&zstat->lock);
	INIT_LIST_HEAD(&zstat->all_devices.list);
	zstat->all_devices.zobj_type = ZIO_DEV;
	INIT_LIST_HEAD(&zstat->all_trigger_types.list);
	zstat->all_trigger_types.zobj_type = ZIO_TRG;
	INIT_LIST_HEAD(&zstat->all_buffer_types.list);
	zstat->all_buffer_types.zobj_type = ZIO_BUF;

	err = zio_default_buffer_init();
	if (err)
		pr_warning("%s: cannot register default buffer\n", __func__);
	err = zio_default_trigger_init();
	if (err)
		pr_warning("%s: cannot register default trigger\n", __func__);
	if (zio_sniffdev_init())
		pr_warning("%s: cannot initialize /dev/zio-sniff.ctrl\n",
			   __func__);

	pr_info("zio-core had been loaded\n");
	return 0;

out_cdev:
	bus_unregister(&zio_bus_type);
out:
	zio_slab_exit();
	return err;
}

static void __exit zio_exit(void)
{
	zio_sniffdev_exit();
	zio_default_trigger_exit();
	zio_default_buffer_exit();

	/* Remove char device */
	zio_unregister_cdev();
	/* Remove ZIO bus */
	bus_unregister(&zio_bus_type);
	zio_slab_exit();
	pr_info("zio-core had been unloaded\n");
	return;
}

subsys_initcall(zio_init);
module_exit(zio_exit);

MODULE_VERSION(GIT_VERSION); /* Defined in local Makefile */
MODULE_AUTHOR("Federico Vaga and Alessandro Rubini");
/* Federico wrote the core, Alessandro wrote default trigger and buffer */
MODULE_DESCRIPTION("ZIO - ZIO Input Output");
MODULE_LICENSE("GPL");

ADDITIONAL_VERSIONS;
