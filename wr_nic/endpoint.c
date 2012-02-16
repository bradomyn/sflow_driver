/*
 * Endoint-specific operations in the White-Rabbit switch network interface
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
#include <linux/errno.h>
#include <linux/etherdevice.h>
#include <linux/io.h>

#include "wr-nic.h"

/*
 * Phy access: used by link status, enable, calibration ioctl etc.
 * Called with endpoint lock (you'll lock the whole sequence of r/w)
 */
int wrn_phy_read(struct net_device *dev, int phy_id, int location)
{
	struct wrn_ep *ep = netdev_priv(dev);
	u32 val;

	wrn_ep_write(ep, MDIO_CR, EP_MDIO_CR_ADDR_W(location));
	while( (wrn_ep_read(ep, MDIO_ASR) & EP_MDIO_ASR_READY) == 0)
		;
	val = wrn_ep_read(ep, MDIO_ASR);
	/* mask from wbgen macros */
	return EP_MDIO_ASR_RDATA_R(val);
}

void wrn_phy_write(struct net_device *dev, int phy_id, int location,
		      int value)
{
	struct wrn_ep *ep = netdev_priv(dev);
	wrn_ep_write(ep, MDIO_CR,
		     EP_MDIO_CR_ADDR_W(location)
		     | EP_MDIO_CR_DATA_W(value)
		     | EP_MDIO_CR_RW);
	while( (wrn_ep_read(ep, MDIO_ASR) & EP_MDIO_ASR_READY) == 0)
		;
}

/* One link status poll per endpoint -- called with endpoint lock */
static void wrn_update_link_status(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);
	u32 ecr, bmsr, bmcr, lpa;

	bmsr = wrn_phy_read(dev, 0, MII_BMSR);
	bmcr = wrn_phy_read(dev, 0, MII_BMCR);

	//netdev_dbg(dev, "%s: read %x %x", __func__, bmsr, bmcr);
//	printk("%s: read %x %x %x\n", __func__, bmsr, bmcr);

		/* Link wnt down? */
	if (!mii_link_ok(&ep->mii)) {	
		if(netif_carrier_ok(dev)) {
			netif_carrier_off(dev);
			clear_bit(WRN_EP_UP, &ep->ep_flags);
			printk(KERN_INFO "%s: Link down.\n", dev->name);
			return;
		}
		return;
	}

	/* Currently the link is active */
	if(netif_carrier_ok(dev)) {
		/* Software already knows it's up */
		return;
	}

	/* What follows is the bring-up step */

	if (bmcr & BMCR_ANENABLE) { /* AutoNegotiation is enabled */
		if (!(bmsr & BMSR_ANEGCOMPLETE)) {
			/* Wait next timer, until it completes */
			return;
		}

		lpa  = wrn_phy_read(dev, 0, MII_LPA);

		if (0) { /* was commented in minic */
			wrn_ep_write(ep, FCR,
				     EP_FCR_TXPAUSE |EP_FCR_RXPAUSE
				     | EP_FCR_TX_THR_W(128)
				     | EP_FCR_TX_QUANTA_W(200));
		}

		printk(KERN_INFO "%s: Link up, lpa 0x%04x.\n",
		       dev->name, lpa);
	} else {
		/* No autonegotiation. It's up immediately */
		printk(KERN_INFO "%s: Link up.\n", dev->name);
	}
	netif_carrier_on(dev);
	set_bit(WRN_EP_UP, &ep->ep_flags);

	/* reset RMON counters */
	ecr = wrn_ep_read(ep, ECR);
	wrn_ep_write(ep, ECR, ecr | EP_ECR_RST_CNT);
	wrn_ep_write(ep, ECR, ecr );
}

/* Actual timer function. Takes the lock and calls above function */
static void wrn_ep_check_link(unsigned long dev_id)
{
	struct net_device *dev = (struct net_device *) dev_id;
	struct wrn_ep *ep = netdev_priv(dev);
	unsigned long flags;

	spin_lock_irqsave(&ep->lock, flags);
	wrn_update_link_status(dev);
	spin_unlock_irqrestore(&ep->lock, flags);

	mod_timer(&ep->ep_link_timer, jiffies + WRN_LINK_POLL_INTERVAL);
}

/* Endpoint open and close turn on and off the timer */
int wrn_ep_open(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);
	unsigned long timerarg = (unsigned long)dev;

	/* Prepare hardware registers: first config, then bring up */
	writel(0
	       | EP_VCR0_QMODE_W(0x3)		/* unqualified port */
	       | EP_VCR0_PRIO_VAL_W(4),		/* some mid priority */
		&ep->ep_regs->VCR0);

	/*
	 * enable RX timestamping (it has no impact on performance)
	 * and we need the RX OOB block to identify orginating endpoints
	 * for RXed packets -- Tom
	 */
	writel(EP_TSCR_EN_TXTS| EP_TSCR_EN_RXTS, &ep->ep_regs->TSCR);

	writel(0
	       | EP_ECR_PORTID_W(ep->ep_number)
	       | EP_ECR_RST_CNT
	       | EP_ECR_TX_EN
	       | EP_ECR_RX_EN,
		&ep->ep_regs->ECR);

	/* Setup DMCR */
	writel(0
	       | EP_DMCR_EN
	       | EP_DMCR_N_AVG_W(256 /* DMTD_AVG_SAMPLES */),
	       &ep->ep_regs->DMCR);

	wrn_phy_write(dev, 0, MII_LPA, 0);
	wrn_phy_write(dev, 0, MII_BMCR, BMCR_ANENABLE | BMCR_ANRESTART);

	/* Prepare the timer for link-up notifications */
	setup_timer(&ep->ep_link_timer, wrn_ep_check_link, timerarg);
	mod_timer(&ep->ep_link_timer, jiffies + WRN_LINK_POLL_INTERVAL);
	return 0;
}

int wrn_ep_close(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);

	writel(0, &ep->ep_regs->ECR);
	del_timer_sync(&ep->ep_link_timer);
	return 0;
}

/*
 * The probe functions brings up the endpoint and the logical ethernet
 * device within Linux. The actual network operations are in nic-core.c,
 * as they are not endpoint-specific, while the mii stuff is in this file.
 * The initial helper here shuts down everything, in case of failure or close
 */
static void __wrn_endpoint_shutdown(struct wrn_ep *ep)
{
	/* Not much to do it seems */
	writel(0, &ep->ep_regs->ECR); /* disable it all */
}

int wrn_endpoint_probe(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);
	int epnum, err;
	u32 val;

	epnum = ep->ep_number;

	/* Check whether the ep has been sinthetized or not */
	val = readl(&ep->ep_regs->IDCODE);
	if (val != WRN_EP_MAGIC) {
		pr_info(DRV_NAME "EP%i (%s) has not been sintethized\n",
			ep->ep_number, dev->name);
		return -ENODEV;
	}
	/* Errors different from -ENODEV are fatal to insmod */

	dev_alloc_name(dev, "wr%d");
	wrn_netops_init(dev); /* function in ./nic-core.c */
	wrn_ethtool_init(dev); /* function in ./ethtool.c */
	/* Napi is not supported on this device */

	ep->mii.dev = dev;		/* Support for ethtool */
	ep->mii.mdio_read = wrn_phy_read;
	ep->mii.mdio_write = wrn_phy_write;
	ep->mii.phy_id = 0;
	ep->mii.phy_id_mask = 0x1f;
	ep->mii.reg_num_mask = 0x1f;

	ep->mii.force_media = 0;
	ep->mii.advertising = ADVERTISE_1000XFULL;
	ep->mii.full_duplex = 1;

	/* Finally, register and succeed, or fail and undo */
	err = register_netdev(dev);
	if (err) {
		printk(KERN_ERR DRV_NAME "Can't register dev %s\n",
		       dev->name);
		__wrn_endpoint_shutdown(ep);
		/* ENODEV means "no more" for the caller, so avoid it */
		return err == -ENODEV ? -EIO : err;
	}

	/* randomize a MAC address, so lazy users can avoid ifconfig */
	random_ether_addr(dev->dev_addr);

	return 0;
}

/* Called for each endpoint, with a valid ep structure. The caller frees */
void wrn_endpoint_remove(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);

	unregister_netdev(dev);
	__wrn_endpoint_shutdown(ep);
}
