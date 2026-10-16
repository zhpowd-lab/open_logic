/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMP_REALTIME_IO_IOCTL_H
#define AMP_REALTIME_IO_IOCTL_H
#include <linux/ioctl.h>
#include "realtime_io.h"
#define AMP_AUX_DEVICE "/dev/rt_mpu_aux"
#define AMP_AUX_SNAPSHOT _IOR(0xb9, 1, struct amp_rtio_snapshot)
#define AMP_AUX_RTC_SET _IOWR(0xb9, 2, struct amp_rtio_command)
#endif
