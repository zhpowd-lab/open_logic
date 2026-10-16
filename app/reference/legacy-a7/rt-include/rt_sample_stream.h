#ifndef RT_SAMPLE_STREAM_H
#define RT_SAMPLE_STREAM_H

#include "rt_shared_ring.h"

#define RT_STREAM_GAP       -5
#define RT_STREAM_FRAME     -6
#define RT_STREAM_BUDGET    -7
#define RT_STREAM_SINK      -8

/* 只保证样点连续；保护数据的新鲜度仍需由 Linux 侧时钟与调度预算判定。 */
/* Linux 本地连续原始流状态；会话/配置/片数固定，next_sequence 与 last_ticks 判漏点，fault 直到重新初始化。 */
struct rt_sample_stream {
    struct rt_ring *ring;
    rt_u32 session_epoch, config_generation, chip_mask, period_ticks;
    rt_u32 continuous_frames, warmup_frames;
    rt_u64 next_sequence, last_ticks;
    int fault;
    rt_u8 started;
};

/* 一次固定快照批消费结果；pending 为入口已发布数量，accepted 为已交付 sink 数量。 */
struct rt_stream_batch_result {
    rt_u32 accepted, pending, continuous_frames;
};

/* 绑定固定会话、配置、片掩码及预热长度；只初始化本地状态，不修改共享游标。 */
int rt_sample_stream_init(struct rt_sample_stream *stream, struct rt_ring *ring,
                           rt_u32 session_epoch, rt_u32 config_generation,
                           rt_u32 chip_mask, rt_u32 period_ticks,
                           rt_u32 warmup_frames);
/* 消费入口时生产游标快照内的整批原始帧并交付 sink；超批预算、坏帧或 sink 失败锁存故障。 */
int rt_sample_stream_batch(struct rt_sample_stream *stream,
                            struct rt_sample_v1 *scratch, rt_u32 max_frames,
                            int (*sink)(void *context, const struct rt_sample_v1 *sample),
                            void *context, struct rt_stream_batch_result *result);
/* 返回是否已达到无故障连续帧预热门槛；该资格不代表数据年龄或实机保护验收通过。 */
int rt_sample_stream_continuous(const struct rt_sample_stream *stream);

#endif
