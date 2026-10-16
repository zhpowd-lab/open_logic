#ifndef RT_ACQUISITION_BOARD_H
#define RT_ACQUISITION_BOARD_H

#include "rt_shared_abi.h"
#include "rt_acquisition_rate.h"

/* 本装置的数据几何：公共电压与十六回路电流共五片 ADC，后续三个片选是数字量。 */
#define RT_ADC_CHIP_COUNT          5U
#define RT_ADC_CHANNELS_PER_CHIP  16U
#define RT_ADC_ENABLED_MASK       ((1U << RT_ADC_CHIP_COUNT) - 1U)
#ifndef RT_MPU_CAPTURE_CHIP_MASK
#define RT_MPU_CAPTURE_CHIP_MASK RT_ADC_ENABLED_MASK
#endif
#define RT_DI_CHIP_COUNT           3U
#define RT_DI_FIRST_CHIP           RT_ADC_CHIP_COUNT
#define RT_DI_CHIP_END             (RT_DI_FIRST_CHIP + RT_DI_CHIP_COUNT)
/* 原保护算法及滤波器的点数固定，独立采集的速率使用 RT_MPU_CAPTURE_* 配置。 */
#define RT_SAMPLES_PER_CYCLE       128U
#define RT_ADC_RANGE_REG_COUNT     4U
#define RT_ADC_SEQUENCE_REG_COUNT  (RT_ADC_CHANNELS_PER_CHIP / 2U)

/* 这些 AD7616 寄存器只用于与原 BSP 的配置入口兼容，不在 Linux 端读写硬件。 */
#define AD7616_REGISTER_COUNT      64U
#define AD7616_REGISTER_VALUE_MAX  0x1ffU
#define AD7616_MODE_REGISTER       2U
#define AD7616_RANGE_FIRST         4U
#define AD7616_RANGE_END           (AD7616_RANGE_FIRST + RT_ADC_RANGE_REG_COUNT)
#define AD7616_SEQUENCE_FIRST      0x20U
#define AD7616_SEQUENCE_END        (AD7616_SEQUENCE_FIRST + RT_ADC_SEQUENCE_REG_COUNT)

RT_ASSERT(RT_ADC_CHIP_COUNT * RT_ADC_CHANNELS_PER_CHIP == RT_ANALOG_CHANNELS,
          board_analog_geometry);

#endif
