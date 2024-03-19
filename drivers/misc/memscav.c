// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt) "memscav: " fmt

#include <linux/efi.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/memory.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/printk.h>
#include <linux/range.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/sysfs.h>

static struct kobject *memscav_kobj;
static bool __read_mostly unloadable = true;

struct hidden_block {
	struct list_head l;
	unsigned long long phys;
};
static LIST_HEAD(hidden_blocks);
static unsigned int hidden_blocks_cnt;

static unsigned int ranges_len;
static struct range *ranges;

static int hidden_block_add(unsigned long long phys)
{
	struct hidden_block *b;

	b = kmalloc(sizeof (*b), GFP_KERNEL);
	if (!b)
		return -ENOMEM;

	b->phys = phys;
	list_add_tail(&b->l, &hidden_blocks);
	++hidden_blocks_cnt;

	return 0;
}

static void hidden_block_free(struct hidden_block *b)
{
	list_del(&b->l);
	kfree(b);
	--hidden_blocks_cnt;
}

static void hidden_blocks_purge(void)
{
	struct hidden_block *b;

	while ((b = list_first_entry_or_null(&hidden_blocks, typeof (*b), l)))
		hidden_block_free(b);

	WARN_ON(hidden_blocks_cnt);
}

static int ram_map_add(u64 base, u64 size)
{
	struct range *r;

	r = krealloc(ranges, sizeof (*ranges) * (ranges_len + 1), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	ranges = r;
	ranges[ranges_len].start = base;
	ranges[ranges_len].end = base + size - 1;
	++ranges_len;

	return 0;
}

static void ram_map_free(void)
{
	kfree(ranges);
	ranges = NULL;
	ranges_len = 0;
}

static inline void disable_unload(void)
{
	if (!unloadable)
		return;

	if (!try_module_get(THIS_MODULE)) {
		pr_warn("Could not prevent unloading. This module will be disabled.\n");
		return;
	}

	unloadable = false;
}

#define MEMSCAV_ATTR_RO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_RO(_name)
#define MEMSCAV_ATTR_WO(_name) \
	static struct kobj_attribute _name##_attr = __ATTR_WO(_name)

static ssize_t ranges_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	size_t count = 0;
	unsigned int i;

	for (i = 0; PAGE_SIZE > count && i < ranges_len; ++i)
		count += sysfs_emit_at(buf, count, "%#llx-%#llx (%#llx)\n",
				       ranges[i].start, ranges[i].end,
				       range_len(&ranges[i]));

	return count;
}
MEMSCAV_ATTR_RO(ranges);

#ifdef CONFIG_MEMSCAV_DEBUG
static ssize_t probe_store(struct kobject *kobj, struct kobj_attribute *attr,
			   const char *buf, size_t count)
{
	u64 phys_addr;
	int nid, rc;

	rc = kstrtoull(buf, 0, &phys_addr);
	if (rc)
		return rc;

	/* memory_block_size_bytes() is guaranteed to be ^2. */
	if (!IS_ALIGNED(phys_addr, memory_block_size_bytes()))
		return -EINVAL;

	nid = memory_add_physaddr_to_nid(phys_addr);
	rc = add_memory_driver_managed(nid, phys_addr,
				       memory_block_size_bytes(),
				       "System RAM (memscav)",
				       MHP_MERGE_RESOURCE);
	if (rc)
		return rc;

	/* One way ticket for now. */
	disable_unload();

	return count;
}
MEMSCAV_ATTR_WO(probe);
#endif /* CONFIG_MEMSCAV_DEBUG */

/* Not exported, see linux/range.h */
static int cmp_range(const void *x1, const void *x2)
{
	const struct range *r1 = x1;
	const struct range *r2 = x2;

	if (r1->start < r2->start)
		return -1;
	if (r1->start > r2->start)
		return 1;
	return 0;
}

#ifdef CONFIG_OF
/*
 * of_property_read_reg() is not yet backported in CS9.
 * Introduced with v6.3-rc7
 */
int __weak of_property_read_reg(struct device_node *np, int idx, u64 *addr,
				u64 *size)
{
	const __be32 *prop = of_get_address(np, idx, size, NULL);

	if (!prop)
		return -EINVAL;

	*addr = of_read_number(prop, of_n_addr_cells(np));

	return 0;
}

static int ram_map_from_fdt(void)
{
	struct device_node *np = NULL;
	u64 address, size;
	unsigned int i;
	int rc;

	/* Find all "reg" ranges from all "/memory" nodes. */
	while ((np = of_find_node_by_type(np, "memory")))
		for (i = 0; !of_property_read_reg(np, i, &address, &size); ++i) {
			rc = ram_map_add(address, size);
			if (rc) {
				ram_map_free();
				return rc;
			}
		}

	/* The device-tree has no order requirement. */
	/* sort_range() from linux/ranges.h, not exported. */
	sort(ranges, ranges_len, sizeof(struct range), cmp_range, NULL);

	return 0;
}
#endif /* CONFIG_OF */

static int ram_map_from_efi(void)
{
	efi_memory_desc_t *md;
	int rc;

	if (WARN_ON_ONCE(!efi_enabled(EFI_MEMMAP)))
		return -EINVAL;

	for_each_efi_memory_desc(md) {
		if (md->type != EFI_CONVENTIONAL_MEMORY)
			continue;

		rc = ram_map_add(md->phys_addr, md->num_pages << EFI_PAGE_SHIFT);
		if (rc) {
			ram_map_free();
			return rc;
		}
	}

	/* efi.memmap has no order requirement. */
	/* sort_range() from linux/ranges.h, not exported. */
	sort(ranges, ranges_len, sizeof(struct range), cmp_range, NULL);

	return 0;
}

static int find_hidden_blocks(void)
{
	unsigned long block_sz = memory_block_size_bytes();
	unsigned int i;
	int rc;

	for (i = 0; i < ranges_len; ++i) {
		unsigned long long start = ALIGN(ranges[i].start, block_sz);

		for (; start + block_sz < ranges[i].end; start += block_sz) {
			if (region_intersects(start, block_sz, IORESOURCE_MEM,
					      IORES_DESC_NONE) != REGION_DISJOINT)
				continue;
			rc = hidden_block_add(start);
			if (rc)
				goto out;
		}
	}

	return 0;

out:
	hidden_blocks_purge();
	return rc;
}

static ssize_t hidden_blocks_show(struct kobject *kobj, struct kobj_attribute *attr, char *buf)
{
	unsigned long block_sz = memory_block_size_bytes();
	unsigned long long start, size = 0;
	struct hidden_block *b;
	size_t count = 0;

	list_for_each_entry(b, &hidden_blocks, l) {
		if (count >= PAGE_SIZE)
			break;
		if (!size) {
			/* first iteration. */
			start = b->phys;
			size = block_sz;
		} else if ((start + size) == b->phys) {
			/* contiguous blocks. */
			size += block_sz;
		} else {
			/* end of contiguous blocks. */
			count += sysfs_emit_at(buf, count,
					       "%#llx-%#llx (%#llx)\n", start,
					       start + size - 1, size);
			start = b->phys;
			size = block_sz;
		}
	}
	/* end of last contiguous blocks, if any. */
	if (size && count < PAGE_SIZE)
		count += sysfs_emit_at(buf, count, "%#llx-%#llx (%#llx)\n",
				       start, start + size - 1, size);

	return count;
}
MEMSCAV_ATTR_RO(hidden_blocks);

static ssize_t scavenge_store(struct kobject *kobj,
			      struct kobj_attribute *attr,
			      const char *buf, size_t count)
{
	unsigned long block_sz = memory_block_size_bytes();
	unsigned long long size, total;
	struct hidden_block *b;
	int nid, rc;

	rc = kstrtoull(buf, 0, &size);
	if (rc)
		return rc;

	/* Only add entire memory blocks. */
	if (!size || size % block_sz)
		return -EINVAL;

	for (total = size; size >= block_sz; size -= block_sz) {
		b = list_first_entry_or_null(&hidden_blocks, typeof (*b), l);
		if (!b)
			break;

		nid = memory_add_physaddr_to_nid(b->phys);
		rc = add_memory_driver_managed(nid, b->phys, block_sz,
					       "System RAM (memscav)",
					       MHP_MERGE_RESOURCE);
		if (rc)
			pr_warn("Failed to recover %#llx-%#llx, removing.\n",
				b->phys, b->phys + block_sz - 1);

		list_del(&b->l);
	}

	/* One way ticket for now. */
	if (total > size)
		disable_unload();

	return count;
}
MEMSCAV_ATTR_WO(scavenge);

static struct attribute *memscav_sysfs_entries[] = {
#ifdef CONFIG_MEMSCAV_DEBUG
	&probe_attr.attr,
#endif /* CONFIG_MEMSCAV_DEBUG */
	&ranges_attr.attr,
	&scavenge_attr.attr,
	&hidden_blocks_attr.attr,
	NULL
};

static const struct attribute_group memscav_attribute_group = {
	.attrs = memscav_sysfs_entries,
};

static int __init memscav_init(void)
{
	int rc;

	memscav_kobj = kobject_create_and_add("memscav", kernel_kobj);
	if (!memscav_kobj)
		return -ENOMEM;

	if (efi_enabled(EFI_MEMMAP)) {
		rc = ram_map_from_efi();
		if (rc) {
			pr_err("Failed to read efi.memmap entries (%d).\n", rc);
			goto out;
		}
	}

#ifdef CONFIG_OF
	if (!efi_enabled(EFI_MEMMAP)) {
		rc = ram_map_from_fdt();
		if (rc) {
			pr_err("Failed to parse device-tree /memory entries (%d).\n", rc);
			goto out;
		}
	}
#endif /* CONFIG_OF */

	rc = find_hidden_blocks();
	if (rc) {
		pr_err("Failed to search for hidden memory blocks (%d).\n", rc);
		goto out;
	}

	rc = sysfs_create_group(memscav_kobj, &memscav_attribute_group);
	if (rc) {
		pr_err("Failed to create sysfs attributes (%d).", rc);
		goto out;
	}

	return 0;

out:
	ram_map_free();
	kobject_put(memscav_kobj);
	return rc;
}
module_init(memscav_init);

static void __exit memscav_exit(void)
{
	sysfs_remove_group(memscav_kobj, &memscav_attribute_group);
	ram_map_free();
	kobject_put(memscav_kobj);
}
module_exit(memscav_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Scavenger of memory hidden from the kernel.");
