/*
 * This file hosts memory-specific inlines, separated for easier review
 *
 * Copyright (C) 2010 CERN (www.cern.ch)
 * Author: Alessandro Rubini <rubini@gnudd.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include "wr-nic.h"
#include <asm/unaligned.h>

/* Descriptor direction, used to locate descriptror data memory */
enum wrn_ddir {
    WRN_DDIR_RX,
    WRN_DDIR_TX
};

/* We place the descriptor memory in fixed positions (2kB each) */
static inline int __wrn_desc_offset(struct wrn_dev *wrn,
				    enum wrn_ddir dir, int nr)
{
	if (dir == WRN_DDIR_RX) nr += WRN_NR_TXDESC;
	return 0x800 * nr;
}

static inline u32 __iomem *__wrn_desc_mem(struct wrn_dev *wrn,
					   enum wrn_ddir dir, int nr)
{
	/* note: this is a void pointer, but then we return a u32 ptr */
	void __iomem *ptr = wrn->databuf;
	return ptr + __wrn_desc_offset(wrn, dir, nr);
}

/* The two copy functions take arguments in the same order as memcpy */
static inline void __wrn_copy_out(u32 __iomem *to, void *from, int size)
{
	u32 i;

	from -= WRN_DDATA_OFFSET;
	size += WRN_DDATA_OFFSET;
	while (size > 0) {
		i = get_unaligned_le32(from);
		writel(i, to);
		to++; from += sizeof(i);
		size -= sizeof(i);
	}
}

static inline void __wrn_copy_in(void *to, u32 __iomem *from, int size)
{
	u32 i;

	to -= WRN_DDATA_OFFSET;
	size += WRN_DDATA_OFFSET;
	while (size > 0) {
		i = readl(from);
		put_unaligned_le32(i, to);
		to += sizeof(i); from++;
		size -= sizeof(i);
	}
}

