#ifndef AMP_TEST_ACCESS_H
#define AMP_TEST_ACCESS_H
#include <linux/ioctl.h>
/* 只提供实验共享页和资源交接；不取代正式/dev/rt_mpu_samples提供者。 */
#define AMP_TEST_DEVICE "/dev/rt_mpu_amp_acq"
#define AMP_TEST_CLAIM _IO(0xb8, 1)
#define AMP_TEST_RELEASE _IO(0xb8, 2)
#endif
