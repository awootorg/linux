// SPDX-License-Identifier: GPL-2.0-only
/*
 * Add configfs and memory store: Kyungchan Koh <kkc6196@fb.com> and
 * Shaohua Li <shli@fb.com>
 */
#include <linux/module.h>

#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/init.h>
#include "null_blk.h"

#undef pr_fmt
#define pr_fmt(fmt)	"null_blk: " fmt

#define FREE_BATCH		16

#define TICKS_PER_SEC		50ULL
#define TIMER_INTERVAL		(NSEC_PER_SEC / TICKS_PER_SEC)

#ifdef CONFIG_BLK_DEV_NULL_BLK_FAULT_INJECTION
static DECLARE_FAULT_ATTR(null_timeout_attr);
static DECLARE_FAULT_ATTR(null_requeue_attr);
static DECLARE_FAULT_ATTR(null_init_hctx_attr);
#endif

static inline u64 mb_per_tick(int mbps)
{
	return (1 << 20) / TICKS_PER_SEC * ((u64) mbps);
}

/*
 * Status flags for nullb_device.
 *
 * CONFIGURED:	Device has been configured and turned on. Cannot reconfigure.
 * UP:		Device is currently on and visible in userspace.
 * THROTTLED:	Device is being throttled.
 * CACHE:	Device is using a write-back cache.
 */
enum nullb_device_flags {
	NULLB_DEV_FL_CONFIGURED	= 0,
	NULLB_DEV_FL_UP		= 1,
	NULLB_DEV_FL_THROTTLED	= 2,
	NULLB_DEV_FL_CACHE	= 3,
};

#define MAP_SZ		((PAGE_SIZE >> SECTOR_SHIFT) + 2)
/*
 * nullb_page is a page in memory for nullb devices.
 *
 * @page:	The page holding the data.
 * @bitmap:	The bitmap represents which sector in the page has data.
 *		Each bit represents one block size. For example, sector 8
 *		will use the 7th bit
 * The highest 2 bits of bitmap are for special purpose. LOCK means the cache
 * page is being flushing to storage. FREE means the cache page is freed and
 * should be skipped from flushing to storage. Please see
 * null_make_cache_space
 */
struct nullb_page {
	struct page *page;
	DECLARE_BITMAP(bitmap, MAP_SZ);
};
#define NULLB_PAGE_LOCK (MAP_SZ - 1)
#define NULLB_PAGE_FREE (MAP_SZ - 2)

static LIST_HEAD(nullb_list);
static struct mutex lock;
static int null_major;
static DEFINE_IDA(nullb_indexes);
static struct blk_mq_tag_set tag_set;

enum {
	NULL_IRQ_NONE		= 0,
	NULL_IRQ_SOFTIRQ	= 1,
	NULL_IRQ_TIMER		= 2,
};

static bool g_virt_boundary;
module_param_named(virt_boundary, g_virt_boundary, bool, 0444);
MODULE_PARM_DESC(virt_boundary, "Require a virtual boundary for the device. Default: False");

static int g_no_sched;
module_param_named(no_sched, g_no_sched, int, 0444);
MODULE_PARM_DESC(no_sched, "No io scheduler");

static int g_submit_queues = 1;
module_param_named(submit_queues, g_submit_queues, int, 0444);
MODULE_PARM_DESC(submit_queues, "Number of submission queues");

static int g_poll_queues = 1;
module_param_named(poll_queues, g_poll_queues, int, 0444);
MODULE_PARM_DESC(poll_queues, "Number of IOPOLL submission queues");

static int g_home_node = NUMA_NO_NODE;
module_param_named(home_node, g_home_node, int, 0444);
MODULE_PARM_DESC(home_node, "Home node for the device");

#ifdef CONFIG_BLK_DEV_NULL_BLK_FAULT_INJECTION
/*
 * For more details about fault injection, please refer to
 * Documentation/fault-injection/fault-injection.rst.
 */
static char g_timeout_str[80];
module_param_string(timeout, g_timeout_str, sizeof(g_timeout_str), 0444);
MODULE_PARM_DESC(timeout, "Fault injection. timeout=<interval>,<probability>,<space>,<times>");

static char g_requeue_str[80];
module_param_string(requeue, g_requeue_str, sizeof(g_requeue_str), 0444);
MODULE_PARM_DESC(requeue, "Fault injection. requeue=<interval>,<probability>,<space>,<times>");

static char g_init_hctx_str[80];
module_param_string(init_hctx, g_init_hctx_str, sizeof(g_init_hctx_str), 0444);
MODULE_PARM_DESC(init_hctx, "Fault injection to fail hctx init. init_hctx=<interval>,<probability>,<space>,<times>");
#endif

/*
 * Historic queue modes.
 *
 * These days nothing but NULL_Q_MQ is actually supported, but we keep it the
 * enum for error reporting.
 */
enum {
	NULL_Q_BIO	= 0,
	NULL_Q_RQ	= 1,
	NULL_Q_MQ	= 2,
};

static int g_queue_mode = NULL_Q_MQ;

static int null_param_store_val(const char *str, int *val, int min, int max)
{
	int ret, new_val;

	ret = kstrtoint(str, 10, &new_val);
	if (ret)
		return -EINVAL;

	if (new_val < min || new_val > max)
		return -EINVAL;

	*val = new_val;
	return 0;
}

static int null_set_queue_mode(const char *str, const struct kernel_param *kp)
{
	return null_param_store_val(str, &g_queue_mode, NULL_Q_BIO, NULL_Q_MQ);
}

static const struct kernel_param_ops null_queue_mode_param_ops = {
	.set	= null_set_queue_mode,
	.get	= param_get_int,
};

device_param_cb(queue_mode, &null_queue_mode_param_ops, &g_queue_mode, 0444);
MODULE_PARM_DESC(queue_mode, "Block interface to use (0=bio,1=rq,2=multiqueue)");

static int g_gb = 250;
module_param_named(gb, g_gb, int, 0444);
MODULE_PARM_DESC(gb, "Size in GB");

static int g_bs = 512;
module_param_named(bs, g_bs, int, 0444);
MODULE_PARM_DESC(bs, "Block size (in bytes)");

static int g_max_sectors;
module_param_named(max_sectors, g_max_sectors, int, 0444);
MODULE_PARM_DESC(max_sectors, "Maximum size of a command (in 512B sectors)");

static unsigned int nr_devices = 1;
module_param(nr_devices, uint, 0444);
MODULE_PARM_DESC(nr_devices, "Number of devices to register");

static bool g_blocking;
module_param_named(blocking, g_blocking, bool, 0444);
MODULE_PARM_DESC(blocking, "Register as a blocking blk-mq driver device");

static bool g_shared_tags;
module_param_named(shared_tags, g_shared_tags, bool, 0444);
MODULE_PARM_DESC(shared_tags, "Share tag set between devices for blk-mq");

static bool g_shared_tag_bitmap;
module_param_named(shared_tag_bitmap, g_shared_tag_bitmap, bool, 0444);
MODULE_PARM_DESC(shared_tag_bitmap, "Use shared tag bitmap for all submission queues for blk-mq");

static int g_irqmode = NULL_IRQ_SOFTIRQ;

static int null_set_irqmode(const char *str, const struct kernel_param *kp)
{
	return null_param_store_val(str, &g_irqmode, NULL_IRQ_NONE,
					NULL_IRQ_TIMER);
}

static const struct kernel_param_ops null_irqmode_param_ops = {
	.set	= null_set_irqmode,
	.get	= param_get_int,
};

device_param_cb(irqmode, &null_irqmode_param_ops, &g_irqmode, 0444);
MODULE_PARM_DESC(irqmode, "IRQ completion handler. 0-none, 1-softirq, 2-timer");

static unsigned long g_completion_nsec = 10000;
module_param_named(completion_nsec, g_completion_nsec, ulong, 0444);
MODULE_PARM_DESC(completion_nsec, "Time in ns to complete a request in hardware. Default: 10,000ns");

static int g_hw_queue_depth = 64;
module_param_named(hw_queue_depth, g_hw_queue_depth, int, 0444);
MODULE_PARM_DESC(hw_queue_depth, "Queue depth for each hardware queue. Default: 64");

static bool g_use_per_node_hctx;
module_param_named(use_per_node_hctx, g_use_per_node_hctx, bool, 0444);
MODULE_PARM_DESC(use_per_node_hctx, "Use per-node allocation for hardware context queues. Default: false");

static bool g_memory_backed;
module_param_named(memory_backed, g_memory_backed, bool, 0444);
MODULE_PARM_DESC(memory_backed, "Create a memory-backed block device. Default: false");

static bool g_discard;
module_param_named(discard, g_discard, bool, 0444);
MODULE_PARM_DESC(discard, "Support discard operations (requires memory-backed null_blk device). Default: false");

static unsigned long g_cache_size;
module_param_named(cache_size, g_cache_size, ulong, 0444);
MODULE_PARM_DESC(mbps, "Cache size in MiB for memory-backed device. Default: 0 (none)");

static bool g_fua = true;
module_param_named(fua, g_fua, bool, 0444);
MODULE_PARM_DESC(fua, "Enable/disable FUA support when cache_size is used. Default: true");

static unsigned int g_mbps;
module_param_named(mbps, g_mbps, uint, 0444);
MODULE_PARM_DESC(mbps, "Limit maximum bandwidth (in MiB/s). Default: 0 (no limit)");

static bool g_zoned;
module_param_named(zoned, g_zoned, bool, S_IRUGO);
MODULE_PARM_DESC(zoned, "Make device as a host-managed zoned block device. Default: false");

static unsigned long g_zone_size = 256;
module_param_named(zone_size, g_zone_size, ulong, S_IRUGO);
MODULE_PARM_DESC(zone_size, "Zone size in MB when block device is zoned. Must be power-of-two: Default: 256");

static unsigned long g_zone_capacity;
module_param_named(zone_capacity, g_zone_capacity, ulong, 0444);
MODULE_PARM_DESC(zone_capacity, "Zone capacity in MB when block device is zoned. Can be less than or equal to zone size. Default: Zone size");

static unsigned int g_zone_nr_conv;
module_param_named(zone_nr_conv, g_zone_nr_conv, uint, 0444);
MODULE_PARM_DESC(zone_nr_conv, "Number of conventional zones when block device is zoned. Default: 0");

static unsigned int g_zone_max_open;
module_param_named(zone_max_open, g_zone_max_open, uint, 0444);
MODULE_PARM_DESC(zone_max_open, "Maximum number of open zones when block device is zoned. Default: 0 (no limit)");

static unsigned int g_zone_max_active;
module_param_named(zone_max_active, g_zone_max_active, uint, 0444);
MODULE_PARM_DESC(zone_max_active, "Maximum number of active zones when block device is zoned. Default: 0 (no limit)");

static int g_zone_append_max_sectors = INT_MAX;
module_param_named(zone_append_max_sectors, g_zone_append_max_sectors, int, 0444);
MODULE_PARM_DESC(zone_append_max_sectors,
		 "Maximum size of a zone append command (in 512B sectors). Specify 0 for zone append emulation");

static bool g_zone_full;
module_param_named(zone_full, g_zone_full, bool, S_IRUGO);
MODULE_PARM_DESC(zone_full, "Initialize the sequential write required zones of a zoned device to be full. Default: false");

static bool g_rotational;
module_param_named(rotational, g_rotational, bool, S_IRUGO);
MODULE_PARM_DESC(rotational, "Set the rotational feature for the device. Default: false");

static struct nullb_device *null_alloc_dev(void);
static void null_free_dev(struct nullb_device *dev);
static void null_del_dev(struct nullb *nullb);
static int null_add_dev(struct nullb_device *dev);
static struct nullb *null_find_dev_by_name(const char *name);
static void null_free_device_storage(struct nullb_device *dev, bool is_cache);

static inline struct nullb_device *to_nullb_device(struct config_item *item)
{
	return item ? container_of(to_config_group(item), struct nullb_device, group) : NULL;
}

static inline ssize_t nullb_device_uint_attr_show(unsigned int val, char *page)
{
	return snprintf(page, PAGE_SIZE, "%u\n", val);
}

static inline ssize_t nullb_device_ulong_attr_show(unsigned long val,
	char *page)
{
	return snprintf(page, PAGE_SIZE, "%lu\n", val);
}

static inline ssize_t nullb_device_bool_attr_show(bool val, char *page)
{
	return snprintf(page, PAGE_SIZE, "%u\n", val);
}

static ssize_t nullb_device_uint_attr_store(unsigned int *val,
	const char *page, size_t count)
{
	unsigned int tmp;
	int result;

	result = kstrtouint(page, 0, &tmp);
	if (result < 0)
		return result;

	*val = tmp;
	return count;
}

static ssize_t nullb_device_ulong_attr_store(unsigned long *val,
	const char *page, size_t count)
{
	int result;
	unsigned long tmp;

	result = kstrtoul(page, 0, &tmp);
	if (result < 0)
		return result;

	*val = tmp;
	return count;
}

static ssize_t nullb_device_bool_attr_store(bool *val, const char *page,
	size_t count)
{
	bool tmp;
	int result;

	result = kstrtobool(page,  &tmp);
	if (result < 0)
		return result;

	*val = tmp;
	return count;
}

/* The following macro should only be used with TYPE = {uint, ulong, bool}. */
#define NULLB_DEVICE_ATTR(NAME, TYPE, APPLY)				\
static ssize_t								\
nullb_device_##NAME##_show(struct config_item *item, char *page)	\
{									\
	return nullb_device_##TYPE##_attr_show(				\
				to_nullb_device(item)->NAME, page);	\
}									\
static ssize_t								\
nullb_device_##NAME##_store(struct config_item *item, const char *page,	\
			    size_t count)				\
{									\
	int (*apply_fn)(struct nullb_device *dev, TYPE new_value) = APPLY;\
	struct nullb_device *dev = to_nullb_device(item);		\
	TYPE new_value = 0;						\
	int ret;							\
									\
	ret = nullb_device_##TYPE##_attr_store(&new_value, page, count);\
	if (ret < 0)							\
		return ret;						\
	if (apply_fn)							\
		ret = apply_fn(dev, new_value);				\
	else if (test_bit(NULLB_DEV_FL_CONFIGURED, &dev->flags)) 	\
		ret = -EBUSY;						\
	if (ret < 0)							\
		return ret;						\
	dev->NAME = new_value;						\
	return count;							\
}									\
CONFIGFS_ATTR(nullb_device_, NAME);

static int nullb_update_nr_hw_queues(struct nullb_device *dev,
				     unsigned int submit_queues,
				     unsigned int poll_queues)

{
	struct blk_mq_tag_set *set;
	int ret, nr_hw_queues;

	if (!dev->nullb)
		return 0;

	/*
	 * Make sure at least one submit queue exists.
	 */
	if (!submit_queues)
		return -EINVAL;

	/*
	 * Make sure that null_init_hctx() does not access nullb->queues[] past
	 * the end of that array.
	 */
	if (submit_queues > nr_cpu_ids || poll_queues > g_poll_queues)
		return -EINVAL;

	/*
	 * Keep previous and new queue numbers in nullb_device for reference in
	 * the call back function null_map_queues().
	 */
	dev->prev_submit_queues = dev->submit_queues;
	dev->prev_poll_queues = dev->poll_queues;
	dev->submit_queues = submit_queues;
	dev->poll_queues = poll_queues;

	set = dev->nullb->tag_set;
	nr_hw_queues = submit_queues + poll_queues;
	blk_mq_update_nr_hw_queues(set, nr_hw_queues);
	ret = set->nr_hw_queues == nr_hw_queues ? 0 : -ENOMEM;

	if (ret) {
		/* on error, revert the queue numbers */
		dev->submit_queues = dev->prev_submit_queues;
		dev->poll_queues = dev->prev_poll_queues;
	}

	return ret;
}

static int nullb_apply_submit_queues(struct nullb_device *dev,
				     unsigned int submit_queues)
{
	int ret;

	mutex_lock(&lock);
	ret = nullb_update_nr_hw_queues(dev, submit_queues, dev->poll_queues);
	mutex_unlock(&lock);

	return ret;
}

static int nullb_apply_poll_queues(struct nullb_device *dev,
				   unsigned int poll_queues)
{
	int ret;

	mutex_lock(&lock);
	ret = nullb_update_nr_hw_queues(dev, dev->submit_queues, poll_queues);
	mutex_unlock(&lock);

	return ret;
}

NULLB_DEVICE_ATTR(size, ulong, NULL);
NULLB_DEVICE_ATTR(completion_nsec, ulong, NULL);
NULLB_DEVICE_ATTR(submit_queues, uint, nullb_apply_submit_queues);
NULLB_DEVICE_ATTR(poll_queues, uint, nullb_apply_poll_queues);
NULLB_DEVICE_ATTR(home_node, uint, NULL);
NULLB_DEVICE_ATTR(queue_mode, uint, NULL);
NULLB_DEVICE_ATTR(blocksize, uint, NULL);
NULLB_DEVICE_ATTR(max_sectors, uint, NULL);
NULLB_DEVICE_ATTR(irqmode, uint, NULL);
NULLB_DEVICE_ATTR(hw_queue_depth, uint, NULL);
NULLB_DEVICE_ATTR(index, uint, NULL);
NULLB_DEVICE_ATTR(blocking, bool, NULL);
NULLB_DEVICE_ATTR(use_per_node_hctx, bool, NULL);
NULLB_DEVICE_ATTR(memory_backed, bool, NULL);
NULLB_DEVICE_ATTR(discard, bool, NULL);
NULLB_DEVICE_ATTR(mbps, uint, NULL);
NULLB_DEVICE_ATTR(cache_size, ulong, NULL);
NULLB_DEVICE_ATTR(zoned, bool, NULL);
NULLB_DEVICE_ATTR(zone_size, ulong, NULL);
NULLB_DEVICE_ATTR(zone_capacity, ulong, NULL);
NULLB_DEVICE_ATTR(zone_nr_conv, uint, NULL);
NULLB_DEVICE_ATTR(zone_max_open, uint, NULL);
NULLB_DEVICE_ATTR(zone_max_active, uint, NULL);
NULLB_DEVICE_ATTR(zone_append_max_sectors, uint, NULL);
NULLB_DEVICE_ATTR(zone_full, bool, NULL);
NULLB_DEVICE_ATTR(virt_boundary, bool, NULL);
NULLB_DEVICE_ATTR(no_sched, bool, NULL);
NULLB_DEVICE_ATTR(shared_tags, bool, NULL);
NULLB_DEVICE_ATTR(shared_tag_bitmap, bool, NULL);
NULLB_DEVICE_ATTR(fua, bool, NULL);
NULLB_DEVICE_ATTR(rotational, bool, NULL);
NULLB_DEVICE_ATTR(badblocks_once, bool, NULL);
NULLB_DEVICE_ATTR(badblocks_partial_io, bool, NULL);

static ssize_t nullb_device_power_show(struct config_item *item, char *page)
{
	return nullb_device_bool_attr_show(to_nullb_device(item)->power, page);
}

static ssize_t nullb_device_power_store(struct config_item *item,
				     const char *page, size_t count)
{
	struct nullb_device *dev = to_nullb_device(item);
	bool newp = false;
	ssize_t ret;

	ret = nullb_device_bool_attr_store(&newp, page, count);
	if (ret < 0)
		return ret;

	ret = count;
	mutex_lock(&lock);
	if (!dev->power && newp) {
		if (test_and_set_bit(NULLB_DEV_FL_UP, &dev->flags))
			goto out;

		ret = null_add_dev(dev);
		if (ret) {
			clear_bit(NULLB_DEV_FL_UP, &dev->flags);
			goto out;
		}

		set_bit(NULLB_DEV_FL_CONFIGURED, &dev->flags);
		dev->power = newp;
		ret = count;
	} else if (dev->power && !newp) {
		if (test_and_clear_bit(NULLB_DEV_FL_UP, &dev->flags)) {
			dev->power = newp;
			null_del_dev(dev->nullb);
		}
		clear_bit(NULLB_DEV_FL_CONFIGURED, &dev->flags);
	}

out:
	mutex_unlock(&lock);
	return ret;
}

CONFIGFS_ATTR(nullb_device_, power);

static ssize_t nullb_device_badblocks_show(struct config_item *item, char *page)
{
	struct nullb_device *t_dev = to_nullb_device(item);

	return badblocks_show(&t_dev->badblocks, page, 0);
}

static ssize_t nullb_device_badblocks_store(struct config_item *item,
				     const char *page, size_t count)
{
	struct nullb_device *t_dev = to_nullb_device(item);
	char *orig, *buf, *tmp;
	u64 start, end;
	int ret;

	orig = kstrndup(page, count, GFP_KERNEL);
	if (!orig)
		return -ENOMEM;

	buf = strstrip(orig);

	ret = -EINVAL;
	if (buf[0] != '+' && buf[0] != '-')
		goto out;
	tmp = strchr(&buf[1], '-');
	if (!tmp)
		goto out;
	*tmp = '\0';
	ret = kstrtoull(buf + 1, 0, &start);
	if (ret)
		goto out;
	ret = kstrtoull(tmp + 1, 0, &end);
	if (ret)
		goto out;
	ret = -EINVAL;
	if (start > end)
		goto out;
	/* enable badblocks */
	cmpxchg(&t_dev->badblocks.shift, -1, 0);
	if (buf[0] == '+') {
		if (badblocks_set(&t_dev->badblocks, start,
				  end - start + 1, 1))
			ret = count;
	} else if (badblocks_clear(&t_dev->badblocks, start,
				   end - start + 1)) {
		ret = count;
	}
out:
	kfree(orig);
	return ret;
}
CONFIGFS_ATTR(nullb_device_, badblocks);

static ssize_t nullb_device_zone_readonly_store(struct config_item *item,
						const char *page, size_t count)
{
	struct nullb_device *dev = to_nullb_device(item);

	return zone_cond_store(dev, page, count, BLK_ZONE_COND_READONLY);
}
CONFIGFS_ATTR_WO(nullb_device_, zone_readonly);

static ssize_t nullb_device_zone_offline_store(struct config_item *item,
					       const char *page, size_t count)
{
	struct nullb_device *dev = to_nullb_device(item);

	return zone_cond_store(dev, page, count, BLK_ZONE_COND_OFFLINE);
}
CONFIGFS_ATTR_WO(nullb_device_, zone_offline);

static struct configfs_attribute *nullb_device_attrs[] = {
	&nullb_device_attr_badblocks,
	&nullb_device_attr_badblocks_once,
	&nullb_device_attr_badblocks_partial_io,
	&nullb_device_attr_blocking,
	&nullb_device_attr_blocksize,
	&nullb_device_attr_cache_size,
	&nullb_device_attr_completion_nsec,
	&nullb_device_attr_discard,
	&nullb_device_attr_fua,
	&nullb_device_attr_home_node,
	&nullb_device_attr_hw_queue_depth,
	&nullb_device_attr_index,
	&nullb_device_attr_irqmode,
	&nullb_device_attr_max_sectors,
	&nullb_device_attr_mbps,
	&nullb_device_attr_memory_backed,
	&nullb_device_attr_no_sched,
	&nullb_device_attr_poll_queues,
	&nullb_device_attr_power,
	&nullb_device_attr_queue_mode,
	&nullb_device_attr_rotational,
	&nullb_device_attr_shared_tag_bitmap,
	&nullb_device_attr_shared_tags,
	&nullb_device_attr_size,
	&nullb_device_attr_submit_queues,
	&nullb_device_attr_use_per_node_hctx,
	&nullb_device_attr_virt_boundary,
	&nullb_device_attr_zone_append_max_sectors,
	&nullb_device_attr_zone_capacity,
	&nullb_device_attr_zone_full,
	&nullb_device_attr_zone_max_active,
	&nullb_device_attr_zone_max_open,
	&nullb_device_attr_zone_nr_conv,
	&nullb_device_attr_zone_offline,
	&nullb_device_attr_zone_readonly,
	&nullb_device_attr_zone_size,
	&nullb_device_attr_zoned,
	NULL,
};

static void nullb_device_release(struct config_item *item)
{
	struct nullb_device *dev = to_nullb_device(item);

	null_free_device_storage(dev, false);
	null_free_dev(dev);
}

static struct configfs_item_operations nullb_device_ops = {
	.release	= nullb_device_release,
};

static const struct config_item_type nullb_device_type = {
	.ct_item_ops	= &nullb_device_ops,
	.ct_attrs	= nullb_device_attrs,
	.ct_owner	= THIS_MODULE,
};

#ifdef CONFIG_BLK_DEV_NULL_BLK_FAULT_INJECTION

static void nullb_add_fault_config(struct nullb_device *dev)
{
	fault_config_init(&dev->timeout_config, "timeout_inject");
	fault_config_init(&dev->requeue_config, "requeue_inject");
	fault_config_init(&dev->init_hctx_fault_config, "init_hctx_fault_inject");

	configfs_add_default_group(&dev->timeout_config.group, &dev->group);
	configfs_add_default_group(&dev->requeue_config.group, &dev->group);
	configfs_add_default_group(&dev->init_hctx_fault_config.group, &dev->group);
}

#else

static void nullb_add_fault_config(struct nullb_device *dev)
{
}

#endif

static struct
config_group *nullb_group_make_group(struct config_group *group, const char *name)
{
	struct nullb_device *dev;

	if (null_find_dev_by_name(name))
		return ERR_PTR(-EEXIST);

	dev = null_alloc_dev();
	if (!dev)
		return ERR_PTR(-ENOMEM);

	config_group_init_type_name(&dev->group, name, &nullb_device_type);
	nullb_add_fault_config(dev);

	return &dev->group;
}

static void
nullb_group_drop_item(struct config_group *group, struct config_item *item)
{
	struct nullb_device *dev = to_nullb_device(item);

	if (test_and_clear_bit(NULLB_DEV_FL_UP, &dev->flags)) {
		mutex_lock(&lock);
		dev->power = false;
		null_del_dev(dev->nullb);
		mutex_unlock(&lock);
	}

	config_item_put(item);
}

static ssize_t memb_group_features_show(struct config_item *item, char *page)
{

	struct configfs_attribute **entry;
	char delimiter = ',';
	size_t left = PAGE_SIZE;
	size_t written = 0;
	int ret;

	for (entry = &nullb_device_attrs[0]; *entry && left > 0; entry++) {
		if (!*(entry + 1))
			delimiter = '\n';
		ret = snprintf(page + written, left, "%s%c", (*entry)->ca_name,
			       delimiter);
		if (ret >= left) {
			WARN_ONCE(1, "Too many null_blk features to print\n");
			memzero_explicit(page, PAGE_SIZE);
			return -ENOBUFS;
		}
		left -= ret;
		written += ret;
	}

	return written;
}

CONFIGFS_ATTR_RO(memb_group_, features);

static struct configfs_attribute *nullb_group_attrs[] = {
	&memb_group_attr_features,
	NULL,
};

static struct configfs_group_operations nullb_group_ops = {
	.make_group	= nullb_group_make_group,
	.drop_item	= nullb_group_drop_item,
};

static const struct config_item_type nullb_group_type = {
	.ct_group_ops	= &nullb_group_ops,
	.ct_attrs	= nullb_group_attrs,
	.ct_owner	= THIS_MODULE,
};

static struct configfs_subsystem nullb_subsys = {
	.su_group = {
		.cg_item = {
			.ci_namebuf = "nullb",
			.ci_type = &nullb_group_type,
		},
	},
};

static inline int null_cache_active(struct nullb *nullb)
{
	return test_bit(NULLB_DEV_FL_CACHE, &nullb->dev->flags);
}

static struct nullb_device *null_alloc_dev(void)
{
	struct nullb_device *dev;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return NULL;

#ifdef CONFIG_BLK_DEV_NULL_BLK_FAULT_INJECTION
	dev->timeout_config.attr = null_timeout_attr;
	dev->requeue_config.attr = null_requeue_attr;
	dev->init_hctx_fault_config.attr = null_init_hctx_attr;
#endif

	INIT_RADIX_TREE(&dev->data, GFP_ATOMIC);
	INIT_RADIX_TREE(&dev->cache, GFP_ATOMIC);
	if (badblocks_init(&dev->badblocks, 0)) {
		kfree(dev);
		return NULL;
	}

	dev->size = g_gb * 1024;
	dev->completion_nsec = g_completion_nsec;
	dev->submit_queues = g_submit_queues;
	dev->prev_submit_queues = g_submit_queues;
	dev->poll_queues = g_poll_queues;
	dev->prev_poll_queues = g_poll_queues;
	dev->home_node = g_home_node;
	dev->queue_mode = g_queue_mode;
	dev->blocksize = g_bs;
	dev->max_sectors = g_max_sectors;
	dev->irqmode = g_irqmode;
	dev->hw_queue_depth = g_hw_queue_depth;
	dev->blocking = g_blocking;
	dev->memory_backed = g_memory_backed;
	dev->discard = g_discard;
	dev->cache_size = g_cache_size;
	dev->mbps = g_mbps;
	dev->use_per_node_hctx = g_use_per_node_hctx;
	dev->zoned = g_zoned;
	dev->zone_size = g_zone_size;
	dev->zone_capacity = g_zone_capacity;
	dev->zone_nr_conv = g_zone_nr_conv;
	dev->zone_max_open = g_zone_max_open;
	dev->zone_max_active = g_zone_max_active;
	dev->zone_append_max_sectors = g_zone_append_max_sectors;
	dev->zone_full = g_zone_full;
	dev->virt_boundary = g_virt_boundary;
	dev->no_sched = g_no_sched;
	dev->shared_tags = g_shared_tags;
	dev->shared_tag_bitmap = g_shared_tag_bitmap;
	dev->fua = g_fua;
	dev->rotational = g_rotational;

	return dev;
}

static void null_free_dev(struct nullb_device *dev)
{
	if (!dev)
		return;

	null_free_zoned_dev(dev);
	badblocks_exit(&dev->badblocks);
	kfree(dev);
}

static enum hrtimer_restart null_cmd_timer_expired(struct hrtimer *timer)
{
	struct nullb_cmd *cmd = container_of(timer, struct nullb_cmd, timer);

	blk_mq_end_request(blk_mq_rq_from_pdu(cmd), cmd->error);
	return HRTIMER_NORESTART;
}

static void null_cmd_end_timer(struct nullb_cmd *cmd)
{
	ktime_t kt = cmd->nq->dev->completion_nsec;

	hrtimer_start(&cmd->timer, kt, HRTIMER_MODE_REL);
}

static void null_complete_rq(struct request *rq)
{
	struct nullb_cmd *cmd = blk_mq_rq_to_pdu(rq);

	blk_mq_end_request(rq, cmd->error);
}

static struct nullb_page *null_alloc_page(void)
{
	struct nullb_page *t_page;

	t_page = kmalloc(sizeof(struct nullb_page), GFP_NOIO);
	if (!t_page)
		return NULL;

	t_page->page = alloc_pages(GFP_NOIO, 0);
	if (!t_page->page) {
		kfree(t_page);
		return NULL;
	}

	memset(t_page->bitmap, 0, sizeof(t_page->bitmap));
	return t_page;
}

static void null_free_page(struct nullb_page *t_page)
{
	__set_bit(NULLB_PAGE_FREE, t_page->bitmap);
	if (test_bit(NULLB_PAGE_LOCK, t_page->bitmap))
		return;
	__free_page(t_page->page);
	kfree(t_page);
}

static bool null_page_empty(struct nullb_page *page)
{
	int size = MAP_SZ - 2;

	return find_first_bit(page->bitmap, size) == size;
}

static void null_free_sector(struct nullb *nullb, sector_t sector,
	bool is_cache)
{
	unsigned int sector_bit;
	u64 idx;
	struct nullb_page *t_page, *ret;
	struct radix_tree_root *root;

	root = is_cache ? &nullb->dev->cache : &nullb->dev->data;
	idx = sector >> PAGE_SECTORS_SHIFT;
	sector_bit = (sector & SECTOR_MASK);

	t_page = radix_tree_lookup(root, idx);
	if (t_page) {
		__clear_bit(sector_bit, t_page->bitmap);

		if (null_page_empty(t_page)) {
			ret = radix_tree_delete_item(root, idx, t_page);
			WARN_ON(ret != t_page);
			null_free_page(ret);
			if (is_cache)
				nullb->dev->curr_cache -= PAGE_SIZE;
		}
	}
}

static struct nullb_page *null_radix_tree_insert(struct nullb *nullb, u64 idx,
	struct nullb_page *t_page, bool is_cache)
{
	struct radix_tree_root *root;

	root = is_cache ? &nullb->dev->cache : &nullb->dev->data;

	if (radix_tree_insert(root, idx, t_page)) {
		null_free_page(t_page);
		t_page = radix_tree_lookup(root, idx);
		WARN_ON(!t_page || t_page->page->private != idx);
	} else if (is_cache)
		nullb->dev->curr_cache += PAGE_SIZE;

	return t_page;
}

static void null_free_device_storage(struct nullb_device *dev, bool is_cache)
{
	unsigned long pos = 0;
	int nr_pages;
	struct nullb_page *ret, *t_pages[FREE_BATCH];
	struct radix_tree_root *root;

	root = is_cache ? &dev->cache : &dev->data;

	do {
		int i;

		nr_pages = radix_tree_gang_lookup(root,
				(void **)t_pages, pos, FREE_BATCH);

		for (i = 0; i < nr_pages; i++) {
			pos = t_pages[i]->page->private;
			ret = radix_tree_delete_item(root, pos, t_pages[i]);
			WARN_ON(ret != t_pages[i]);
			null_free_page(ret);
		}

		pos++;
	} while (nr_pages == FREE_BATCH);

	if (is_cache)
		dev->curr_cache = 0;
}

static struct nullb_page *__null_lookup_page(struct nullb *nullb,
	sector_t sector, bool for_write, bool is_cache)
{
	unsigned int sector_bit;
	u64 idx;
	struct nullb_page *t_page;
	struct radix_tree_root *root;

	idx = sector >> PAGE_SECTORS_SHIFT;
	sector_bit = (sector & SECTOR_MASK);

	root = is_cache ? &nullb->dev->cache : &nullb->dev->data;
	t_page = radix_tree_lookup(root, idx);
	WARN_ON(t_page && t_page->page->private != idx);

	if (t_page && (for_write || test_bit(sector_bit, t_page->bitmap)))
		return t_page;

	return NULL;
}

static struct nullb_page *null_lookup_page(struct nullb *nullb,
	sector_t sector, bool for_write, bool ignore_cache)
{
	struct nullb_page *page = NULL;

	if (!ignore_cache)
		page = __null_lookup_page(nullb, sector, for_write, true);
	if (page)
		return page;
	return __null_lookup_page(nullb, sector, for_write, false);
}

static struct nullb_page *null_insert_page(struct nullb *nullb,
					   sector_t sector, bool ignore_cache)
	__releases(&nullb->lock)
	__acquires(&nullb->lock)
{
	u64 idx;
	struct nullb_page *t_page;

	t_page = null_lookup_page(nullb, sector, true, ignore_cache);
	if (t_page)
		return t_page;

	spin_unlock_irq(&nullb->lock);

	t_page = null_alloc_page();
	if (!t_page)
		goto out_lock;

	if (radix_tree_preload(GFP_NOIO))
		goto out_freepage;

	spin_lock_irq(&nullb->lock);
	idx = sector >> PAGE_SECTORS_SHIFT;
	t_page->page->private = idx;
	t_page = null_radix_tree_insert(nullb, idx, t_page, !ignore_cache);
	radix_tree_preload_end();

	return t_page;
out_freepage:
	null_free_page(t_page);
out_lock:
	spin_lock_irq(&nullb->lock);
	return null_lookup_page(nullb, sector, true, ignore_cache);
}

static int null_flush_cache_page(struct nullb *nullb, struct nullb_page *c_page)
{
	int i;
	unsigned int offset;
	u64 idx;
	struct nullb_page *t_page, *ret;
	void *dst, *src;

	idx = c_page->page->private;

	t_page = null_insert_page(nullb, idx << PAGE_SECTORS_SHIFT, true);

	__clear_bit(NULLB_PAGE_LOCK, c_page->bitmap);
	if (test_bit(NULLB_PAGE_FREE, c_page->bitmap)) {
		null_free_page(c_page);
		if (t_page && null_page_empty(t_page)) {
			ret = radix_tree_delete_item(&nullb->dev->data,
				idx, t_page);
			null_free_page(t_page);
		}
		return 0;
	}

	if (!t_page)
		return -ENOMEM;

	src = kmap_local_page(c_page->page);
	dst = kmap_local_page(t_page->page);

	for (i = 0; i < PAGE_SECTORS;
			i += (nullb->dev->blocksize >> SECTOR_SHIFT)) {
		if (test_bit(i, c_page->bitmap)) {
			offset = (i << SECTOR_SHIFT);
			memcpy(dst + offset, src + offset,
				nullb->dev->blocksize);
			__set_bit(i, t_page->bitmap);
		}
	}

	kunmap_local(dst);
	kunmap_local(src);

	ret = radix_tree_delete_item(&nullb->dev->cache, idx, c_page);
	null_free_page(ret);
	nullb->dev->curr_cache -= PAGE_SIZE;

	return 0;
}

static int null_make_cache_space(struct nullb *nullb, unsigned long n)
{
	int i, err, nr_pages;
	struct nullb_page *c_pages[FREE_BATCH];
	unsigned long flushed = 0, one_round;

again:
	if ((nullb->dev->cache_size * 1024 * 1024) >
	     nullb->dev->curr_cache + n || nullb->dev->curr_cache == 0)
		return 0;

	nr_pages = radix_tree_gang_lookup(&nullb->dev->cache,
			(void **)c_pages, nullb->cache_flush_pos, FREE_BATCH);
	/*
	 * nullb_flush_cache_page could unlock before using the c_pages. To
	 * avoid race, we don't allow page free
	 */
	for (i = 0; i < nr_pages; i++) {
		nullb->cache_flush_pos = c_pages[i]->page->private;
		/*
		 * We found the page which is being flushed to disk by other
		 * threads
		 */
		if (test_bit(NULLB_PAGE_LOCK, c_pages[i]->bitmap))
			c_pages[i] = NULL;
		else
			__set_bit(NULLB_PAGE_LOCK, c_pages[i]->bitmap);
	}

	one_round = 0;
	for (i = 0; i < nr_pages; i++) {
		if (c_pages[i] == NULL)
			continue;
		err = null_flush_cache_page(nullb, c_pages[i]);
		if (err)
			return err;
		one_round++;
	}
	flushed += one_round << PAGE_SHIFT;

	if (n > flushed) {
		if (nr_pages == 0)
			nullb->cache_flush_pos = 0;
		if (one_round == 0) {
			/* give other threads a chance */
			spin_unlock_irq(&nullb->lock);
			spin_lock_irq(&nullb->lock);
		}
		goto again;
	}
	return 0;
}

static int copy_to_nullb(struct nullb *nullb, struct page *source,
	unsigned int off, sector_t sector, size_t n, bool is_fua)
{
	size_t temp, count = 0;
	unsigned int offset;
	struct nullb_page *t_page;

	while (count < n) {
		temp = min_t(size_t, nullb->dev->blocksize, n - count);

		if (null_cache_active(nullb) && !is_fua)
			null_make_cache_space(nullb, PAGE_SIZE);

		offset = (sector & SECTOR_MASK) << SECTOR_SHIFT;
		t_page = null_insert_page(nullb, sector,
			!null_cache_active(nullb) || is_fua);
		if (!t_page)
			return -ENOSPC;

		memcpy_page(t_page->page, offset, source, off + count, temp);

		__set_bit(sector & SECTOR_MASK, t_page->bitmap);

		if (is_fua)
			null_free_sector(nullb, sector, true);

		count += temp;
		sector += temp >> SECTOR_SHIFT;
	}
	return 0;
}

static int copy_from_nullb(struct nullb *nullb, struct page *dest,
	unsigned int off, sector_t sector, size_t n)
{
	size_t temp, count = 0;
	unsigned int offset;
	struct nullb_page *t_page;

	while (count < n) {
		temp = min_t(size_t, nullb->dev->blocksize, n - count);

		offset = (sector & SECTOR_MASK) << SECTOR_SHIFT;
		t_page = null_lookup_page(nullb, sector, false,
			!null_cache_active(nullb));

		if (t_page)
			memcpy_page(dest, off + count, t_page->page, offset,
				    temp);
		else
			memzero_page(dest, off + count, temp);

		count += temp;
		sector += temp >> SECTOR_SHIFT;
	}
	return 0;
}

static void nullb_fill_pattern(struct nullb *nullb, struct page *page,
			       unsigned int len, unsigned int off)
{
	memset_page(page, off, 0xff, len);
}

blk_status_t null_handle_discard(struct nullb_device *dev,
				 sector_t sector, sector_t nr_sectors)
{
	struct nullb *nullb = dev->nullb;
	size_t n = nr_sectors << SECTOR_SHIFT;
	size_t temp;

	spin_lock_irq(&nullb->lock);
	while (n > 0) {
		temp = min_t(size_t, n, dev->blocksize);
		null_free_sector(nullb, sector, false);
		if (null_cache_active(nullb))
			null_free_sector(nullb, sector, true);
		sector += temp >> SECTOR_SHIFT;
		n -= temp;
	}
	spin_unlock_irq(&nullb->lock);

	return BLK_STS_OK;
}

static blk_status_t null_handle_flush(struct nullb *nullb)
{
	int err;

	if (!null_cache_active(nullb))
		return 0;

	spin_lock_irq(&nullb->lock);
	while (true) {
		err = null_make_cache_space(nullb,
			nullb->dev->cache_size * 1024 * 1024);
		if (err || nullb->dev->curr_cache == 0)
			break;
	}

	WARN_ON(!radix_tree_empty(&nullb->dev->cache));
	spin_unlock_irq(&nullb->lock);
	return errno_to_blk_status(err);
}

static int null_transfer(struct nullb *nullb, struct page *page,
	unsigned int len, unsigned int off, bool is_write, sector_t sector,
	bool is_fua)
{
	struct nullb_device *dev = nullb->dev;
	unsigned int valid_len = len;
	int err = 0;

	if (!is_write) {
		if (dev->zoned)
			valid_len = null_zone_valid_read_len(nullb,
				sector, len);

		if (valid_len) {
			err = copy_from_nullb(nullb, page, off,
				sector, valid_len);
			off += valid_len;
			len -= valid_len;
		}

		if (len)
			nullb_fill_pattern(nullb, page, len, off);
		flush_dcache_page(page);
	} else {
		flush_dcache_page(page);
		err = copy_to_nullb(nullb, page, off, sector, len, is_fua);
	}

	return err;
}

/*
 * Transfer data for the given request. The transfer size is capped with the
 * nr_sectors argument.
 */
static blk_status_t null_handle_data_transfer(struct nullb_cmd *cmd,
					      sector_t nr_sectors)
{
	struct request *rq = blk_mq_rq_from_pdu(cmd);
	struct nullb *nullb = cmd->nq->dev->nullb;
	int err = 0;
	unsigned int len;
	sector_t sector = blk_rq_pos(rq);
	unsigned int max_bytes = nr_sectors << SECTOR_SHIFT;
	unsigned int transferred_bytes = 0;
	struct req_iterator iter;
	struct bio_vec bvec;

	spin_lock_irq(&nullb->lock);
	rq_for_each_segment(bvec, rq, iter) {
		len = bvec.bv_len;
		if (transferred_bytes + len > max_bytes)
			len = max_bytes - transferred_bytes;
		err = null_transfer(nullb, bvec.bv_page, len, bvec.bv_offset,
				     op_is_write(req_op(rq)), sector,
				     rq->cmd_flags & REQ_FUA);
		if (err)
			break;
		sector += len >> SECTOR_SHIFT;
		transferred_bytes += len;
		if (transferred_bytes >= max_bytes)
			break;
	}
	spin_unlock_irq(&nullb->lock);

	return errno_to_blk_status(err);
}

static inline blk_status_t null_handle_throttled(struct nullb_cmd *cmd)
{
	struct nullb_device *dev = cmd->nq->dev;
	struct nullb *nullb = dev->nullb;
	blk_status_t sts = BLK_STS_OK;
	struct request *rq = blk_mq_rq_from_pdu(cmd);

	if (!hrtimer_active(&nullb->bw_timer))
		hrtimer_restart(&nullb->bw_timer);

	if (atomic_long_sub_return(blk_rq_bytes(rq), &nullb->cur_bytes) < 0) {
		blk_mq_stop_hw_queues(nullb->q);
		/* race with timer */
		if (atomic_long_read(&nullb->cur_bytes) > 0)
			blk_mq_start_stopped_hw_queues(nullb->q, true);
		/* requeue request */
		sts = BLK_STS_DEV_RESOURCE;
	}
	return sts;
}

/*
 * Check if the command should fail for the badblocks. If so, return
 * BLK_STS_IOERR and return number of partial I/O sectors to be written or read,
 * which may be less than the requested number of sectors.
 *
 * @cmd:        The command to handle.
 * @sector:     The start sector for I/O.
 * @nr_sectors: Specifies number of sectors to write or read, and returns the
 *              number of sectors to be written or read.
 */
blk_status_t null_handle_badblocks(struct nullb_cmd *cmd, sector_t sector,
				   unsigned int *nr_sectors)
{
	struct badblocks *bb = &cmd->nq->dev->badblocks;
	struct nullb_device *dev = cmd->nq->dev;
	unsigned int block_sectors = dev->blocksize >> SECTOR_SHIFT;
	sector_t first_bad, bad_sectors;
	unsigned int partial_io_sectors = 0;

	if (!badblocks_check(bb, sector, *nr_sectors, &first_bad, &bad_sectors))
		return BLK_STS_OK;

	if (cmd->nq->dev->badblocks_once)
		badblocks_clear(bb, first_bad, bad_sectors);

	if (cmd->nq->dev->badblocks_partial_io) {
		if (!IS_ALIGNED(first_bad, block_sectors))
			first_bad = ALIGN_DOWN(first_bad, block_sectors);
		if (sector < first_bad)
			partial_io_sectors = first_bad - sector;
	}
	*nr_sectors = partial_io_sectors;

	return BLK_STS_IOERR;
}

blk_status_t null_handle_memory_backed(struct nullb_cmd *cmd, enum req_op op,
				       sector_t sector, sector_t nr_sectors)
{
	struct nullb_device *dev = cmd->nq->dev;

	if (op == REQ_OP_DISCARD)
		return null_handle_discard(dev, sector, nr_sectors);

	return null_handle_data_transfer(cmd, nr_sectors);
}

static void nullb_zero_read_cmd_buffer(struct nullb_cmd *cmd)
{
	struct request *rq = blk_mq_rq_from_pdu(cmd);
	struct nullb_device *dev = cmd->nq->dev;
	struct bio *bio;

	if (!dev->memory_backed && req_op(rq) == REQ_OP_READ) {
		__rq_for_each_bio(bio, rq)
			zero_fill_bio(bio);
	}
}

static inline void nullb_complete_cmd(struct nullb_cmd *cmd)
{
	struct request *rq = blk_mq_rq_from_pdu(cmd);

	/*
	 * Since root privileges are required to configure the null_blk
	 * driver, it is fine that this driver does not initialize the
	 * data buffers of read commands. Zero-initialize these buffers
	 * anyway if KMSAN is enabled to prevent that KMSAN complains
	 * about null_blk not initializing read data buffers.
	 */
	if (IS_ENABLED(CONFIG_KMSAN))
		nullb_zero_read_cmd_buffer(cmd);

	/* Complete IO by inline, softirq or timer */
	switch (cmd->nq->dev->irqmode) {
	case NULL_IRQ_SOFTIRQ:
		blk_mq_complete_request(rq);
		break;
	case NULL_IRQ_NONE:
		blk_mq_end_request(rq, cmd->error);
		break;
	case NULL_IRQ_TIMER:
		null_cmd_end_timer(cmd);
		break;
	}
}

blk_status_t null_process_cmd(struct nullb_cmd *cmd, enum req_op op,
			      sector_t sector, unsigned int nr_sectors)
{
	struct nullb_device *dev = cmd->nq->dev;
	blk_status_t badblocks_ret = BLK_STS_OK;
	blk_status_t ret;

	if (dev->badblocks.shift != -1)
		badblocks_ret = null_handle_badblocks(cmd, sector, &nr_sectors);

	if (dev->memory_backed && nr_sectors) {
		ret = null_handle_memory_backed(cmd, op, sector, nr_sectors);
		if (ret != BLK_STS_OK)
			return ret;
	}

	return badblocks_ret;
}

static void null_handle_cmd(struct nullb_cmd *cmd, sector_t sector,
			    sector_t nr_sectors, enum req_op op)
{
	struct nullb_device *dev = cmd->nq->dev;
	struct nullb *nullb = dev->nullb;
	blk_status_t sts;

	if (op == REQ_OP_FLUSH) {
		cmd->error = null_handle_flush(nullb);
		goto out;
	}

	if (dev->zoned)
		sts = null_process_zoned_cmd(cmd, op, sector, nr_sectors);
	else
		sts = null_process_cmd(cmd, op, sector, nr_sectors);

	/* Do not overwrite errors (e.g. timeout errors) */
	if (cmd->error == BLK_STS_OK)
		cmd->error = sts;

out:
	nullb_complete_cmd(cmd);
}

static enum hrtimer_restart nullb_bwtimer_fn(struct hrtimer *timer)
{
	struct nullb *nullb = container_of(timer, struct nullb, bw_timer);
	ktime_t timer_interval = ktime_set(0, TIMER_INTERVAL);
	unsigned int mbps = nullb->dev->mbps;

	if (atomic_long_read(&nullb->cur_bytes) == mb_per_tick(mbps))
		return HRTIMER_NORESTART;

	atomic_long_set(&nullb->cur_bytes, mb_per_tick(mbps));
	blk_mq_start_stopped_hw_queues(nullb->q, true);

	hrtimer_forward_now(&nullb->bw_timer, timer_interval);

	return HRTIMER_RESTART;
}

static void nullb_setup_bwtimer(struct nullb *nullb)
{
	ktime_t timer_interval = ktime_set(0, TIMER_INTERVAL);

	hrtimer_setup(&nullb->bw_timer, nullb_bwtimer_fn, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	atomic_long_set(&nullb->cur_bytes, mb_per_tick(nullb->dev->mbps));
	hrtimer_start(&nullb->bw_timer, timer_interval, HRTIMER_MODE_REL);
}

#ifdef CONFIG_BLK_DEV_NULL_BLK_FAULT_INJECTION

static bool should_timeout_request(struct request *rq)
{
	struct nullb_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct nullb_device *dev = cmd->nq->dev;

	return should_fail(&dev->timeout_config.attr, 1);
}

static bool should_requeue_request(struct request *rq)
{
	struct nullb_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct nullb_device *dev = cmd->nq->dev;

	return should_fail(&dev->requeue_config.attr, 1);
}

static bool should_init_hctx_fail(struct nullb_device *dev)
{
	return should_fail(&dev->init_hctx_fault_config.attr, 1);
}

#else

static bool should_timeout_request(struct request *rq)
{
	return false;
}

static bool should_requeue_request(struct request *rq)
{
	return false;
}

static bool should_init_hctx_fail(struct nullb_device *dev)
{
	return false;
}

#endif

static void null_map_queues(struct blk_mq_tag_set *set)
{
	struct nullb *nullb = set->driver_data;
	int i, qoff;
	unsigned int submit_queues = g_submit_queues;
	unsigned int poll_queues = g_poll_queues;

	if (nullb) {
		struct nullb_device *dev = nullb->dev;

		/*
		 * Refer nr_hw_queues of the tag set to check if the expected
		 * number of hardware queues are prepared. If block layer failed
		 * to prepare them, use previous numbers of submit queues and
		 * poll queues to map queues.
		 */
		if (set->nr_hw_queues ==
		    dev->submit_queues + dev->poll_queues) {
			submit_queues = dev->submit_queues;
			poll_queues = dev->poll_queues;
		} else if (set->nr_hw_queues ==
			   dev->prev_submit_queues + dev->prev_poll_queues) {
			submit_queues = dev->prev_submit_queues;
			poll_queues = dev->prev_poll_queues;
		} else {
			pr_warn("tag set has unexpected nr_hw_queues: %d\n",
				set->nr_hw_queues);
			WARN_ON_ONCE(true);
			submit_queues = 1;
			poll_queues = 0;
		}
	}

	for (i = 0, qoff = 0; i < set->nr_maps; i++) {
		struct blk_mq_queue_map *map = &set->map[i];

		switch (i) {
		case HCTX_TYPE_DEFAULT:
			map->nr_queues = submit_queues;
			break;
		case HCTX_TYPE_READ:
			map->nr_queues = 0;
			continue;
		case HCTX_TYPE_POLL:
			map->nr_queues = poll_queues;
			break;
		}
		map->queue_offset = qoff;
		qoff += map->nr_queues;
		blk_mq_map_queues(map);
	}
}

static int null_poll(struct blk_mq_hw_ctx *hctx, struct io_comp_batch *iob)
{
	struct nullb_queue *nq = hctx->driver_data;
	LIST_HEAD(list);
	int nr = 0;
	struct request *rq;

	spin_lock(&nq->poll_lock);
	list_splice_init(&nq->poll_list, &list);
	list_for_each_entry(rq, &list, queuelist)
		blk_mq_set_request_complete(rq);
	spin_unlock(&nq->poll_lock);

	while (!list_empty(&list)) {
		struct nullb_cmd *cmd;
		struct request *req;

		req = list_first_entry(&list, struct request, queuelist);
		list_del_init(&req->queuelist);
		cmd = blk_mq_rq_to_pdu(req);
		cmd->error = null_process_cmd(cmd, req_op(req), blk_rq_pos(req),
						blk_rq_sectors(req));
		if (!blk_mq_add_to_batch(req, iob, cmd->error != BLK_STS_OK,
					 blk_mq_end_request_batch))
			blk_mq_end_request(req, cmd->error);
		nr++;
	}

	return nr;
}

static enum blk_eh_timer_return null_timeout_rq(struct request *rq)
{
	struct blk_mq_hw_ctx *hctx = rq->mq_hctx;
	struct nullb_cmd *cmd = blk_mq_rq_to_pdu(rq);

	if (hctx->type == HCTX_TYPE_POLL) {
		struct nullb_queue *nq = hctx->driver_data;

		spin_lock(&nq->poll_lock);
		/* The request may have completed meanwhile. */
		if (blk_mq_request_completed(rq)) {
			spin_unlock(&nq->poll_lock);
			return BLK_EH_DONE;
		}
		list_del_init(&rq->queuelist);
		spin_unlock(&nq->poll_lock);
	}

	pr_info("rq %p timed out\n", rq);

	/*
	 * If the device is marked as blocking (i.e. memory backed or zoned
	 * device), the submission path may be blocked waiting for resources
	 * and cause real timeouts. For these real timeouts, the submission
	 * path will complete the request using blk_mq_complete_request().
	 * Only fake timeouts need to execute blk_mq_complete_request() here.
	 */
	cmd->error = BLK_STS_TIMEOUT;
	if (cmd->fake_timeout || hctx->type == HCTX_TYPE_POLL)
		blk_mq_complete_request(rq);
	return BLK_EH_DONE;
}

static blk_status_t null_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct request *rq = bd->rq;
	struct nullb_cmd *cmd = blk_mq_rq_to_pdu(rq);
	struct nullb_queue *nq = hctx->driver_data;
	sector_t nr_sectors = blk_rq_sectors(rq);
	sector_t sector = blk_rq_pos(rq);
	const bool is_poll = hctx->type == HCTX_TYPE_POLL;

	might_sleep_if(hctx->flags & BLK_MQ_F_BLOCKING);

	if (!is_poll && nq->dev->irqmode == NULL_IRQ_TIMER) {
		hrtimer_setup(&cmd->timer, null_cmd_timer_expired, CLOCK_MONOTONIC,
			      HRTIMER_MODE_REL);
	}
	cmd->error = BLK_STS_OK;
	cmd->nq = nq;
	cmd->fake_timeout = should_timeout_request(rq) ||
		blk_should_fake_timeout(rq->q);

	if (should_requeue_request(rq)) {
		/*
		 * Alternate between hitting the core BUSY path, and the
		 * driver driven requeue path
		 */
		nq->requeue_selection++;
		if (nq->requeue_selection & 1)
			return BLK_STS_RESOURCE;
		blk_mq_requeue_request(rq, true);
		return BLK_STS_OK;
	}

	if (test_bit(NULLB_DEV_FL_THROTTLED, &nq->dev->flags)) {
		blk_status_t sts = null_handle_throttled(cmd);

		if (sts != BLK_STS_OK)
			return sts;
	}

	blk_mq_start_request(rq);

	if (is_poll) {
		spin_lock(&nq->poll_lock);
		list_add_tail(&rq->queuelist, &nq->poll_list);
		spin_unlock(&nq->poll_lock);
		return BLK_STS_OK;
	}
	if (cmd->fake_timeout)
		return BLK_STS_OK;

	null_handle_cmd(cmd, sector, nr_sectors, req_op(rq));
	return BLK_STS_OK;
}

static void null_queue_rqs(struct rq_list *rqlist)
{
	struct rq_list requeue_list = {};
	struct blk_mq_queue_data bd = { };
	blk_status_t ret;

	do {
		struct request *rq = rq_list_pop(rqlist);

		bd.rq = rq;
		ret = null_queue_rq(rq->mq_hctx, &bd);
		if (ret != BLK_STS_OK)
			rq_list_add_tail(&requeue_list, rq);
	} while (!rq_list_empty(rqlist));

	*rqlist = requeue_list;
}

static void null_init_queue(struct nullb *nullb, struct nullb_queue *nq)
{
	nq->dev = nullb->dev;
	INIT_LIST_HEAD(&nq->poll_list);
	spin_lock_init(&nq->poll_lock);
}

static int null_init_hctx(struct blk_mq_hw_ctx *hctx, void *driver_data,
			  unsigned int hctx_idx)
{
	struct nullb *nullb = hctx->queue->queuedata;
	struct nullb_queue *nq;

	if (should_init_hctx_fail(nullb->dev))
		return -EFAULT;

	nq = &nullb->queues[hctx_idx];
	hctx->driver_data = nq;
	null_init_queue(nullb, nq);

	return 0;
}

static const struct blk_mq_ops null_mq_ops = {
	.queue_rq       = null_queue_rq,
	.queue_rqs	= null_queue_rqs,
	.complete	= null_complete_rq,
	.timeout	= null_timeout_rq,
	.poll		= null_poll,
	.map_queues	= null_map_queues,
	.init_hctx	= null_init_hctx,
};

static void null_del_dev(struct nullb *nullb)
{
	struct nullb_device *dev;

	if (!nullb)
		return;

	dev = nullb->dev;

	ida_free(&nullb_indexes, nullb->index);

	list_del_init(&nullb->list);

	del_gendisk(nullb->disk);

	if (test_bit(NULLB_DEV_FL_THROTTLED, &nullb->dev->flags)) {
		hrtimer_cancel(&nullb->bw_timer);
		atomic_long_set(&nullb->cur_bytes, LONG_MAX);
		blk_mq_start_stopped_hw_queues(nullb->q, true);
	}

	put_disk(nullb->disk);
	if (nullb->tag_set == &nullb->__tag_set)
		blk_mq_free_tag_set(nullb->tag_set);
	kfree(nullb->queues);
	if (null_cache_active(nullb))
		null_free_device_storage(nullb->dev, true);
	kfree(nullb);
	dev->nullb = NULL;
}

static void null_config_discard(struct nullb *nullb, struct queue_limits *lim)
{
	if (nullb->dev->discard == false)
		return;

	if (!nullb->dev->memory_backed) {
		nullb->dev->discard = false;
		pr_info("discard option is ignored without memory backing\n");
		return;
	}

	if (nullb->dev->zoned) {
		nullb->dev->discard = false;
		pr_info("discard option is ignored in zoned mode\n");
		return;
	}

	lim->max_hw_discard_sectors = UINT_MAX >> 9;
}

static const struct block_device_operations null_ops = {
	.owner		= THIS_MODULE,
	.report_zones	= null_report_zones,
};

static int setup_queues(struct nullb *nullb)
{
	int nqueues = nr_cpu_ids;

	if (g_poll_queues)
		nqueues += g_poll_queues;

	nullb->queues = kcalloc(nqueues, sizeof(struct nullb_queue),
				GFP_KERNEL);
	if (!nullb->queues)
		return -ENOMEM;

	return 0;
}

static int null_init_tag_set(struct blk_mq_tag_set *set, int poll_queues)
{
	set->ops = &null_mq_ops;
	set->cmd_size = sizeof(struct nullb_cmd);
	set->timeout = 5 * HZ;
	set->nr_maps = 1;
	if (poll_queues) {
		set->nr_hw_queues += poll_queues;
		set->nr_maps += 2;
	}
	return blk_mq_alloc_tag_set(set);
}

static int null_init_global_tag_set(void)
{
	int error;

	if (tag_set.ops)
		return 0;

	tag_set.nr_hw_queues = g_submit_queues;
	tag_set.queue_depth = g_hw_queue_depth;
	tag_set.numa_node = g_home_node;
	if (g_no_sched)
		tag_set.flags |= BLK_MQ_F_NO_SCHED_BY_DEFAULT;
	if (g_shared_tag_bitmap)
		tag_set.flags |= BLK_MQ_F_TAG_HCTX_SHARED;
	if (g_blocking)
		tag_set.flags |= BLK_MQ_F_BLOCKING;

	error = null_init_tag_set(&tag_set, g_poll_queues);
	if (error)
		tag_set.ops = NULL;
	return error;
}

static int null_setup_tagset(struct nullb *nullb)
{
	if (nullb->dev->shared_tags) {
		nullb->tag_set = &tag_set;
		return null_init_global_tag_set();
	}

	nullb->tag_set = &nullb->__tag_set;
	nullb->tag_set->driver_data = nullb;
	nullb->tag_set->nr_hw_queues = nullb->dev->submit_queues;
	nullb->tag_set->queue_depth = nullb->dev->hw_queue_depth;
	nullb->tag_set->numa_node = nullb->dev->home_node;
	if (nullb->dev->no_sched)
		nullb->tag_set->flags |= BLK_MQ_F_NO_SCHED_BY_DEFAULT;
	if (nullb->dev->shared_tag_bitmap)
		nullb->tag_set->flags |= BLK_MQ_F_TAG_HCTX_SHARED;
	if (nullb->dev->blocking)
		nullb->tag_set->flags |= BLK_MQ_F_BLOCKING;
	return null_init_tag_set(nullb->tag_set, nullb->dev->poll_queues);
}

static int null_validate_conf(struct nullb_device *dev)
{
	if (dev->queue_mode == NULL_Q_RQ) {
		pr_err("legacy IO path is no longer available\n");
		return -EINVAL;
	}
	if (dev->queue_mode == NULL_Q_BIO) {
		pr_err("BIO-based IO path is no longer available, using blk-mq instead.\n");
		dev->queue_mode = NULL_Q_MQ;
	}

	if (dev->use_per_node_hctx) {
		if (dev->submit_queues != nr_online_nodes)
			dev->submit_queues = nr_online_nodes;
	} else if (dev->submit_queues > nr_cpu_ids)
		dev->submit_queues = nr_cpu_ids;
	else if (dev->submit_queues == 0)
		dev->submit_queues = 1;
	dev->prev_submit_queues = dev->submit_queues;

	if (dev->poll_queues > g_poll_queues)
		dev->poll_queues = g_poll_queues;
	dev->prev_poll_queues = dev->poll_queues;
	dev->irqmode = min_t(unsigned int, dev->irqmode, NULL_IRQ_TIMER);

	/* Do memory allocation, so set blocking */
	if (dev->memory_backed)
		dev->blocking = true;
	else /* cache is meaningless */
		dev->cache_size = 0;
	dev->cache_size = min_t(unsigned long, ULONG_MAX / 1024 / 1024,
						dev->cache_size);
	dev->mbps = min_t(unsigned int, 1024 * 40, dev->mbps);

	if (dev->zoned &&
	    (!dev->zone_size || !is_power_of_2(dev->zone_size))) {
		pr_err("zone_size must be power-of-two\n");
		return -EINVAL;
	}

	return 0;
}

#ifdef CONFIG_BLK_DEV_NULL_BLK_FAULT_INJECTION
static bool __null_setup_fault(struct fault_attr *attr, char *str)
{
	if (!str[0])
		return true;

	if (!setup_fault_attr(attr, str))
		return false;

	attr->verbose = 0;
	return true;
}
#endif

static bool null_setup_fault(void)
{
#ifdef CONFIG_BLK_DEV_NULL_BLK_FAULT_INJECTION
	if (!__null_setup_fault(&null_timeout_attr, g_timeout_str))
		return false;
	if (!__null_setup_fault(&null_requeue_attr, g_requeue_str))
		return false;
	if (!__null_setup_fault(&null_init_hctx_attr, g_init_hctx_str))
		return false;
#endif
	return true;
}

static int null_add_dev(struct nullb_device *dev)
{
	struct queue_limits lim = {
		.logical_block_size	= dev->blocksize,
		.physical_block_size	= dev->blocksize,
		.max_hw_sectors		= dev->max_sectors,
	};

	struct nullb *nullb;
	int rv;

	rv = null_validate_conf(dev);
	if (rv)
		return rv;

	nullb = kzalloc_node(sizeof(*nullb), GFP_KERNEL, dev->home_node);
	if (!nullb) {
		rv = -ENOMEM;
		goto out;
	}
	nullb->dev = dev;
	dev->nullb = nullb;

	spin_lock_init(&nullb->lock);

	rv = setup_queues(nullb);
	if (rv)
		goto out_free_nullb;

	rv = null_setup_tagset(nullb);
	if (rv)
		goto out_cleanup_queues;

	if (dev->virt_boundary)
		lim.virt_boundary_mask = PAGE_SIZE - 1;
	null_config_discard(nullb, &lim);
	if (dev->zoned) {
		rv = null_init_zoned_dev(dev, &lim);
		if (rv)
			goto out_cleanup_tags;
	}

	if (dev->cache_size > 0) {
		set_bit(NULLB_DEV_FL_CACHE, &nullb->dev->flags);
		lim.features |= BLK_FEAT_WRITE_CACHE;
		if (dev->fua)
			lim.features |= BLK_FEAT_FUA;
	}

	if (dev->rotational)
		lim.features |= BLK_FEAT_ROTATIONAL;

	nullb->disk = blk_mq_alloc_disk(nullb->tag_set, &lim, nullb);
	if (IS_ERR(nullb->disk)) {
		rv = PTR_ERR(nullb->disk);
		goto out_cleanup_zone;
	}
	nullb->q = nullb->disk->queue;

	if (dev->mbps) {
		set_bit(NULLB_DEV_FL_THROTTLED, &dev->flags);
		nullb_setup_bwtimer(nullb);
	}

	nullb->q->queuedata = nullb;

	rv = ida_alloc(&nullb_indexes, GFP_KERNEL);
	if (rv < 0)
		goto out_cleanup_disk;

	nullb->index = rv;
	dev->index = rv;

	if (config_item_name(&dev->group.cg_item)) {
		/* Use configfs dir name as the device name */
		snprintf(nullb->disk_name, sizeof(nullb->disk_name),
			 "%s", config_item_name(&dev->group.cg_item));
	} else {
		sprintf(nullb->disk_name, "nullb%d", nullb->index);
	}

	set_capacity(nullb->disk,
		((sector_t)nullb->dev->size * SZ_1M) >> SECTOR_SHIFT);
	nullb->disk->major = null_major;
	nullb->disk->first_minor = nullb->index;
	nullb->disk->minors = 1;
	nullb->disk->fops = &null_ops;
	nullb->disk->private_data = nullb;
	strscpy(nullb->disk->disk_name, nullb->disk_name);

	if (nullb->dev->zoned) {
		rv = null_register_zoned_dev(nullb);
		if (rv)
			goto out_ida_free;
	}

	rv = add_disk(nullb->disk);
	if (rv)
		goto out_ida_free;

	list_add_tail(&nullb->list, &nullb_list);

	pr_info("disk %s created\n", nullb->disk_name);

	return 0;

out_ida_free:
	ida_free(&nullb_indexes, nullb->index);
out_cleanup_disk:
	put_disk(nullb->disk);
out_cleanup_zone:
	null_free_zoned_dev(dev);
out_cleanup_tags:
	if (nullb->tag_set == &nullb->__tag_set)
		blk_mq_free_tag_set(nullb->tag_set);
out_cleanup_queues:
	kfree(nullb->queues);
out_free_nullb:
	kfree(nullb);
	dev->nullb = NULL;
out:
	return rv;
}

static struct nullb *null_find_dev_by_name(const char *name)
{
	struct nullb *nullb = NULL, *nb;

	mutex_lock(&lock);
	list_for_each_entry(nb, &nullb_list, list) {
		if (strcmp(nb->disk_name, name) == 0) {
			nullb = nb;
			break;
		}
	}
	mutex_unlock(&lock);

	return nullb;
}

static int null_create_dev(void)
{
	struct nullb_device *dev;
	int ret;

	dev = null_alloc_dev();
	if (!dev)
		return -ENOMEM;

	mutex_lock(&lock);
	ret = null_add_dev(dev);
	mutex_unlock(&lock);
	if (ret) {
		null_free_dev(dev);
		return ret;
	}

	return 0;
}

static void null_destroy_dev(struct nullb *nullb)
{
	struct nullb_device *dev = nullb->dev;

	null_del_dev(nullb);
	null_free_device_storage(dev, false);
	null_free_dev(dev);
}

static int __init null_init(void)
{
	int ret = 0;
	unsigned int i;
	struct nullb *nullb;

	if (g_bs > PAGE_SIZE) {
		pr_warn("invalid block size\n");
		pr_warn("defaults block size to %lu\n", PAGE_SIZE);
		g_bs = PAGE_SIZE;
	}

	if (g_home_node != NUMA_NO_NODE && g_home_node >= nr_online_nodes) {
		pr_err("invalid home_node value\n");
		g_home_node = NUMA_NO_NODE;
	}

	if (!null_setup_fault())
		return -EINVAL;

	if (g_queue_mode == NULL_Q_RQ) {
		pr_err("legacy IO path is no longer available\n");
		return -EINVAL;
	}

	if (g_use_per_node_hctx) {
		if (g_submit_queues != nr_online_nodes) {
			pr_warn("submit_queues param is set to %u.\n",
				nr_online_nodes);
			g_submit_queues = nr_online_nodes;
		}
	} else if (g_submit_queues > nr_cpu_ids) {
		g_submit_queues = nr_cpu_ids;
	} else if (g_submit_queues <= 0) {
		g_submit_queues = 1;
	}

	config_group_init(&nullb_subsys.su_group);
	mutex_init(&nullb_subsys.su_mutex);

	ret = configfs_register_subsystem(&nullb_subsys);
	if (ret)
		return ret;

	mutex_init(&lock);

	null_major = register_blkdev(0, "nullb");
	if (null_major < 0) {
		ret = null_major;
		goto err_conf;
	}

	for (i = 0; i < nr_devices; i++) {
		ret = null_create_dev();
		if (ret)
			goto err_dev;
	}

	pr_info("module loaded\n");
	return 0;

err_dev:
	while (!list_empty(&nullb_list)) {
		nullb = list_entry(nullb_list.next, struct nullb, list);
		null_destroy_dev(nullb);
	}
	unregister_blkdev(null_major, "nullb");
err_conf:
	configfs_unregister_subsystem(&nullb_subsys);
	return ret;
}

static void __exit null_exit(void)
{
	struct nullb *nullb;

	configfs_unregister_subsystem(&nullb_subsys);

	unregister_blkdev(null_major, "nullb");

	mutex_lock(&lock);
	while (!list_empty(&nullb_list)) {
		nullb = list_entry(nullb_list.next, struct nullb, list);
		null_destroy_dev(nullb);
	}
	mutex_unlock(&lock);

	if (tag_set.ops)
		blk_mq_free_tag_set(&tag_set);

	mutex_destroy(&lock);
}

module_init(null_init);
module_exit(null_exit);

MODULE_AUTHOR("Jens Axboe <axboe@kernel.dk>");
MODULE_DESCRIPTION("multi queue aware block test driver");
MODULE_LICENSE("GPL");
