#ifndef AMP_ACQUISITION_RATES_H
#define AMP_ACQUISITION_RATES_H

/* 高档只用于独立采集实验；原完整DTU应用仍只支持128/256点。 */
#ifndef AMP_HIGH_RATE_EXPERIMENT
#define AMP_HIGH_RATE_EXPERIMENT 0
#endif
#ifndef AMP_512_EXPERIMENT
#define AMP_512_EXPERIMENT 0
#endif
#if (AMP_512_EXPERIMENT != 0 && AMP_512_EXPERIMENT != 1) || \
    (AMP_512_EXPERIMENT && !AMP_HIGH_RATE_EXPERIMENT)
#error "512-point experiment requires the explicit high-rate build"
#endif
#if AMP_HIGH_RATE_EXPERIMENT != 0 && AMP_HIGH_RATE_EXPERIMENT != 1
#error "AMP_HIGH_RATE_EXPERIMENT must be 0 or 1"
#endif

static inline unsigned amp_rate_period(unsigned hz)
{
    switch (hz) {
    case 6400U: return 3750U;
    case 12800U: return 1875U;
#if AMP_HIGH_RATE_EXPERIMENT
    case 16000U: return 1500U;
    case 19200U: return 1250U;
    case 20000U: return 1200U;
#if AMP_512_EXPERIMENT
    /* 名义512点；24MHz固定整数周期取938，实际25586.353945SPS，不能称精确25.6k。 */
    case 25600U: return 938U;
#endif
#endif
    default: return 0U;
    }
}

static inline unsigned amp_rate_points(unsigned period)
{
    switch (period) {
    case 3750U: return 128U;
    case 1875U: return 256U;
#if AMP_HIGH_RATE_EXPERIMENT
    case 1500U: return 320U;
    case 1250U: return 384U;
    case 1200U: return 400U;
#if AMP_512_EXPERIMENT
    case 938U: return 512U;
#endif
#endif
    default: return 0U;
    }
}

#endif
