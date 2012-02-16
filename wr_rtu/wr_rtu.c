/*
 * White Rabbit RTU (Routing Table Unit)
 * Copyright (C) 2010, CERN.
 *
 * Version:     wr_rtu v1.0
 *
 * Authors:     Alessandro Rubini <rubini@gnudd.com>
 *              Juan Luis Manas   <juan.manas@integrasys.es>
 *              Miguel Baizan     <miguel.baizan@integrasys.es>
 *
 * Description:  RTU IRQ registration, capture and handling. 
 *               Applies gnurabbit (http://www.ohwr.org) misc device concepts 
 *               to make RTU UFIFO interrupts available to user space.
 *
 * Fixes:       
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/ioctl.h>
#include <linux/interrupt.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/spinlock.h>

#include "../wbgen-regs/sflow-regs.h"
#include "wr_sflow.h"

#define DRV_MODULE_VERSION      "0.1"

#define WR_RTU_IRQ		2   //Test change from 2 (RTU) to 3 
#define WRVIC_BASE_IRQ		(NR_AIC_IRQS + (5 * 32)) // top of GPIO interr

#define FPGA_BASE_RTU		0x10060000               // fpga_regs.h

struct wr_sflow_dev {
	wait_queue_head_t	q;
	spinlock_t		lock;
	unsigned long		flags;
};
#define WR_FLAG_IRQDISABLE	0x00000002

static struct wr_rtu_dev dev;

static struct RTU_WB __iomem *regs;

#define wr_rtu_readl(r)		__raw_readl(&regs->r);
#define wr_rtu_writel(val, r)	__raw_writel(val, &regs->r);

static void wr_rtu_enable_irq(void)
{
	wr_rtu_writel(RTU_EIC_IER_NEMPTY, EIC_IER);
}

static void wr_rtu_disable_irq(void)
{
	wr_rtu_writel(RTU_EIC_IDR_NEMPTY, EIC_IDR);
}

static void wr_rtu_clear_irq(void)
{
	wr_rtu_writel(RTU_EIC_ISR_NEMPTY, EIC_ISR);
}

static int rtu_ufifo_is_empty(void)
{
	uint32_t csr =  wr_rtu_readl(UFIFO_CSR);
	return RTU_UFIFO_CSR_EMPTY & csr;
}

// RTU interrupt handler
static irqreturn_t wr_rtu_interrupt(int irq, void *unused)
{
	// When IRQ is enabled an irq is raised even if UFIFO is empty. 
	// In such a case just ignore IRQ.
	if (rtu_ufifo_is_empty())
		return IRQ_NONE;
	spin_lock(&dev.lock);
	dev.flags |= WR_FLAG_IRQDISABLE;
	wr_rtu_disable_irq();
	spin_unlock(&dev.lock);
	wake_up_interruptible(&dev.q);
	return IRQ_HANDLED;
}


static long wr_rtu_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	int ret = 0;

	// Check cmd type
	if (_IOC_TYPE(cmd) != __WR_IOC_MAGIC)
		return -ENOIOCTLCMD;

	switch(cmd) {
	case WR_RTU_IRQWAIT: // Await UFIFO IRQ
		spin_lock_irq(&dev.lock);
		if (dev.flags & WR_FLAG_IRQDISABLE)
			ret = -EAGAIN;
		spin_unlock_irq(&dev.lock);
		if (ret < 0)
			return ret;
		wait_event_interruptible(dev.q, !rtu_ufifo_is_empty());
		// Make sure 'wait' was interrupted by IRQ
		if (signal_pending(current))
			return -ERESTARTSYS;
		return ret;
	case WR_RTU_IRQENA:  // Re-enable UFIFO IRQ
		spin_lock_irq(&dev.lock);
		if (!(dev.flags & WR_FLAG_IRQDISABLE)) {
			ret = -EAGAIN;
		} else {
			dev.flags &= ~WR_FLAG_IRQDISABLE;
			wr_rtu_clear_irq();
			wr_rtu_enable_irq();
		}
		spin_unlock_irq(&dev.lock);
		return ret;
	default:
		return -ENOIOCTLCMD;
	}
}

static struct file_operations wr_rtu_fops = {
	.owner          = THIS_MODULE,
	.unlocked_ioctl = wr_rtu_ioctl
};

// TODO check available minor numbers
static struct miscdevice wr_rtu_misc = {
	.minor = 77,
	.name  = "wr_rtu",
	.fops  = &wr_rtu_fops
};

static int __init wr_rtu_init(void)
{
	int err;

	// register misc device
	err = misc_register(&wr_rtu_misc);
	if (err < 0) {
		printk(KERN_ERR "%s: Can't register misc device\n",
		       KBUILD_MODNAME);
		return err;
	}

	// map RTU memory
	regs = ioremap(
		FPGA_BASE_RTU,
		sizeof(struct RTU_WB)
		);

	if (!regs) {
		misc_deregister(&wr_rtu_misc);
		return -ENOMEM;
	}

	// register interrupt handler
	wr_rtu_disable_irq();

	err = request_irq(
		WRVIC_BASE_IRQ + WR_RTU_IRQ,
		wr_rtu_interrupt,
		IRQF_SHARED,
		"wr-rtu",
		(void*)regs
		);

	// if succeeded, enable interrupts
	if (err) {
		printk(KERN_ERR "%s: Cant' request IRQ, error %i\n",
		       KBUILD_MODNAME, err);
		iounmap(regs);
		misc_deregister(&wr_rtu_misc);
		return err;
	}
	wr_rtu_enable_irq();

	// Init wait queue
	init_waitqueue_head(&dev.q);

	printk(KERN_INFO "%s: initialized\n", KBUILD_MODNAME);
	return err;
}

static void __exit wr_rtu_exit(void)
{
	// disable RTU interrupts
	wr_rtu_disable_irq();
	// Unregister IRQ handler
	free_irq(WRVIC_BASE_IRQ + WR_RTU_IRQ, (void*)regs);
	// Unmap RTU memory
	iounmap(regs);
	// Unregister misc device driver
	misc_deregister(&wr_rtu_misc);

	printk(KERN_INFO "%s: cleaned up\n", KBUILD_MODNAME);
}

module_init(wr_rtu_init);
module_exit(wr_rtu_exit);

MODULE_DESCRIPTION("WR RTU IRQ handler");
MODULE_VERSION(DRV_MODULE_VERSION);
MODULE_LICENSE("GPL");


