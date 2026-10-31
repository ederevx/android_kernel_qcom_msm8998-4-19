// SPDX-License-Identifier: GPL-2.0
/*
 * Operator-triggered panic for OnePlus msm8998 crash-dump diagnostics.
 *
 * Inert on load: the panic happens only when the root-only trigger parameter is
 * written. The restart that follows is the platform's own panic path, see
 * drivers/power/reset/msm-poweroff.c.
 */
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/string.h>

static bool ready;

static int trigger_set(const char *val, const struct kernel_param *kp)
{
	bool requested;
	int ret;

	if (!READ_ONCE(ready))
		return -EAGAIN;

	ret = kstrtobool(val, &requested);
	if (ret)
		return ret;
	if (!requested)
		return -EINVAL;

	panic("oneplus_panic_trigger: operator-requested panic");
}

static const struct kernel_param_ops trigger_ops = {
	.set = trigger_set,
};

module_param_cb(trigger, &trigger_ops, NULL, 0200);
MODULE_PARM_DESC(trigger, "Write 1 after loading to panic immediately");

static int __init oneplus_panic_trigger_init(void)
{
	WRITE_ONCE(ready, true);
	pr_info("oneplus_panic_trigger: ready, explicit trigger required\n");
	return 0;
}

static void __exit oneplus_panic_trigger_exit(void)
{
	WRITE_ONCE(ready, false);
}

module_init(oneplus_panic_trigger_init);
module_exit(oneplus_panic_trigger_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Operator-triggered panic for OnePlus dump diagnostics");
