#ifndef RT_ADC_LBC_TIMING_H
#define RT_ADC_LBC_TIMING_H

#include "rt_shared_abi.h"

#ifndef RT_MPU_ADC_LBC_RUNTIME_TIMING
#define RT_MPU_ADC_LBC_RUNTIME_TIMING 0
#endif
#ifndef RT_MPU_ADC_SELECT_SETTLE_TICKS
#define RT_MPU_ADC_SELECT_SETTLE_TICKS 24U
#endif
#ifndef RT_MPU_ADC_LBC_RD_CYCLE_TICKS
#define RT_MPU_ADC_LBC_RD_CYCLE_TICKS 12U
#endif
#ifndef RT_MPU_ADC_LBC_CS_ON_TICKS
#define RT_MPU_ADC_LBC_CS_ON_TICKS 0U
#endif
#ifndef RT_MPU_ADC_LBC_CS_OFF_TICKS
#define RT_MPU_ADC_LBC_CS_OFF_TICKS 9U
#endif
#ifndef RT_MPU_ADC_LBC_OE_ON_TICKS
#define RT_MPU_ADC_LBC_OE_ON_TICKS 2U
#endif
#ifndef RT_MPU_ADC_LBC_OE_OFF_TICKS
#define RT_MPU_ADC_LBC_OE_OFF_TICKS 7U
#endif
#ifndef RT_MPU_ADC_LBC_ACCESS_TICKS
#define RT_MPU_ADC_LBC_ACCESS_TICKS 6U
#endif

/* GPIO稳定等待用24MHz tick；LBC字段用固定100MHz时钟计数，不混用单位。 */
struct rt_adc_lbc_config_v1 {
    rt_u32 abi_version, bytes, select_ticks, rd_cycle;
    rt_u32 cs_on, cs_off, oe_on, oe_off, access;
};

/* configured是下次ARM请求；applied表示寄存器已读回，不表示外部引脚时序已验收。 */
struct rt_adc_lbc_status_v1 {
    struct rt_adc_lbc_config_v1 configured;
    rt_u32 writable, generation, applied, clock_hz;
    rt_u32 cs_reg, oe_reg, cycle_reg, access_reg;
};

#define RT_ADC_LBC_DEFAULT_CONFIG {RT_ABI_V1, sizeof(struct rt_adc_lbc_config_v1), \
    RT_MPU_ADC_SELECT_SETTLE_TICKS, RT_MPU_ADC_LBC_RD_CYCLE_TICKS, \
    RT_MPU_ADC_LBC_CS_ON_TICKS, RT_MPU_ADC_LBC_CS_OFF_TICKS, \
    RT_MPU_ADC_LBC_OE_ON_TICKS, RT_MPU_ADC_LBC_OE_OFF_TICKS, RT_MPU_ADC_LBC_ACCESS_TICKS}

/* 限定试验范围及读沿顺序；最慢配置仍受原整帧期限保护，不放宽BUSY/超时预算。 */
static inline int rt_adc_lbc_config_valid(const struct rt_adc_lbc_config_v1 *c)
{
    return c && c->abi_version == RT_ABI_V1 && c->bytes == sizeof(*c) &&
        c->select_ticks >= 6U && c->select_ticks <= 48U &&
        c->rd_cycle >= 9U && c->rd_cycle <= 32U &&
        c->cs_on <= c->oe_on && c->oe_on >= 1U && c->oe_on <= 26U &&
        c->access >= c->oe_on + 2U && c->access <= 31U &&
        c->oe_off >= c->access + 1U && c->oe_off <= 31U &&
        c->oe_off >= c->oe_on + 5U && c->cs_off >= c->oe_off + 1U &&
        c->cs_off <= c->rd_cycle;
}

RT_ASSERT(sizeof(struct rt_adc_lbc_config_v1) == 36, adc_lbc_config_size);
RT_ASSERT(sizeof(struct rt_adc_lbc_status_v1) == 68, adc_lbc_status_size);
#endif
