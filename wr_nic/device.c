/*
 * Device initialization and cleanup for White-Rabbit switch network interface
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
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/platform_device.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/io.h>

#include "wr-nic.h"
#include "nic-mem.h"

/* The remove function is used by probe, so it's not __devexit */
static int __devexit wrn_remove(struct platform_device *pdev)
{
	struct wrn_dev *wrn = pdev->dev.platform_data;
	int i;

	spin_lock(&wrn->lock);
	--wrn->use_count; /* Hmmm... looks like overkill... */
	spin_unlock(&wrn->lock);

	/* First of all, stop any transmission */
	writel(0, &wrn->regs->CR);

	/* Then remove devices, memory maps, interrupts */
	for (i = 0; i < WRN_NR_ENDPOINTS; i++) {
		if (wrn->dev[i]) {
			wrn_endpoint_remove(wrn->dev[i]);
			free_netdev(wrn->dev[i]);
			wrn->dev[i] = NULL;
		}
	}

	for (i = 0; i < ARRAY_SIZE(wrn->bases); i++) {
		if (wrn->bases[i])
			iounmap(wrn->bases[i]);
	}

	/* Unregister all interrupts that were registered */
	for (i = 0; wrn->irq_registered; i++) {
		static int irqs[] = WRN_IRQ_NUMBERS;
		if (wrn->irq_registered & (1 << i))
			free_irq(irqs[i], wrn);
		wrn->irq_registered &= ~(1 << i);
	}
	return 0;
}

/* This helper is used by probe below */
static int __devinit __wrn_map_resources(struct platform_device *pdev)
{
	int i;
	struct resource *res;
	void __iomem *ptr;
	struct wrn_dev *wrn = pdev->dev.platform_data;

	/*
	 * The memory regions are mapped once for all endpoints.
	 * We don't populate the whole array, but use the resource list
	 */
	for (i = 0; i < pdev->num_resources; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		if (!res || !res->start)
			continue;
		ptr = ioremap(res->start, res->end + 1 - res->start);
		if (!ptr) {
			dev_err(&pdev->dev, "Remap for res %i (%08x) failed\n",
				i, res->start);
			return -ENOMEM;
		}
		/* Hack: find the block number and fill the array */
		pr_debug("Remapped %08x (block %i) to %p\n",
			 res->start, i, ptr);
		wrn->bases[i] = ptr;
	}
	return 0;
}

static int __devinit wrn_probe(struct platform_device *pdev)
{
	struct net_device *netdev;
	struct wrn_ep *ep;
	struct wrn_dev *wrn = pdev->dev.platform_data;
	int i, err = 0;

	/* Lazily: irqs are not in the resource list */
	static int irqs[] = WRN_IRQ_NUMBERS;
	static char *irq_names[] = WRN_IRQ_NAMES;
	static irq_handler_t irq_handlers[] = WRN_IRQ_HANDLERS;

	/* No need to lock_irq: we only protect count and continue unlocked */
	spin_lock(&wrn->lock);
	if (++wrn->use_count != 1) {
		--wrn->use_count;
		spin_unlock(&wrn->lock);
		return -EBUSY;
	}
	spin_unlock(&wrn->lock);

	/* Map our resource list and instantiate the shortcut pointers */
	if ( (err = __wrn_map_resources(pdev)) )
		goto out;
	wrn->regs = wrn->bases[WRN_FB_NIC];
	wrn->txtsu_regs = wrn->bases[WRN_FB_TS];
	wrn->ppsg_regs = wrn->bases[WRN_FB_PPSG];
	wrn->txd = ((void *)wrn->regs) + 0x80; /* was: TX1_D1 */
	wrn->rxd = ((void *)wrn->regs) + 0x100; /* was: RX1_D1 */
	wrn->databuf = (void *)wrn->regs + offsetof(struct NIC_WB, MEM);
	printk("regs %p, txd %p, rxd %p, buffer %p\n",
	       wrn->regs, wrn->txd, wrn->rxd, wrn->databuf);

	/* Register the interrupt handlers (not shared) */
	for (i = 0; i < ARRAY_SIZE(irq_names); i++) {
		err = request_irq(irqs[i], irq_handlers[i],
			      IRQF_TRIGGER_LOW, irq_names[i], wrn);
		if (err) goto out;
		wrn->irq_registered |= 1 << i;
	}
	/* Reset the device, just to be sure, before making anything */
	writel(0, &wrn->regs->CR);
	mdelay(10);

	/* Finally, register one interface per endpoint */
	memset(wrn->dev, 0, sizeof(wrn->dev));
	for (i = 0; i < WRN_NR_ENDPOINTS; i++) {
		netdev = alloc_etherdev(sizeof(struct wrn_ep));
		if (!netdev) {
			dev_err(&pdev->dev, "Etherdev alloc failed.\n");
			err = -ENOMEM;
			goto out;
		}
		/* The ep structure is filled before calling ep_probe */
		ep = netdev_priv(netdev);
		ep->wrn = wrn;
		ep->ep_regs = wrn->bases[WRN_FB_EP] + i * FPGA_SIZE_EACH_EP;
		printk("ep %p, regs %i = %p\n", ep, i, ep->ep_regs);
		ep->ep_number = i;
#if 0 /* FIXME: UPlink or not? */
		if (i < WRN_NR_UPLINK)
			set_bit(WRN_EP_IS_UPLINK, &ep->ep_flags);
#endif

		/* The netdevice thing is registered from the endpoint */
		err = wrn_endpoint_probe(netdev);
		if (err == -ENODEV)
			break;
		if (err)
			goto out;
		/* This endpoint went in properly */
		wrn->dev[i] = netdev;
	}

	for (i = 0; i < WRN_NR_TXDESC; i++) { /* Clear all tx descriptors */
		struct wrn_txd *tx;
		tx = wrn->txd + i;
		writel(0, &tx->tx1);
	}

	/* Now, prepare RX descriptors */
	for (i = 0; i < WRN_NR_RXDESC; i++) {
		struct wrn_rxd *rx;
		int offset;

		rx = wrn->rxd + i;
		offset = __wrn_desc_offset(wrn, WRN_DDIR_RX, i);
		writel( (2000 << 16) | offset, &rx->rx3);
		writel(NIC_RX1_D1_EMPTY, &rx->rx1);
	}

	/*
	 * make sure all head/tail are 0 -- not needed here, but if we
	 * disable and then re-enable, this _is_ needed
	 */
	wrn->next_tx_head = wrn->next_tx_tail = wrn->next_rx = 0;

	writel(NIC_CR_RX_EN | NIC_CR_TX_EN, &wrn->regs->CR);
	writel(~0, (void *)wrn->regs + 0x24 /* EIC_IER */);
	printk("imr: %08x\n", readl((void *)wrn->regs + 0x28 /* EIC_IMR */));

	wrn_tstamp_init(wrn);
	err = 0;
out:
	if (err) {
		/* Call the remove function to avoid duplicating code */
		wrn_remove(pdev);
	} else {
		dev_info(&pdev->dev, "White Rabbit NIC driver\n");
	}
	return err;
}

/* This is not static as ./module.c is going to register it */
struct platform_driver wrn_driver = {
	.probe		= wrn_probe,
	.remove		= wrn_remove, /* not __exit_p as probe calls it */
	/* No suspend or resume by now */
	.driver		= {
		.name		= DRV_NAME,
		.owner		= THIS_MODULE,
	},
};
