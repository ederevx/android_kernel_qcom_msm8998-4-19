// SPDX-License-Identifier: GPL-2.0-only
/*
 * OnePlus project information sysfs interface
 */

#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>
#include <linux/types.h>

#include <linux/soc/qcom/smem.h>

#define SMEM_ONEPLUS_PROJECT_INFO	136

struct oneplus_project_info {
	char project_name[8];
	u32 hw_version;
};

static u32 oneplus_hw_id;

static ssize_t hw_id_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", oneplus_hw_id);
}

static struct kobj_attribute hw_id_attr = __ATTR_RO(hw_id);

static int __init oneplus_project_info_init(void)
{
	const struct oneplus_project_info *info;
	struct kobject *project_info_kobj;
	size_t size;
	int ret;

	info = qcom_smem_get(QCOM_SMEM_HOST_ANY,
			     SMEM_ONEPLUS_PROJECT_INFO, &size);
	if (IS_ERR(info)) {
		pr_err("oneplus-project-info: failed to read SMEM item: %ld\n",
		       PTR_ERR(info));
		return PTR_ERR(info);
	}

	if (size < sizeof(*info)) {
		pr_err("oneplus-project-info: invalid SMEM item size: %zu\n",
		       size);
		return -EINVAL;
	}

	oneplus_hw_id = info->hw_version;
	project_info_kobj = kobject_create_and_add("project_info", NULL);
	if (!project_info_kobj)
		return -ENOMEM;

	ret = sysfs_create_file(project_info_kobj, &hw_id_attr.attr);
	if (ret) {
		pr_err("oneplus-project-info: failed to create hw_id: %d\n",
		       ret);
		kobject_put(project_info_kobj);
		return ret;
	}

	pr_info("oneplus-project-info: hardware ID %u\n", oneplus_hw_id);

	return 0;
}
subsys_initcall(oneplus_project_info_init);
