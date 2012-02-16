#ifndef __WR_RTU_H
#define __WR_RTU_H

#define __WR_IOC_MAGIC		'4'

#define WR_RTU_IRQWAIT		_IO(__WR_IOC_MAGIC, 4)
#define WR_RTU_IRQENA		_IO(__WR_IOC_MAGIC, 5)

#endif /*__WR_RTU_H*/
