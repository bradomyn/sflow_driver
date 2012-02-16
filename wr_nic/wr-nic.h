/*
 * wr-nic definitions, structures and prototypes
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
#ifndef __WR_NIC_H__
#define __WR_NIC_H__
#include <linux/irqreturn.h>
#include <linux/spinlock.h>
#include <linux/mii.h>		/* Needed for stuct mii_if_info in wrn_dev */
#include <linux/netdevice.h>	/* Needed for net_device_stats in wrn_dev */
#include <linux/timer.h>	/* Needed for struct time_list in wrn_dev*/

#include "nic-hardware.h" /* Magic numbers: please fix them as needed */

#define DRV_NAME "wr-nic" /* Used in messages and device/driver names */
#define DRV_VERSION "0.1" /* For ethtool->get_drvinfo -- FIXME: auto-vers */

/*
 * Interrupt information should be hidden in resource lists,
 * but the looping code would be hairy. So let's define three
 * arrays of the same size, and code loops over these
 */
#if 0
#define WRN_IRQ_NUMBERS \
	{WRN_IRQ_PPSG, WRN_IRQ_NIC, WRN_IRQ_RTU, WRN_IRQ_RTUT, WRN_IRQ_TSTAMP}
#define WRN_IRQ_NAMES \
	{"wr-ppsg", "wr-nic", "wr-rtu", "wr-rtut", "wr-tstamp"}
#define WRN_IRQ_HANDLERS \
	{NULL, wrn_interrupt, NULL, NULL, wrn_tstamp_interrupt}
#endif

/* Temporarily, two handlers only */
#define WRN_IRQ_NUMBERS {WRN_IRQ_NIC, WRN_IRQ_TSTAMP}
#define WRN_IRQ_NAMES {"wr-nic", "wr-tstamp"}
#define WRN_IRQ_HANDLERS {wrn_interrupt, wrn_tstamp_interrupt}

#define WRN_TS_BUF_SIZE 1024 /* array of timestamp structures */

struct wrn_ep; /* Defined later */

/* A timestamping structure to keep information for user-space */
struct wrn_tx_tstamp {
	u8 valid;
	u8 port_id;
	u16 frame_id;
	u32 ts;
};

/* We must remember both skb and id for each pending descriptor */
struct wrn_desc_pending {
	struct sk_buff *skb;
	u32 id; /* only 16 bits, actually */
};

/*
 * This is the main data structure for our NIC device. As for locking,
 * the rule is that _either_ the wrn _or_ the endpoint is locked. Not both.
 */
struct wrn_dev {
	/* Base addresses. It's easier with an array, but not all are used */
	void __iomem		*bases[WRN_NR_OF_BLOCKS];

	struct NIC_WB __iomem	*regs; /* shorthand for NIC-block registers */
	struct TXTSU_WB __iomem *txtsu_regs; /* ... and the same for TXTSU */
	struct PPSG_WB __iomem *ppsg_regs; /* ... */

	spinlock_t		lock;
	struct wrn_txd __iomem	*txd;
	struct wrn_rxd __iomem	*rxd;
	void __iomem		*databuf; /* void to ease pointer arith */
	int			next_tx_head, next_tx_tail;
	int			next_rx;

	/* For TX descriptors, we must keep track of the ownwer */
	struct wrn_desc_pending	skb_desc[WRN_NR_TXDESC];
	int			id;

	struct net_device	*dev[WRN_NR_ENDPOINTS];
	struct wrn_tx_tstamp	ts_buf[WRN_TS_BUF_SIZE];

	/* FIXME: all dev fields must be verified */

	//unsigned int rx_head, rx_avail, rx_base, rx_size;
	//unsigned int tx_head, tx_avail, tx_base, tx_size;


	//u32 cur_rx_desc;

	int use_count; /* only used at probe time */
	int irq_registered;
};

/* Each network device (endpoint) has one such priv structure */
struct wrn_ep {
	struct wrn_dev		*wrn;
	struct EP_WB __iomem	*ep_regs; /* each EP has its own memory */
	spinlock_t		lock;
	struct timer_list	ep_link_timer;
	volatile unsigned long	ep_flags;
	struct mii_if_info	mii; /* for ethtool operations */
	int			ep_number;
	int			pkt_count; /* used for tx stamping ID */

	struct net_device_stats	stats;
	//struct sk_buff		*current_skb;

	//bool synced;
	//bool syncing_counters;

	//u32 cur_rx_desc;
};
#define WRN_LINK_POLL_INTERVAL (HZ/5)

enum ep_flags { /* only used in the ep_flags register */
	WRN_EP_UP		= 0,
	WRN_EP_IS_UPLINK	= 1,
	WRN_EP_STAMPING_TX	= 2,
	WRN_EP_STAMPING_RX	= 3,
};

/* Our resources. */
enum wrn_resnames {
	/*
	 * The names are used as indexes in the resource array. Note that
	 * they are unrelated with the memory addresses: we can't have
	 * holes in the memory list, so these are _different_ values
	 */
	WRN_RES_MEM_EP_UP0,
	WRN_RES_MEM_EP_UP1,
	WRN_RES_MEM_EP_DP0,
	WRN_RES_MEM_EP_DP1,
	WRN_RES_MEM_EP_DP2,
	WRN_RES_MEM_EP_DP3,
	WRN_RES_MEM_EP_DP4,
	WRN_RES_MEM_EP_DP5,
	WRN_RES_MEM_EP_DP6,
	WRN_RES_MEM_EP_DP7,
	WRN_RES_MEM_PPSG,
	WRN_RES_MEM_CALIBRATOR,
	WRN_RES_MEM_NIC,
	WRN_RES_MEM_TSTAMP,
	/* Irq is last, so platform_get_resource() can use previous enums */
	WRN_RES_IRQ,
};

/*
 * Register access may be needed outside of specific files.
 * Please note that this takes a register *name*, uppercase with no prefix.
 */
#define wrn_ep_read(ep, reg) __raw_readl(&(ep)->ep_regs->reg)
#define wrn_ep_write(ep, reg, val) __raw_writel((val), &(ep)->ep_regs->reg)

/* Private ioctls, (the first 2 are the same as they were in wr_minic.c */
#define PRIV_IOCGCALIBRATE	(SIOCDEVPRIVATE + 1)
#define PRIV_IOCGGETPHASE	(SIOCDEVPRIVATE + 2)
#define PRIV_IOCREADREG		(SIOCDEVPRIVATE + 3)
#define PRIV_IOCPHYREG		(SIOCDEVPRIVATE + 4)

#define NIC_READ_PHY_CMD(addr)  (((addr) & 0xff) << 16)
#define NIC_RESULT_DATA(val) ((val) & 0xffff)
#define NIC_WRITE_PHY_CMD(addr, value)  ((((addr) & 0xff) << 16) \
					 | (1 << 31) \
					 | ((value) & 0xffff))



/* Structures straight from wr_minic.c -- should user-space include this? */
struct wrn_calibration_req {
	int cmd;
	int cal_present;
};

struct wrn_phase_req {
	int ready;
	u32 phase;
};
#define WRN_DMTD_AVG_SAMPLES 256
#define WRN_DMTD_MAX_PHASE 16384

#define WRN_CAL_TX_ON 1
#define WRN_CAL_TX_OFF 2
#define WRN_CAL_RX_ON 3
#define WRN_CAL_RX_OFF 4
#define WRN_CAL_RX_CHECK 5

/* This a WR-specific register in the mdio space */
#define WRN_MDIO_WR_SPEC 0x00000010
#define WRN_MDIO_WR_SPEC_TX_CAL		0x01 /* TX calib pattern */
#define WRN_MDIO_WR_SPEC_RX_CAL_STAT	0x02 /* RX calib status */
#define WRN_MDIO_WR_SPEC_CAL_CRST	0x04 /* Reset calibration counter */


/* Following functions are in nic-core.c */
extern irqreturn_t wrn_interrupt(int irq, void *dev_id);
extern int wrn_netops_init(struct net_device *netdev);

/* Following data in device.c */
struct platform_driver;
extern struct platform_driver wrn_driver;

/* Following functions in ethtool.c */
extern int wrn_ethtool_init(struct net_device *netdev);

/* Following functions in endpoint.c */
extern int wrn_phy_read(struct net_device *dev, int phy_id, int location);
extern void wrn_phy_write(struct net_device *dev, int phy_id, int loc, int v);

extern int wrn_ep_open(struct net_device *dev);
extern int wrn_ep_close(struct net_device *dev);

extern int  wrn_endpoint_probe(struct net_device *netdev);
extern void wrn_endpoint_remove(struct net_device *netdev);

/* Following functions from timestamp.c */
extern void wrn_tstamp_find_skb(struct wrn_dev *wrn, int i);
extern int wrn_tstamp_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
extern irqreturn_t wrn_tstamp_interrupt(int irq, void *dev_id);
extern void wrn_tstamp_init(struct wrn_dev *wrn);

/* Following functions from dmtd.c */
extern int wrn_phase_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
extern int wrn_calib_ioctl(struct net_device *dev, struct ifreq *rq, int cmd);
extern void wrn_ppsg_read_time(struct wrn_dev *wrn, u32 *fine_cnt, u32 *utc);

#endif /* __WR_NIC_H__ */
