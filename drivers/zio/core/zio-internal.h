#ifndef ZIO_INTERNAL_H_
#define ZIO_INTERNAL_H_

#include <linux/version.h>

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,34)
#define ZIO_HAS_BINARY_CONTROL 1
#else
#define ZIO_HAS_BINARY_CONTROL 0
#endif

/* Defined in sysfs.c */
extern const struct attribute_group *def_zdev_groups_ptr[];
extern const struct attribute_group *def_cset_groups_ptr[];
extern const struct attribute_group *def_chan_groups_ptr[];
extern const struct attribute_group *def_ti_groups_ptr[];
extern const struct attribute_group *def_bi_groups_ptr[];
extern struct bin_attribute zio_bin_attr[];
/* Defined in object.c, used also in bus.c  */
extern struct device_type zdevhw_device_type;
extern struct device_type zdev_device_type;

/* This list is used in the core to keep track of registered objects */
struct zio_object_list {
	enum zio_object_type	zobj_type;
	struct list_head	list;
};
struct zio_object_list_item {
	struct list_head	list;
	char			name[ZIO_OBJ_NAME_LEN]; /* object name copy*/
	struct module		*owner;
	struct zio_obj_head	*obj_head;
};

/* Global framework status (i.e., globals in zio-core) */
struct zio_status {
	/* a pointer to set up standard ktype with create */
	struct kobject		*kobj;
	/* The minor numbers are allocated with the first-fit allocator. */
	struct zio_ffa		*minors;
	struct cdev		chrdev;
	dev_t			basedev;
	spinlock_t		lock;

	/* List of cset, used to retrieve a cset from a minor base*/
	struct list_head	list_cset;

	/* The three lists of registered devices, with owner module */
	struct zio_object_list	all_devices;
	struct zio_object_list	all_trigger_types;
	struct zio_object_list	all_buffer_types;
};

extern struct zio_status zio_global_status;

/* Defined in chardev.c */
extern int zio_minorbase_get(struct zio_cset *zcset);
extern void zio_minorbase_put(struct zio_cset *zcset);

extern int zio_register_cdev(void);
extern void zio_unregister_cdev(void);

extern int zio_create_chan_devices(struct zio_channel *zchan);
extern void zio_destroy_chan_devices(struct zio_channel *zchan);

extern int zio_init_buffer_fops(struct zio_buffer_type *zbuf);
extern int zio_fini_buffer_fops(struct zio_buffer_type *zbuf);

/* Exported but those that know to be the default */
extern int zio_default_buffer_init(void);
extern void zio_default_buffer_exit(void);
extern int zio_default_trigger_init(void);
extern void zio_default_trigger_exit(void);

/* Defined in sysfs.c */
extern void __ctrl_update_nsamples(struct zio_ti *ti);
extern void __zattr_trig_init_ctrl(struct zio_ti *ti, struct zio_control *ctrl);
extern int __check_dev_zattr(struct zio_attribute_set *parent,
		      struct zio_attribute_set *this);
extern int __zattr_dev_init_ctrl(struct zio_device *zdev);
extern int zio_create_attributes(struct zio_obj_head *head,
				 const struct zio_sysfs_operations *s_op,
				 struct zio_attribute_set *zattr_set_tmpl);
extern void zio_destroy_attributes(struct zio_obj_head *head);
extern int __zio_object_enable(struct zio_obj_head *head, unsigned int enable);
extern spinlock_t *__zio_get_dev_spinlock(struct zio_obj_head *head);
extern int __zio_conf_set(struct zio_obj_head *head,
			  struct zio_attribute *zattr, uint32_t val);
extern void __zio_attr_propagate_value(struct zio_obj_head *head,
				    struct zio_attribute *zattr);

/* Defined in objects.c */
extern int __zdev_register(struct zio_device *parent,
			   const struct zio_device_id *id);
extern void __zdev_unregister(struct zio_device *zdev);
extern struct zio_device *zio_device_find_child(struct zio_device *parent);
extern int zio_change_current_trigger(struct zio_cset *cset, char *name);
extern int zio_change_current_buffer(struct zio_cset *cset, char *name);

#endif /* ZIO_INTERNAL_H_ */
