/*
 * Module-related material for wr-nic: load and unload
 *
 * Copyright (C) 2010 CERN (www.cern.ch)
 * Author: Alessandro Rubini <rubini@gnudd.com>
 * Partly from previous work by Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 * Partly from previous work by  Emilio G. Cota <cota@braap.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/platform_device.h>

#include "wr-nic.h"

/* Our platform data is actually the device itself, and we have 1 only */
static struct wrn_dev wrn_dev;

/* The WRN_RES_ names are defined in the header file. Each block 64kB */
#define __RES(name_) {						\
	.start = FPGA_BASE_ ## name_,				\
	.end =   FPGA_BASE_ ## name_ + FPGA_SIZE_ ## name_ - 1,	\
	.flags = IORESOURCE_MEM				\
	}

/* Not all the blocks are relevant to this driver, only list the used ones */
static struct resource wrn_resources[] = {
	[WRN_FB_NIC] = __RES( NIC ),
	[WRN_FB_EP] = __RES( EP ),
	[WRN_FB_TS] = __RES( TS ),
	[WRN_FB_PPSG] = __RES( PPSG ),
};
#undef __RES

static void wrn_release(struct device *dev)
{
	/* nothing to do, but mandatory function */
	pr_debug("%s\n", __func__);
}

static struct platform_device wrn_device = {
	.name = DRV_NAME,
	.id = 0,
	.resource = wrn_resources,
	.num_resources = ARRAY_SIZE(wrn_resources),
	.dev = {
		.platform_data = &wrn_dev,
		.release = &wrn_release,
		/* dma_mask not used, as we make no DMA */
	},
};

/*
 * Module init and exit stuff. Here we register the platform data
 * as well, but the driver itself is in device.c
 */
int __init wrn_init(void)
{
	/* The clock period must be a multiple of 1ns, so bug out otherwise */
	BUILD_BUG_ON(REFCLK_FREQ * NSEC_PER_TICK != NSEC_PER_SEC);

	/* A few fields must be initialized at run time */
	spin_lock_init(&wrn_dev.lock);

	platform_device_register(&wrn_device);
	platform_driver_register(&wrn_driver);
	return 0;
}

void __exit wrn_exit(void)
{
	platform_driver_unregister(&wrn_driver);
	platform_device_unregister(&wrn_device);
	return;
}

module_init(wrn_init);
module_exit(wrn_exit);

MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:wr-nic");
