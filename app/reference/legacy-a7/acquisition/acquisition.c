#include <string.h>
#include "acquisition.h"
#include "rates.h"

/* 所有故障都先撤转换源；首错保留，不能自动重建资格或补点。 */
static int fail(struct amp_acquisition *acq, int error, rt_u32 fault)
{
    int stopped = 0;
    if (!acq->first_error)
        acq->fail_poll_flags = (rt_u32)acq->completed | ((rt_u32)acq->seen_high << 1) | ((rt_u32)acq->synchronized << 8);
    if (acq->armed)
        stopped = acq->ops.stop(acq->ops.context);
    acq->armed = 0;
    acq->di_valid = 0;
    acq->state = RT_STATE_FAULT;
    acq->faults |= fault;
    acq->errors++;
    if (!acq->first_error)
        acq->first_error = error;
    /* 撤源失败保持 FAULT，调用方禁止 CPU_OFF/归还资源。 */
    if (stopped)
        acq->faults |= RT_FAULT_PROTOCOL;
    return error;
}

void amp_acquisition_init(struct amp_acquisition *acq,
                           const struct amp_acquisition_ops *ops, rt_u32 epoch)
{
    memset(acq, 0, sizeof(*acq));
    acq->ops = *ops;
    acq->epoch = epoch;
    acq->state = RT_STATE_STOPPED;
}

int amp_acquisition_arm(struct amp_acquisition *acq,
                          const struct rt_acq_config_v1 *config, rt_u64 now)
{
    struct rt_adc_register_plan plan;
    int result;
    if (!acq || !config || !acq->epoch || acq->armed ||
        acq->state != RT_STATE_STOPPED || !acq->ops.prepare || !acq->ops.start ||
        !acq->ops.stop || !acq->ops.read || !acq->ops.publish ||
        (AMP_BUS_DI_ONLY && !acq->ops.read_digital))
        return RT_REPLY_BAD_STATE;
    /* 高档需显式实验构建；仍保留OS0、真实BUSY及公共读窗预算。 */
    if (config->oversampling_code > 3U ||
        (config->initial_period_ticks == 938U && config->oversampling_code) ||
        !amp_rate_points(config->initial_period_ticks) ||
        (config->enabled_chip_mask != RT_ADC_COMPLETE_MASK &&
         (!config->enabled_chip_mask || config->enabled_chip_mask > 16U ||
          (config->enabled_chip_mask & (config->enabled_chip_mask - 1U)))) ||
        config->samples_per_cycle != amp_rate_points(config->initial_period_ticks))
        return RT_REPLY_BAD_CONFIG;
    result = rt_adc_compile_config(config, &plan);
    if (result)
        return result;
    /* prepare 的失败也必须有清理入口；只有撤源/归还成功才可离开本次会话。 */
    acq->plan = plan;
    memset(&acq->sample, 0, sizeof(acq->sample));
    amp_sample_time_reset(&acq->sample_time, &acq->rtc.bank[acq->rtc.active]);
    acq->armed = 1;
    result = acq->ops.prepare(acq->ops.context, &plan);
    if (result)
        return fail(acq, result, RT_FAULT_ADC);
    result = acq->ops.start(acq->ops.context, plan.period_ticks);
    if (result)
        return fail(acq, result, RT_FAULT_ADC);
    acq->qualified = 0;
    acq->di_valid = 0;
    acq->di_due_ticks = 0;
    acq->last_sample_ticks = 0;
    acq->synchronized = acq->seen_high = acq->completed = 0;
    acq->host_seen_ticks = now;
    acq->run_generation++;
    acq->state = RT_STATE_ARMED;
    if (AMP_BUS_DI_ONLY) acq->state = RT_STATE_RUNNING;
    return RT_OK;
}

int amp_acquisition_stop(struct amp_acquisition *acq)
{
    int result = acq->ops.stop(acq->ops.context);
    if (result) {
        acq->state = RT_STATE_FAULT;
        acq->faults |= RT_FAULT_PROTOCOL;
        if (!acq->first_error)
            acq->first_error = result;
        return result;
    }
    acq->armed = 0;
    acq->di_valid = 0;
    acq->state = RT_STATE_STOPPED;
    acq->synchronized = acq->seen_high = acq->completed = 0;
    return RT_OK;
}

void amp_acquisition_host_alive(struct amp_acquisition *acq, rt_u64 now)
{
    acq->host_seen_ticks = now;
}

static int read_digital(struct amp_acquisition *acq, rt_u64 now)
{
    rt_u32 raw = 0, ticks = 0;
    int result;
    acq->di_valid = 0;
    if (!acq->ops.read_digital) return 0;
    result = acq->ops.read_digital(acq->ops.context, &raw, &ticks);
    if (result) {
        acq->di_errors++;
        return fail(acq, result, RT_FAULT_ADC);
    }
    raw &= AMP_BUS_DI_VALID;
    if (acq->di_scans) {
        rt_u64 interval = now - acq->di_last_scan_ticks;
        if (interval > acq->di_max_interval_ticks) acq->di_max_interval_ticks = interval;
        if (raw != acq->di_raw) acq->di_changes++;
    } else acq->di_first_scan_ticks = now;
    acq->di_last_scan_ticks = now;
    acq->di_scans++;
    acq->di_sum_ticks += ticks;
    acq->di_last_ticks = ticks;
    if (!acq->di_min_ticks || ticks < acq->di_min_ticks) acq->di_min_ticks = ticks;
    if (ticks > acq->di_max_ticks) acq->di_max_ticks = ticks;
    acq->di_raw = raw & AMP_BUS_DI_VALID;
    acq->di_valid = AMP_BUS_DI_VALID;
    return 0;
}

int amp_acquisition_poll(struct amp_acquisition *acq, const struct amp_observation *now)
{
    struct rt_sample_v1 *sample = &acq->sample;
    struct rt_adc_busy_event event;
    rt_u64 elapsed, read_ns, sample_tick;
    rt_u32 mask, ticks;
    int result;
    if (!acq->armed)
        return RT_REPLY_BAD_STATE;
    acq->before_observation = acq->previous;
    acq->last_observation = *now;
    if (now->ticks < acq->host_seen_ticks ||
        now->ticks - acq->host_seen_ticks >= AMP_HOST_LEASE_TICKS)
        return fail(acq, AMP_EHOST_LEASE, RT_FAULT_PROTOCOL);
    if (AMP_BUS_DI_ONLY) {
        /* 独立DI测试只走单调时钟，不伪造BUSY、ADC资格或模拟帧。 */
        if (acq->di_scans && now->ticks < acq->di_last_scan_ticks)
            return fail(acq, AMP_ECLOCK, RT_FAULT_PROTOCOL);
        if (AMP_BUS_DI_PERIOD_TICKS) {
            if (!acq->di_scans) acq->di_due_ticks = now->ticks;
            if (now->ticks < acq->di_due_ticks) return 0;
#if AMP_BUS_DI_PERIOD_TICKS
            acq->di_missed_periods += (now->ticks - acq->di_due_ticks) / AMP_BUS_DI_PERIOD_TICKS;
            acq->di_due_ticks += ((now->ticks - acq->di_due_ticks) / AMP_BUS_DI_PERIOD_TICKS + 1U) * AMP_BUS_DI_PERIOD_TICKS;
#endif
        }
        result = read_digital(acq, now->ticks);
        if (result) return result;
        /* 仍用同一256字节记录及样点环；模拟槽无有效位，不计ADC资格。 */
        sample->sample_sequence = rt_words_make(++acq->sequence);
        sample->sample_ticks = rt_words_make(now->ticks);
        sample->session_epoch = acq->epoch;
        sample->config_generation = acq->plan.generation;
        sample->period_ticks = AMP_BUS_DI_PERIOD_TICKS;
        sample->digital_words = 1;
        sample->digital[0] = (rt_u16)acq->di_raw;
        sample->digital[1] = 0;
        sample->digital_valid[0] = AMP_BUS_DI_VALID;
        sample->digital_valid[1] = 0;
        sample->flags = RT_SAMPLE_PAYLOAD_CRC;
        amp_sample_time_stamp(&acq->sample_time, &acq->rtc, sample);
        sample->payload_crc32 = rt_crc32(sample->analog, 176U);
        result = acq->ops.publish(acq->ops.context, sample);
        if (result) {
            acq->ring_errors++;
            return fail(acq, result, RT_FAULT_RING_FULL);
        }
        return 0;
    }
    if (now->period != acq->plan.period_ticks || now->phase >= now->period ||
        now->ticks < now->phase || now->busy < 0 || now->busy > 1)
        return fail(acq, AMP_ECLOCK, RT_FAULT_ADC);
    if (!acq->synchronized) {
        acq->previous = *now;
        acq->phase_progress_ticks = now->ticks;
        acq->synchronized = 1;
        return RT_OK;
    }
    if (now->ticks < acq->previous.ticks)
        return fail(acq, AMP_ECLOCK, RT_FAULT_ADC);
    elapsed = now->ticks - acq->previous.ticks;
    if (now->phase != acq->previous.phase)
        acq->phase_progress_ticks = now->ticks;
    else if (now->ticks - acq->phase_progress_ticks > AMP_POLL_GUARD_TICKS)
        return fail(acq, AMP_ECLOCK, RT_FAULT_ADC);
    if (elapsed > acq->max_poll_ticks)
        acq->max_poll_ticks = elapsed > 0xffffffffU ? 0xffffffffU : (rt_u32)elapsed;
    /* 超过一整周期无法证明只经过一次转换，禁止用相位回绕推算/补齐。 */
    if (elapsed >= now->period) {
        acq->missed_cycles++;
        return fail(acq, AMP_EMISSED_CYCLE, RT_FAULT_ADC);
    }
    {
        /* 两个相位已校验在同一合法周期内，条件回绕可替代每次轮询的整数除法。 */
        rt_u32 advance = now->phase >= acq->previous.phase ?
            now->phase - acq->previous.phase :
            now->phase + now->period - acq->previous.phase;
        if ((rt_u64)advance + AMP_POLL_GUARD_TICKS < elapsed ||
            (rt_u64)advance > elapsed + AMP_POLL_GUARD_TICKS)
            return fail(acq, AMP_ECLOCK, RT_FAULT_ADC);
    }
    if (now->phase < acq->previous.phase) {
        if (!acq->completed && acq->synchronized == 2) {
            acq->missed_cycles++;
            return fail(acq, AMP_EBUSY_EDGE, RT_FAULT_ADC);
        }
        acq->synchronized = 2;
        acq->seen_high = acq->completed = 0;
    }
    acq->previous = *now;
    /* 启动的半个PWM周期不计资格；必须先观察一次真实相位回绕。 */
    if (acq->synchronized != 2)
        return RT_OK;
    if (acq->completed) {
        /* PWM有效脉冲位于计数周期尾部，下一转换BUSY可先于回绕升高；
         * 只允许尾部1µs脉宽和有界观测偏差，回绕后仍必须重新观察真实高低沿。 */
        if (now->busy && now->phase + 24U + AMP_POLL_GUARD_TICKS < now->period)
            return fail(acq, AMP_EBUSY_EDGE, RT_FAULT_ADC);
        return RT_OK;
    }
    if (now->busy) {
        acq->seen_high = 1;
        if (now->phase > AD7616_FRAME_READY_TICKS) {
            acq->busy_timeouts++;
            return fail(acq, AMP_EBUSY_EDGE, RT_FAULT_ADC);
        }
        return RT_OK;
    }
    if (!acq->seen_high) {
        if (now->phase > AD7616_FRAME_READY_TICKS) {
            acq->busy_timeouts++;
            return fail(acq, AMP_EBUSY_EDGE, RT_FAULT_ADC);
        }
        return RT_OK;
    }
    /* 已在同一PWM周期观察到BUSY高和当前低，间隔稍长不等于漏转换。
     * 仍限制真实完成相位及整周期连续性，不扩大20µs转换截止或快照误差预算。 */
    if (now->phase > AD7616_FRAME_READY_TICKS) {
        acq->missed_cycles++;
        return fail(acq, AMP_EBUSY_EDGE, RT_FAULT_ADC);
    }
    event.phase = now->phase;
    event.monotonic_ticks = now->ticks;
    event.clock_ns = AMP_512_EXPERIMENT ? 0 : amp_ticks_to_ns(now->ticks);
    acq->falls++;
    /* 真实BUSY完成入口先取本地开入，再取五片ADC，保持一帧共同上送。 */
    result = read_digital(acq, now->ticks);
    if (result) return result;
    /* 读函数只有整帧校验通过才覆盖80槽，错误路径绝不发布私有旧帧。 */
    result = acq->ops.read(acq->ops.context, &event, sample->analog, &mask, &read_ns);
    if (result || mask != acq->plan.enabled_chip_mask) {
        acq->read_errors++;
        return fail(acq, result ? result : RT_ADC_EWINDOW, RT_FAULT_ADC);
    }
    /* 完整读窗已限制为短时段；32bit等价计算避免每帧一次通用64bit除法。 */
    ticks = read_ns <= 1431655765U ? (rt_u32)read_ns * 3U / 125U :
        (rt_u32)(read_ns * 24U / 1000U);
    acq->read_count++;
    acq->read_sum_ticks += ticks;
    if (ticks > acq->read_max_ticks)
        acq->read_max_ticks = ticks;
    sample->digital_words = acq->di_valid ? 1U : 0U;
    sample->digital[0] = (rt_u16)acq->di_raw;
    sample->digital[1] = 0;
    sample->digital_valid[0] = (rt_u16)acq->di_valid;
    sample->digital_valid[1] = 0;
    acq->completed = 1;
    if (acq->qualified < AMP_QUALIFY_FRAMES) {
        acq->qualified++;
        if (acq->qualified == AMP_QUALIFY_FRAMES)
            acq->state = RT_STATE_RUNNING;
        return RT_OK;
    }
    /* 时间来自真实PWM相位回算的启动沿，有界轮询误差，不冒充硬件捕获时标。 */
    sample_tick = now->ticks - now->phase;
    if (acq->last_sample_ticks && (sample_tick <= acq->last_sample_ticks ||
        sample_tick - acq->last_sample_ticks < now->period - AMP_POLL_GUARD_TICKS ||
        sample_tick - acq->last_sample_ticks > now->period + AMP_POLL_GUARD_TICKS)) {
        acq->missed_cycles++;
        return fail(acq, AMP_EMISSED_CYCLE, RT_FAULT_ADC);
    }
    acq->last_sample_ticks = sample_tick;
    sample->sample_sequence = rt_words_make(++acq->sequence);
    sample->sample_ticks = rt_words_make(sample_tick);
    sample->session_epoch = acq->epoch;
    sample->config_generation = acq->plan.generation;
    sample->period_ticks = now->period;
    sample->analog_count = RT_ANALOG_CHANNELS;
    sample->valid_chip_mask = mask;
    sample->irq_latency_ticks = RT_LATENCY_UNKNOWN;
    sample->flags = RT_SAMPLE_ADC_VALID | RT_SAMPLE_EDGE_PREDICTED | RT_SAMPLE_PAYLOAD_CRC;
    amp_sample_time_stamp(&acq->sample_time, &acq->rtc, sample);
    sample->payload_crc32 = rt_crc32(sample->analog, 176U);
    result = acq->ops.publish(acq->ops.context, sample);
    if (result) {
        acq->ring_errors++;
        return fail(acq, result, RT_FAULT_RING_FULL);
    }
    return RT_OK;
}
