#ifndef __WR_SFLOW_H
#define __WR_SFLOW_H

#define __WR_IOC_MAGIC		'5' // no assigned according to ioctl-number.txt

#define WR_SFLW_IRQWAIT		_IO(__WR_IOC_MAGIC, 5)
#define WR_SFLW_IRQENA		_IO(__WR_IOC_MAGIC, 6)

#endif /*__WR_SFLOW_H*/
