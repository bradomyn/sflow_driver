/*
 * Timestamping routines for WR Switch
 *
 * Copyright (C) 2010 CERN (www.cern.ch)
 * Author: Tomasz Wlostowski <tomasz.wlostowski@cern.ch>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/sockios.h>
#include <linux/net_tstamp.h>

#include "wr-nic.h"

/* This looks for an skb in the already-received stamp list */
void wrn_tstamp_find_skb(struct wrn_dev *wrn, int desc)
{
	struct skb_shared_hwtstamps *hwts;
	struct sk_buff *skb = wrn->skb_desc[desc].skb;
	struct timespec ts;
	int id = wrn->skb_desc[desc].id;
	u32 counter_ppsg; /* PPS generator nanosecond counter */
	u32 utc;
	int i; /* FIXME: use list for faster access */

	for(i = 0; i < WRN_TS_BUF_SIZE; i++)
		if(wrn->ts_buf[i].valid && wrn->ts_buf[i].frame_id == id)
			break;

	if (i == WRN_TS_BUF_SIZE) {
		pr_debug("%s: not found\n", __func__);
		return;
	}
	pr_debug("%s: found\n", __func__);

	/* so we found the skb, do the timestamping magic */
	hwts = skb_hwtstamps(skb);
	wrn_ppsg_read_time(wrn, &counter_ppsg, &utc);
	if(counter_ppsg > 3*REFCLK_FREQ/4 && wrn->ts_buf[i].ts < REFCLK_FREQ/4)
		utc--;

	ts.tv_sec = (s32)utc & 0x7fffffff;
	ts.tv_nsec = wrn->ts_buf[i].ts * NSEC_PER_TICK;
	hwts->hwtstamp = timespec_to_ktime(ts);
	skb_tstamp_tx(skb, hwts);
	dev_kfree_skb_irq(skb);

	/* release both the descriptor and the tstamp entry */
	wrn->skb_desc[desc].skb = 0;
	wrn->ts_buf[i].valid = 0;
}

/* This function records the timestamp in a list -- called from interrupt */
static int record_tstamp(struct wrn_dev *wrn, u32 tsval, u32 idreg)
{
	int port_id = TXTSU_TSF_R1_PID_R(idreg);
	int frame_id = TXTSU_TSF_R1_FID_R(idreg);
	struct skb_shared_hwtstamps *hwts;
	struct timespec ts;
	struct sk_buff *skb;
	u32 utc, counter_ppsg; /* PPS generator nanosecond counter */
	int i; /* FIXME: use list for faster access */

	/*printk("%s: Got TS: %x pid %d fid %d\n", __func__,
		 tsval, port_id, frame_id);*/

	/* First of all look if the skb is already pending */
	for (i = 0; i < WRN_NR_DESC; i++)
		if (wrn->skb_desc[i].skb && wrn->skb_desc[i].id == frame_id)
			break;

	if (i < WRN_NR_DESC) {
		/*printk("%s: found\n", __func__);*/
		skb = wrn->skb_desc[i].skb;
		hwts = skb_hwtstamps(skb);

		wrn_ppsg_read_time(wrn, &counter_ppsg, &utc);
		if(counter_ppsg < (tsval & 0xfffffff))
			utc--;

		ts.tv_sec = (s32)utc & 0x7fffffff;
		ts.tv_nsec = (tsval & 0xfffffff) * NSEC_PER_TICK;
		hwts->hwtstamp = timespec_to_ktime(ts);
		skb_tstamp_tx(skb, hwts);
		dev_kfree_skb_irq(skb);
		wrn->skb_desc[i].skb = 0;
		return 0;
	}
	/* Otherwise, save it to the list  */
	for(i = 0; i < WRN_TS_BUF_SIZE; i++)
		if(!wrn->ts_buf[i].valid)
			break;

	if (i == WRN_TS_BUF_SIZE) {
		pr_debug("%s: ENOMEM\n", __func__);
		return -ENOMEM;
	}
	pr_debug("%s: save to slot %i\n", __func__, i);
	wrn->ts_buf[i].ts = tsval;
	wrn->ts_buf[i].port_id = port_id;
	wrn->ts_buf[i].frame_id = frame_id;
	wrn->ts_buf[i].valid = 1;
	return 0;
}

irqreturn_t wrn_tstamp_interrupt(int irq, void *dev_id)
{
	struct wrn_dev *wrn = dev_id;
	struct TXTSU_WB *regs = wrn->txtsu_regs;
	u32 r0, r1;

	/* printk("%s: %i\n", __func__, __LINE__); */
	/* FIXME: locking */
	r0 = readl(&regs->TSF_R0);
	r1 = readl(&regs->TSF_R1);

	if(record_tstamp(wrn, r0, r1) < 0) {
		printk("%s: ENOMEM in the TS buffer. Disabling TX stamping.\n",
		       __func__);
		writel(TXTSU_EIC_IER_NEMPTY, &wrn->txtsu_regs->EIC_IDR);
	}
	writel(TXTSU_EIC_IER_NEMPTY, &wrn->txtsu_regs->EIC_ISR); /* ack irq */
	return IRQ_HANDLED;
}

int wrn_tstamp_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct wrn_ep *ep = netdev_priv(dev);
	struct hwtstamp_config config;

	if (copy_from_user(&config, rq->ifr_data, sizeof(config)))
		return -EFAULT;
	if (0) netdev_dbg(dev, "%s: tx type %i, rx filter %i\n", __func__,
		   config.tx_type, config.rx_filter);

	switch (config.tx_type) {
		/* Set up time stamping on transmission */
	case HWTSTAMP_TX_ON:
		set_bit(WRN_EP_STAMPING_TX, &ep->ep_flags);
		/* FIXME: enable timestamp on tx in hardware */
		break;

	case HWTSTAMP_TX_OFF:
		/* FIXME: disable timestamp on tx in hardware */
		clear_bit(WRN_EP_STAMPING_TX, &ep->ep_flags);
		break;

	default:
		return -ERANGE;
	}

	/*
	 * For the time being, make this really simple and stupid: either
	 * time-tag _all_ the incoming packets or none of them.
	 */
	switch (config.rx_filter) {
	case HWTSTAMP_FILTER_NONE:
		/* FIXME: disable rx in hardware */
		clear_bit(WRN_EP_STAMPING_RX, &ep->ep_flags);
		break;

	default: /* All other case: activate stamping */
		/* FIXME: enable rx in hardware */
		set_bit(WRN_EP_STAMPING_RX, &ep->ep_flags);

		break;
	}

	/* FIXME: minic_update_ts_config(nic); */

	if (copy_to_user(rq->ifr_data, &config, sizeof(config)))
		return -EFAULT;
	return 0;
}

void wrn_tstamp_init(struct wrn_dev *wrn)
{
	memset(wrn->ts_buf, 0, sizeof(wrn->ts_buf));
	/* enable TXTSU irq */
	writel(TXTSU_EIC_IER_NEMPTY, &wrn->txtsu_regs->EIC_IER);
}

