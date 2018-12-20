/*
 * Copyright 2011 CERN
 * Author: Federico Vaga <federico.vaga@gmail.com>
 *
 * GNU GPLv2 or later
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/types.h>

#include <linux/zio.h>
#include <linux/zio-sysfs.h>
#include "zio-internal.h"

static struct zio_status *zstat = &zio_global_status; /* Always use ptr */

/**
 * It verifies the compatibility between the current ZIO version and the
 * given driver
 */
static int __zobj_version_check(struct zio_driver *drv)
{
	uint32_t min = drv->min_version;

	if (!min) {
		pr_warn("zio: %s does not have a driver version, cannot verify compatibility",
			drv->driver.name);
		return 0;
	}

	if (zio_version_major(min) != zio_version_major(zio_version))
		goto out;

	if (zio_version_minor(min) > zio_version_minor(zio_version))
		goto out;

	if (zio_version_patch(min) > zio_version_patch(zio_version))
		goto out;

	return 0;
out:
	pr_err("zio: %s require version %d.%d-%d but ZIO is %d.%d-%d\n",
	       drv->driver.name,
	       zio_version_major(min), zio_version_minor(min),
	       zio_version_patch(min), zio_version_major(zio_version),
	       zio_version_minor(zio_version), zio_version_patch(zio_version));
	return -1;
}


/*
 * zio_show_version
 * It shows the current version of ZIO
 */
static ssize_t zio_show_version(struct bus_type *bus, char *buf)
{
	return sprintf(buf, "%d.%d-%d\n", zio_version_major(zio_version),
		       zio_version_minor(zio_version),
		       zio_version_patch(zio_version));
}

/*
 * zio_show_buffers
 * It shows all buffers available
 */
static ssize_t zio_show_buffers(struct bus_type *bus, char *buf)
{
	struct zio_object_list_item *cur;
	ssize_t len = 0;

	spin_lock(&zstat->lock);
	list_for_each_entry(cur, &zstat->all_buffer_types.list, list)
		len = sprintf(buf, "%s%s\n", buf, cur->name);
	spin_unlock(&zstat->lock);

	return len;
}

/*
 * zio_show_triggers
 * It shows all triggers available
 */
static ssize_t zio_show_triggers(struct bus_type *bus, char *buf)
{
	struct zio_object_list_item *cur;
	ssize_t len = 0;

	spin_lock(&zstat->lock);
	list_for_each_entry(cur, &zstat->all_trigger_types.list, list)
		len = sprintf(buf, "%s%s\n", buf, cur->name);
	spin_unlock(&zstat->lock);

	return len;
}

enum zio_bus_attributs_enumeration {
	ZIO_DAN_BUS_VERSION,
	ZIO_DAN_BUS_TRIGGERS,
	ZIO_DAN_BUS_BUFFERS,
};
static struct bus_attribute def_bus_attrs[] = {
	[ZIO_DAN_BUS_VERSION] = __ATTR(version, ZIO_RO_PERM,
					zio_show_version, NULL),
	[ZIO_DAN_BUS_TRIGGERS] = __ATTR(available_buffers, ZIO_RO_PERM,
					zio_show_buffers, NULL),
	[ZIO_DAN_BUS_BUFFERS] = __ATTR(available_triggers, ZIO_RO_PERM,
					zio_show_triggers, NULL),
	__ATTR_NULL,
};

#if LINUX_VERSION_CODE > KERNEL_VERSION(3,11,0)
static struct attribute *zio_bus_def_attrs[] = {
	&def_bus_attrs[ZIO_DAN_BUS_VERSION].attr,
	&def_bus_attrs[ZIO_DAN_BUS_TRIGGERS].attr,
	&def_bus_attrs[ZIO_DAN_BUS_BUFFERS].attr,
	NULL,
};

ATTRIBUTE_GROUPS(zio_bus_def);

#endif

/*
 * zio_match_id
 * @id list of zio_device_if supported by a driver
 * @head the ZIO header to compare with the list
 *
 * It matches a list of zio_device_if with a name. It returns the matched
 * zio_device_id otherwise a NULL pointer.
 */
static const struct zio_device_id *zio_match_id(const struct zio_device_id *id,
						const struct zio_obj_head *head)
{
	while (id->name[0]) {
		dev_dbg(&head->dev, "%s comparing  %s == %s\n", __func__,
			 id->name, head->name);
		if (!strcmp(head->name, id->name))
			return id;
		++id;
	}
	dev_dbg(&head->dev, "%s fail\n", __func__);
	return NULL;
}

/*
 * zio_get_device_id
 * @zdev ZIO device
 *
 * It returns a zio_device_id which match with zdev
 */
const struct zio_device_id *zio_get_device_id(const struct zio_device *zdev)
{
	const struct zio_driver *zdrv = to_zio_drv(zdev->head.dev.driver);

	return zio_match_id(zdrv->id_table, &zdev->head);
}
EXPORT_SYMBOL(zio_get_device_id);


/*
 * zio_match_device
 * @drv driver to match
 * @dev device to match
 *
 * This function is the core of the ZIO bus system. It matches drivers with
 * devices. To allow the definition of custom sysfs attributes of different
 * devices, ZIO use a mechanism of double registration. At driver load time,
 * the driver registers a fake zio_device, then (if it match with a driver)
 * this match function will register the real ZIO device which always match.
 */
static int zio_match_device(struct device *dev, struct device_driver *drv)
{
	const struct zio_driver *zdrv = to_zio_drv(drv);
	const struct zio_device_id *id;
	struct zio_device *child;
	int err = 0;

	if (!zdrv->id_table)
		return 0;
	id = zio_match_id(zdrv->id_table, to_zio_head(dev));
	if (!id)
		return 0;

	/* device and driver match */
	if (dev->type == &zdevhw_device_type) {
		/* Register the real zio device */
		err = __zdev_register(to_zio_dev(dev), id);
		if (err) {
			pr_err("ZIO: Cannot register real zio_device (%d)\n",
				err);
			return 0;
		}
		child = zio_device_find_child(to_zio_dev(dev));
		/*
		 * Probe the device because the real device always match. We
		 * cannot do probe when the zio_device is registered because
		 * we need also its sub-devices for a safe probe. More over it
		 * allow us to easily remove the device on probe failure.
		 */
		if (zdrv->probe)
			err = zdrv->probe(child);
		if (err) {
			dev_err(&child->head.dev, "probe() fail with error %d",
				err);
			__zdev_unregister(child);
		} else {
			dev_info(&child->head.dev, "device loaded\n");
		}
		/* We done everything with child */
		put_device(&child->head.dev);
		return 0;
	} else if (dev->type == &zdev_device_type) {
		return 1; /* real device always match*/
	}
	return 0;
}

struct bus_type zio_bus_type = {
	.name = "zio",
#if LINUX_VERSION_CODE > KERNEL_VERSION(3,11,0)
	.bus_groups = zio_bus_def_groups,
#else
	.bus_attrs = def_bus_attrs,
#endif
	.match = zio_match_device,
};


/*
 * zio_drv_remove
 * @dev device to remove
 *
 * It invokes the ZIO driver remove function
 */
static int zio_drv_remove(struct device *dev)
{
	struct zio_driver *zdrv = to_zio_drv(dev->driver);
	struct zio_device *zdev = to_zio_dev(dev);

	if (zdrv->remove)
		return zdrv->remove(zdev);
	return 0;
}

/*
 * _zdev_template_check_and_init
 *
 * zio_register_driver() invokes this function to perform a preliminary test
 * and initialization on templates registered within the driver.
 *
 * NOTE: this not perform a complete test and initialization, during
 * driver->probe ZIO can rise new error and performs other initialization stuff
 *
 * FIXME try to move all the validations here
 */
static int _zdev_template_check_and_init(struct zio_device *zdev,
					 const char *name)
{
	struct zio_cset *cset;
	int i;

	if (!zdev) {
		pr_err("%s:%d missing zio_device template\n",
				__func__, __LINE__);
		return -EINVAL;
	}
	if (!zdev->owner) {
		/* FIXME I can use driver->owner */
		dev_err(&zdev->head.dev, "device template %s has no owner\n",
			name);
		return -EINVAL;
	}
	if (!zdev->cset || !zdev->n_cset) {
		dev_err(&zdev->head.dev, "no cset in device template %s",
			name);
		return -EINVAL;
	}

	for (i = 0; i < zdev->n_cset; ++i) {
		cset = &zdev->cset[i];
		if (!cset->n_chan) {
			dev_err(&zdev->head.dev,
				"no channels in %s cset%i\n",
				name, cset->index);
			return -EINVAL;
		}
		/*
		* Only time-type can have a zero-sized sample. This
		* protects against users who forgot to fill fields.
		*/
		if (!cset->ssize &&
		    (cset->flags & ZIO_CSET_TYPE) != ZIO_CSET_TYPE_TIME) {
			dev_err(&zdev->head.dev,
				"ssize can not be 0 in %s cset%i\n",
				name, cset->index);
			return -EINVAL;
		}
	}
	return 0;
}

int zio_register_driver(struct zio_driver *zdrv)
{
	int i, err;

	/* Check version compatibility */
	err = __zobj_version_check(zdrv);
	if (err) {
		pr_err("ZIO: Cannot register driver %s: incompatible version\n",
			zdrv->driver.name);
		return -EINVAL;
	}

	if (!zdrv->id_table) {
		pr_err("ZIO: id_table is mandatory for a zio driver\n");
		return -EINVAL;
	}
	for (i = 0; zdrv->id_table[i].name[0]; ++i) {
		err = _zdev_template_check_and_init(zdrv->id_table[i].template,
						    zdrv->id_table[i].name);
		if (err)
			return err;
	}

	zdrv->driver.bus = &zio_bus_type;
	zdrv->driver.remove = zio_drv_remove;

	return driver_register(&zdrv->driver);
}
EXPORT_SYMBOL(zio_register_driver);
void zio_unregister_driver(struct zio_driver *zdrv)
{
	driver_unregister(&zdrv->driver);
}
EXPORT_SYMBOL(zio_unregister_driver);
