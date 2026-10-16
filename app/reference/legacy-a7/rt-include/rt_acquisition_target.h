#ifndef RT_ACQUISITION_TARGET_H
#define RT_ACQUISITION_TARGET_H

/* 只在采集平台入口选择核心；业务、维护、规约和录波不依赖这些宏。 */
#define ADC_CORE_E907 0
#define ADC_CORE_A7_AMP 1
#define ADC_LBC_EXISTING 0
#define ADC_LBC_IDA_PIO 1
#define ADC_LBC_IDA_DMA 2
#define ADC_LBC_DA 3

#ifndef ADC_ACQ_CORE
#define ADC_ACQ_CORE ADC_CORE_E907
#endif
#ifndef ADC_LBC_MODE
#define ADC_LBC_MODE ADC_LBC_EXISTING
#endif

#if ADC_ACQ_CORE != ADC_CORE_E907 && ADC_ACQ_CORE != ADC_CORE_A7_AMP
#error "Invalid ADC_ACQ_CORE"
#endif
#if ADC_LBC_MODE < ADC_LBC_EXISTING || ADC_LBC_MODE > ADC_LBC_DA
#error "Invalid ADC_LBC_MODE"
#endif
#if ADC_ACQ_CORE == ADC_CORE_E907 && ADC_LBC_MODE == ADC_LBC_DA
#error "T153 E907 supports IDA/DMA only"
#endif

/* A7核号及阶段显式配置，默认仍是没有ADC接管的最小心跳。 */
#ifndef ADC_AMP_CPU
#define ADC_AMP_CPU 3
#endif
#ifndef ADC_AMP_HEARTBEAT_ONLY
#define ADC_AMP_HEARTBEAT_ONLY 1
#endif
#if ADC_ACQ_CORE == ADC_CORE_A7_AMP && ADC_AMP_CPU != 3
#error "This isolated experiment reserves A7 CPU3 only"
#endif

#endif
