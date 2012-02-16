/*
 * DMTD calibration procedures
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

#include "wr-nic.h"

int wrn_phase_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct wrn_phase_req phase_req;
	struct wrn_ep *ep = netdev_priv(dev);
	u32 dmsr;
	s32 ph;

	phase_req.phase = 0;
	phase_req.ready = 0;

	dmsr = readl(&ep->ep_regs->DMSR);

	if(dmsr & EP_DMSR_PS_RDY) {
		ph = EP_DMSR_PS_VAL_R(dmsr);

		/* Sign-extend the 24-bit value */
		if(ph & 0x800000)
		    ph |= 0xff << 24;

		/* Divide by nsamples (average) */
		ph /= WRN_DMTD_AVG_SAMPLES;

		/* Put it back in the proper range */
		ph = (ph + WRN_DMTD_MAX_PHASE) % WRN_DMTD_MAX_PHASE;

		phase_req.phase = ph;
		phase_req.ready = 1;
	}

	if (copy_to_user(rq->ifr_data, &phase_req, sizeof(phase_req)))
		return -EFAULT;
	return 0;
}

int wrn_calib_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct wrn_calibration_req cal_req;
	struct wrn_ep *ep = netdev_priv(dev);
	u32 tmp;

	if (copy_from_user(&cal_req, rq->ifr_data, sizeof(cal_req)))
		return -EFAULT;

	if (0) { /* FIXME: what' coming out of this thing? */
		if (!(ep->ep_flags & BIT(WRN_EP_UP)))
			return -EIO; /* was -EFAULT in minic */
	}

	switch(cal_req.cmd) {
	case WRN_CAL_TX_ON:
		tmp = wrn_phy_read(dev, 0, WRN_MDIO_WR_SPEC);
		wrn_phy_write(dev, 0, WRN_MDIO_WR_SPEC,
			      tmp | WRN_MDIO_WR_SPEC_TX_CAL);
		break;

	case WRN_CAL_TX_OFF:
		tmp = wrn_phy_read(dev, 0, WRN_MDIO_WR_SPEC);
		wrn_phy_write(dev, 0, WRN_MDIO_WR_SPEC,
			  tmp & (~WRN_MDIO_WR_SPEC_TX_CAL));
		break;

	case WRN_CAL_RX_ON:
		tmp = wrn_phy_read(dev, 0, WRN_MDIO_WR_SPEC);
		wrn_phy_write(dev, 0, WRN_MDIO_WR_SPEC,
			  tmp | WRN_MDIO_WR_SPEC_CAL_CRST);
		break;

	case WRN_CAL_RX_OFF:
		// do nothing.....
		break;

	case WRN_CAL_RX_CHECK:
		tmp = wrn_phy_read(dev, 0, WRN_MDIO_WR_SPEC);

		cal_req.cal_present = tmp & WRN_MDIO_WR_SPEC_RX_CAL_STAT
			? 1 : 0;

		if (copy_to_user(rq->ifr_data,&cal_req,
				 sizeof(cal_req)))
			return -EFAULT;
		break;
	}

	return 0;
}
