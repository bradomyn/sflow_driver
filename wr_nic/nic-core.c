/*
 * Core file for White-Rabbit switch network interface
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
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/errno.h>
#include <linux/spinlock.h>
#include <linux/net_tstamp.h>
#include <asm/unaligned.h>

#include "wr-nic.h"
#include "nic-mem.h"

/*
 * The following functions are the standard network device operations.
 * They act on the _endpoint_ (as each Linux interface is one endpoint)
 * so sometimes a call to something within ./endpoint.c is performed.
 */
static int wrn_open(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);
	u32 val;

	/* This is "open" just for an endpoint. The nic hw is already on */
	//netdev_dbg(dev, "%s\n", __func__);

	if (!is_valid_ether_addr(dev->dev_addr))
		return -EADDRNOTAVAIL;

	/* Mark it as down, and start the ep-specific polling timer */
	clear_bit(WRN_EP_UP, &ep->ep_flags);
	wrn_ep_open(dev);

	/* Software-only management is in this file*/
	if (netif_queue_stopped(dev)) {
		netif_wake_queue(dev);
	} else {
		netif_start_queue(dev);
	}

	/* Set the MRU equal to the MTU */
	val = readl(&ep->ep_regs->RFCR) & ~EP_RFCR_MRU_MASK;
	writel (val | EP_RFCR_MRU_W(dev->mtu), &ep->ep_regs->RFCR);

	/* Most drivers call platform_set_drvdata() but we don't need it */
	return 0;
}

static int wrn_close(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);
	int ret;

	if ( (ret = wrn_ep_close(dev)) )
		return ret;

	/* FIXME: software-only fixing at close time */
	netif_stop_queue(dev);
	netif_carrier_off(dev);
	clear_bit(WRN_EP_UP, &ep->ep_flags);
	return 0;
}

static int wrn_set_mac_address(struct net_device *dev, void* vaddr)
{
	struct wrn_ep *ep = netdev_priv(dev);
	struct sockaddr *addr = vaddr;
	u32 val;

	//netdev_dbg(dev, "%s\n", __func__);

	if (!is_valid_ether_addr(addr->sa_data)) {
		//netdev_dbg(dev, "%s: invalid\n", __func__);
		return -EADDRNOTAVAIL;
	}

	memcpy(dev->dev_addr, addr->sa_data, dev->addr_len);

	/* MACH gets the first two bytes, MACL the rest  */
	val = get_unaligned_be16(dev->dev_addr);
	writel(val, &ep->ep_regs->MACH);
	val = get_unaligned_be32(dev->dev_addr+2);
	writel(val, &ep->ep_regs->MACL);
	return 0;
}

/* Next descriptor */
static int __wrn_next_desc(int i)
{
	return (i+1) % WRN_NR_DESC;
}

/* This is called with the lock taken */
static int __wrn_alloc_tx_desc(struct wrn_dev *wrn)
{
	int ret = wrn->next_tx_head;
	struct wrn_txd __iomem *tx;

	tx = wrn->txd + ret;

	/* Check if it's available */
	if (readl(&tx->tx1) & NIC_TX1_D1_READY) {
		pr_debug("%s: not free %i\n", __func__, ret);
		return -ENOMEM;
	}
	wrn->next_tx_head = __wrn_next_desc(ret);
	return ret;
}

/* Actual transmission over a single endpoint */
static void __wrn_tx_desc(struct wrn_ep *ep, int desc,
			  void *data, int len, int id, int do_stamp)
{
	struct wrn_dev *wrn = ep->wrn;
	int offset = __wrn_desc_offset(wrn, WRN_DDIR_TX, desc);
	u32 *ptr = __wrn_desc_mem(wrn, WRN_DDIR_TX, desc);
	struct wrn_txd __iomem *tx = wrn->txd + desc;

	/* data */
	pr_debug("%s: %i -- data %p, len %i ", __func__, __LINE__,
	       data, len);
	pr_debug("-- desc %i (tx %p)\n", desc, tx);

	__wrn_copy_out(ptr, data, len);

	/* TX register 3: mask of endpoints (FIXME: broadcast) */
	//printk("EP Num: %d\n", ep->ep_number);

	writel(1<<ep->ep_number, &tx->tx3);

	/* TX register 2: offset and length */
	writel(offset | (len << 16), &tx->tx2);

	/* TX register 1: id and masks -- and tx_enable if needed */
	writel((len < 60 ? NIC_TX1_D1_PAD_E : 0) | NIC_TX1_D1_READY
	       | (do_stamp ? NIC_TX1_D1_TS_E : 0) | (id << 16),
	       &tx->tx1);
}


static int wrn_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);
	struct wrn_dev *wrn = ep->wrn;
	struct skb_shared_info *info = skb_shinfo(skb);
	unsigned long flags;
	int desc;
	int id;
	int do_stamp = 0;
	void *data; /* FIXME: move data and len to __wrn_tx_desc */
	unsigned len;

	if (unlikely(skb->len > WRN_MTU)) {
		/* FIXME: check this WRN_MTU is needed and used properly */
		ep->stats.tx_errors++;
		return -EMSGSIZE;
	}

	/* Allocate a descriptor and id (start from last allocated) */
	spin_lock_irqsave(&wrn->lock, flags);
	desc = __wrn_alloc_tx_desc(wrn);
	id = (wrn->id++) & 0xffff;
	spin_unlock_irqrestore(&wrn->lock, flags);

	if (desc < 0) /* error */
		return desc;

	data = skb->data;
	len = skb->len;

	//spin_lock_irqsave(&ep->lock, flags);

	if (wrn->skb_desc[desc].skb) {
		pr_err("%s: descriptor overflow: tx timestamp pending\n",
			__func__);
	}
	wrn->skb_desc[desc].skb = skb; /* Save for tx irq and stamping */
	wrn->skb_desc[desc].id = id; /* Save for tx irq and stamping */

	//netif_stop_queue(dev); /* Queue stopped until tx is over (FIXME?) */

	/* FIXME: check the WRN_EP_STAMPING_TX flag and its meaning */
	if (info->tx_flags & SKBTX_HW_TSTAMP) {
		/* hardware timestamping is enabled */
		do_stamp = 1;
	}

	/* This both copies the data to the descriptr and fires tx */
	__wrn_tx_desc(ep, desc, data, len, id, do_stamp);

	/* We are done, this is trivial maiintainance*/
	ep->stats.tx_packets++;
	ep->stats.tx_bytes += len;
	dev->trans_start = jiffies;

	//spin_unlock_irqrestore(&ep->lock, flags);
	return 0;
}

struct net_device_stats *wrn_get_stats(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);

	/* FIXME: we should get the RMON information from endpoint */
	return &ep->stats;
	return NULL;
}

static int wrn_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct wrn_ep *ep = netdev_priv(dev);
	int res;
	u32 reg;

	switch (cmd) {
	case SIOCSHWTSTAMP:
		return wrn_tstamp_ioctl(dev, rq, cmd);
	case PRIV_IOCGCALIBRATE:
		return wrn_calib_ioctl(dev, rq, cmd);
	case PRIV_IOCGGETPHASE:
		return wrn_phase_ioctl(dev, rq, cmd);
	case PRIV_IOCREADREG:
		if (get_user(reg, (u32 *)rq->ifr_data) < 0)
			return -EFAULT;
		if (reg > sizeof(struct EP_WB) || reg & 3)
			return -EINVAL;
		reg = readl((void *)ep->ep_regs + reg);
		if (put_user(reg, (u32 *)rq->ifr_data) < 0)
			return -EFAULT;
		return 0;
	case PRIV_IOCPHYREG:
		/* this command allows to read and write a phy register */
		if (get_user(reg, (u32 *)rq->ifr_data) < 0)
			return -EFAULT;
		if (reg & (1<<31)) {
			wrn_phy_write(dev, 0, (reg >> 16) & 0xff,
				      reg & 0xffff);
			return 0;
		}
		reg = wrn_phy_read(dev, 0, (reg >> 16) & 0xff);
		if (put_user(reg, (u32 *)rq->ifr_data) < 0)
			return -EFAULT;
		return 0;

	default:
		spin_lock_irq(&ep->lock);
		res = generic_mii_ioctl(&ep->mii, if_mii(rq), cmd, NULL);
		spin_unlock_irq(&ep->lock);
		return res;
	}
}

static const struct net_device_ops wrn_netdev_ops = {
	.ndo_open		= wrn_open,
	.ndo_stop		= wrn_close,
	.ndo_start_xmit		= wrn_start_xmit,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_get_stats		= wrn_get_stats,
	.ndo_set_mac_address	= wrn_set_mac_address,
	.ndo_do_ioctl		= wrn_ioctl,
#if 0
	/* Missing ops, possibly to add later */
	.ndo_set_multicast_list	= wrn_set_multicast_list,
	.ndo_change_mtu		= wrn_change_mtu,
	/* There are several more, but not really useful for us */
#endif
};


int wrn_netops_init(struct net_device *dev)
{
	dev->netdev_ops = &wrn_netdev_ops;
	return 0;
}

/*
 * From this onwards, it's all about interrupt management
 */
static void __wrn_rx_descriptor(struct wrn_dev *wrn, int desc)
{
	struct net_device *dev;
	struct wrn_ep *ep;
	struct sk_buff *skb;
	struct wrn_rxd __iomem *rx;
	u32 r1, r2, r3, offset;
	int epnum, off, len;
	u32 ts_r, ts_f;
	struct skb_shared_hwtstamps *hwts;
	struct timespec ts;
	u32 counter_ppsg; /* PPS generator nanosecond counter */
	u32 utc;
	s32 cntr_diff;

	rx = wrn->rxd + desc;
	r1 = readl(&rx->rx1);
	r2 = readl(&rx->rx2);
	r3 = readl(&rx->rx3);
	pr_debug("%s: %i: %08x %08x %08x\n", __func__, desc, r1, r2, r3);

	/* So, this descriptor is not empty. Get the port (ep) */

	offset = __wrn_desc_offset(wrn, WRN_DDIR_RX, desc);


	if (r1 & NIC_RX1_D1_GOT_TS) {
		/*
		 * check if the packet has an RX OOB block
		 * if not, we can't determine the orginating port
		 * [sorry for confusion with the flag name - it should be
		 * GOT_OOB. I'll fix it later -- Tom]
		 */
		epnum = NIC_RX1_D1_PORT_R(r1);
		ts_r = NIC_RX1_D2_TS_R_R(r2);
		ts_f = NIC_RX1_D2_TS_F_R(r2);
	} else {
		pr_err("No RX OOB? Something's seriously fkd....\n");

		writel( (2000 << 16) | offset, &rx->rx3);
		writel(NIC_RX1_D1_EMPTY, &rx->rx1); /* FIXME: count as error */
		return ;
	}

	dev = wrn->dev[epnum];
	ep = netdev_priv(dev);

	/* Data and length */
	off = NIC_RX1_D3_OFFSET_R(r3);
	len = NIC_RX1_D3_LEN_R(r3);
	skb = netdev_alloc_skb(dev, len + 16 /* FIXME: real size for rx */);
	/* FIXME: handle allocation failure */
	skb_reserve(skb, 2);
	__wrn_copy_in(skb_put(skb, len), wrn->databuf + off, len);

	/*
	 * reload the descriptor length (it was modified by the NIC
	 * during reception of the packet)
	 */
	writel( (2000 << 16) | offset, &rx->rx3);
	writel(NIC_RX1_D1_EMPTY, &rx->rx1);

	/* RX timestamping part */

	hwts = skb_hwtstamps(skb);

	wrn_ppsg_read_time(wrn, &counter_ppsg, &utc);

	if(counter_ppsg < REFCLK_FREQ/4 && ts_r > 3*REFCLK_FREQ/4)
		utc--;

	ts.tv_sec = (s32)utc & 0x7fffffff;
	cntr_diff = (ts_r & 0xf) - ts_f;
	/* the bit says the rising edge cnter is 1tick ahead */
	if(cntr_diff == 1 || cntr_diff == (-0xf))
		ts.tv_sec |= 0x80000000;
	ts.tv_nsec = ts_r * NSEC_PER_TICK;

	pr_debug("Timestamp: %li:%li, ahead = %d\n",
	       ts.tv_sec & 0x7fffffff,
	       ts.tv_nsec & 0x7fffffff,
	       ts.tv_sec & 0x80000000 ? 1 :0);

	hwts->hwtstamp = timespec_to_ktime(ts);
	skb->protocol = eth_type_trans(skb, dev);
	skb->ip_summed = CHECKSUM_UNNECESSARY;
	dev->last_rx = jiffies;
	ep->stats.rx_packets++;
	ep->stats.rx_bytes += len;
	netif_receive_skb(skb);
}

static void wrn_rx_interrupt(struct wrn_dev *wrn)
{
	int desc;
	struct wrn_rxd __iomem *rx;
	u32 reg;

	while (1) {
		desc = wrn->next_rx;
		rx = wrn->rxd + desc;
		reg = readl(&rx->rx1);
		if (reg & NIC_RX1_D1_EMPTY)
			return;
		__wrn_rx_descriptor(wrn, desc);
		wrn->next_rx = __wrn_next_desc(desc);
	}
}

static void wrn_tx_interrupt(struct wrn_dev *wrn)
{
	struct wrn_txd *tx;
	struct sk_buff *skb;
	struct skb_shared_info *info;

	u32 reg;
	int i;

	/* Loop using our talil until one is not sent */
	while ( (i = wrn->next_tx_tail) != wrn->next_tx_head) {
		/* Check if this is txdone */
		tx = wrn->txd + i;
		reg = readl(&tx->tx1);
		if (reg & NIC_TX1_D1_READY)
			return; /* no more */

		skb = wrn->skb_desc[i].skb;
		if (!skb) {
			pr_err("no socket in descriptor %i\n");
			return;
		}
		info = skb_shinfo(skb);

		if (info->tx_flags & SKBTX_HW_TSTAMP) {
			/* hardware timestamping is enabled */
			info->tx_flags |= SKBTX_IN_PROGRESS;
			pr_debug("%s: %i -- in progress\n", __func__, __LINE__);
			wrn_tstamp_find_skb(wrn, i);
			/* It has been freed if found; otherwise keep it */
		} else {
			dev_kfree_skb_irq(skb);
			wrn->skb_desc[i].skb = 0;
		}
		wrn->next_tx_tail = __wrn_next_desc(i);
	}
}

irqreturn_t wrn_interrupt(int irq, void *dev_id)
{
	struct wrn_dev *wrn = dev_id;
	struct NIC_WB *regs = wrn->regs;
	u32 i, irqs;

	irqs = readl((void *)regs + 0x2c /*EIC_ISR */);
	i =  readl(&regs->SR);
	pr_debug("%s: irqs 0x%x, sr 0x%x\n", __func__, irqs, i);
	if (irqs & NIC_EIC_ISR_TXERR) {
		pr_err("%s: TX error\n", __func__); /* FIXME */
		writel(NIC_EIC_ISR_TXERR, (void *)regs + 0x2c);
	}
	if (irqs & NIC_EIC_ISR_TCOMP) {
		pr_debug("%s: TX complete\n", __func__);
		wrn_tx_interrupt(wrn);
		writel(NIC_EIC_ISR_TCOMP, (void *)regs + 0x2c);
	}
	if (irqs & NIC_EIC_ISR_RCOMP) {
		pr_debug("%s: RX complete\n", __func__);
		wrn_rx_interrupt(wrn);
		writel(NIC_EIC_ISR_RCOMP, (void *)regs + 0x2c);
	}
	return IRQ_HANDLED;
}
