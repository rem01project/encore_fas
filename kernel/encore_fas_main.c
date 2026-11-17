// SPDX-License-Identifier: GPL-2.0-only

#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>

#include "fas_lkm_backend.h"

#ifndef FAS_VERSION
    #define FAS_VERSION 0
#endif
#ifndef FAS_BRANCH
    #define FAS_BRANCH "-"
#endif

static struct miscdevice fas_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "encore_fas",
    .fops = &fas_device_fops,
    .mode = 0600,
};

static int __init fas_init(void) {
    int ret;

    BUILD_BUG_ON(sizeof(struct fas_event) != 64);
    BUILD_BUG_ON(offsetof(struct fas_event, type) != 56);
    BUILD_BUG_ON(sizeof(struct fas_stash_slot) != 32);
    BUILD_BUG_ON(FAS_RING_RECORDS & FAS_RING_MASK);
    BUILD_BUG_ON((1u << FAS_STASH_SHIFT) != FAS_STASH_SLOTS);

    ret = misc_register(&fas_miscdev);
    if (ret)
        return ret;

    pr_info("encore_fas: version %u, branch %s, ABI %u, ring %u records\n",
            FAS_VERSION,
            FAS_BRANCH,
            FAS_ABI_VERSION,
            FAS_RING_RECORDS);
    return 0;
}

static void __exit fas_exit(void) {
    misc_deregister(&fas_miscdev);
}

module_init(fas_init);
module_exit(fas_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Encore Frame Aware Scheduling LKM");
