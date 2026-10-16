/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMP_LINUX_REALTIME_IO_H
#define AMP_LINUX_REALTIME_IO_H
#include "realtime_io.h"
#if AMP_REALTIME_IO
int amp_rtio_linux_init(void __iomem *page);
int amp_rtio_linux_claim(void);
int amp_rtio_linux_release(void);
void amp_rtio_linux_exit(void);
#else
static inline int amp_rtio_linux_init(void __iomem *page) { (void)page; return 0; }
static inline int amp_rtio_linux_claim(void) { return 0; }
static inline int amp_rtio_linux_release(void) { return 0; }
static inline void amp_rtio_linux_exit(void) { }
#endif
#endif
