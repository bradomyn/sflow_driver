/*
 * Pulse per second management for White-Rabbit switch network interface
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

void wrn_ppsg_read_time(struct wrn_dev *wrn, u32 *fine_counter, u32 *utc)
{
	u32 utc1, utc2, cnt;

	utc1 = readl(&wrn->ppsg_regs->CNTR_UTCLO);
	cnt = readl(&wrn->ppsg_regs->CNTR_NSEC);
	utc2 = readl(&wrn->ppsg_regs->CNTR_UTCLO);

	if (utc2 != utc1)
		cnt = readl(&wrn->ppsg_regs->CNTR_NSEC);

	*utc = utc2;
	*fine_counter = cnt;
}
