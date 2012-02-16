/*
 * Ethtool operations for White-Rabbit switch network interface
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
#include <linux/netdevice.h>
#include <linux/mii.h>
#include <linux/ethtool.h>
#include <linux/spinlock.h>

#include "wr-nic.h"

static int wrn_get_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct wrn_ep *ep = netdev_priv(dev);
	int ret;

	spin_lock_irq(&ep->lock);
	ret = mii_ethtool_gset(&ep->mii, cmd);
	spin_unlock_irq(&ep->lock);

	cmd->supported=
		SUPPORTED_FIBRE | /* FIXME: copper sfp? */
		SUPPORTED_Autoneg |
		SUPPORTED_1000baseKX_Full;
	cmd->advertising =
		ADVERTISED_1000baseKX_Full |
		ADVERTISED_Autoneg;
	cmd->port = PORT_FIBRE;
	cmd->speed = SPEED_1000;
	cmd->duplex = DUPLEX_FULL;
	cmd->autoneg = AUTONEG_ENABLE;
	return ret;
}

static int wrn_set_settings(struct net_device *dev, struct ethtool_cmd *cmd)
{
	struct wrn_ep *ep = netdev_priv(dev);
	int ret;

	spin_lock_irq(&ep->lock);
	ret = mii_ethtool_sset(&ep->mii, cmd);
	spin_unlock_irq(&ep->lock);

	return ret;
}

static int wrn_nwayreset(struct net_device *dev)
{
	struct wrn_ep *ep = netdev_priv(dev);
	int ret;

	spin_lock_irq(&ep->lock);
	ret = mii_nway_restart(&ep->mii);
	spin_unlock_irq(&ep->lock);

	return ret;
}

static void wrn_get_drvinfo(struct net_device *dev,
			      struct ethtool_drvinfo *info)
{
	strlcpy(info->driver, DRV_NAME, sizeof(info->driver));
	strlcpy(info->version, DRV_VERSION, sizeof(info->version));
	strlcpy(info->bus_info, dev_name(dev->dev.parent),
		sizeof(info->bus_info));
}

/*
 * These are the operations we support. No coalescing is there since
 * most of the traffic will just happen within the FPGA switching core.
 * Similarly, other funcionality like ringparam are not used.
 * get_eeprom/set_eeprom may be useful for a simple MAC address management.
 */
static const struct ethtool_ops wrn_ethtool_ops = {
	.get_settings	= wrn_get_settings,
	.set_settings	= wrn_set_settings,
	.get_drvinfo	= wrn_get_drvinfo,
	.nway_reset	= wrn_nwayreset,
	/* Some of the default methods apply for us */
	.get_link	= ethtool_op_get_link,
	/* FIXME: get_regs_len and get_regs may be useful for debugging */
};

int wrn_ethtool_init(struct net_device *netdev)
{
	netdev->ethtool_ops = &wrn_ethtool_ops;
	return 0;
}
