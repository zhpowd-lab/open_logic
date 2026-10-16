#ifndef RT_SAMPLE_DEVICE_H
#define RT_SAMPLE_DEVICE_H

#include <linux/ioctl.h>
#include "rt_shared_abi.h"

#define RT_SAMPLE_DEVICE_PATH "/dev/rt_mpu_samples"
#define RT_SAMPLE_DEVICE_VERSION 1U
#define RT_SAMPLE_DEVICE_MAX_FRAMES RT_SAMPLE_CAPACITY
#define RT_SAMPLE_DEVICE_PEER_READY (1U << 0)
#define RT_SAMPLE_DEVICE_APP_OWNER (1U << 1)
#define RT_SAMPLE_DEVICE_RETRIED (1U << 2)
#define RT_SAMPLE_DEVICE_INVALIDATED (1U << 3)
#define RT_SAMPLE_DEVICE_BATCH_FAULT (1U << 4)
/* GET_STATUS明确标识仍借用的映射，ACK失效后可同步用户态租用状态。 */
#define RT_SAMPLE_DEVICE_MMAP_BORROWED (1U << 5)

/* 固定宽度用户接口：游标均是模 2^32 逻辑索引，绝不是物理或虚拟指针。 */
struct rt_sample_device_header {
    rt_u32 version, header_bytes, record_bytes, max_frames;
    /* GET_STATUS 的 frame_count 为私有待交付量，其入口游标仍描述当前共享环。 */
    rt_u32 flags, batch_serial, frame_count, pending_at_entry;
    rt_u32 read_index_entry, write_index_entry, read_index_after, capacity;
    rt_u32 headroom_at_entry, ring_dropped, host_epoch, remote_epoch;
    rt_u32 run_generation, consumer_generation, config_generation, period_ticks;
    rt_u32 remote_state, remote_faults, sample_flags_or, sample_flags_and;
    /* 本批质量计数及累计应用未交付量饱和到 UINT32_MAX，不以回绕伪装为零。 */
    rt_u32 bad_frames, sequence_gaps, missed_samples, undelivered_frames;
    /* Linux CLOCK_MONOTONIC 读批起止时刻；不得直接减去 E907 sample_ticks。 */
    struct rt_words64 read_start_ns, read_end_ns;
};

/* 调用者按 max_frames 分配头后紧随的 RAW80 数组，最多 1024 帧；不传本机指针。 */
struct rt_sample_device_batch {
    struct rt_sample_device_header header;
    struct rt_sample_v1 samples[];
};

/* ioctl 只编码 128 字节头，避免整批数组超过 _IOC_SIZE 位宽。 */
#define RT_SAMPLE_DEVICE_GET_STATUS     _IOC(_IOC_READ, 0xb7, 1, 128U)
#define RT_SAMPLE_DEVICE_READ_BATCH     _IOC(_IOC_READ | _IOC_WRITE, 0xb7, 2, 128U)
/* 只读映射仍由 Linux 保留私有整批；BEGIN 借用后须 ACK 才允许覆盖下一批。 */
#define RT_SAMPLE_DEVICE_BEGIN_MMAP_BATCH _IOC(_IOC_READ | _IOC_WRITE, 0xb7, 3, 128U)
#define RT_SAMPLE_DEVICE_ACK_MMAP_BATCH   _IOC(_IOC_WRITE, 0xb7, 4, 128U)
#define RT_SAMPLE_DEVICE_MAP_BYTES (128U + RT_SAMPLE_DEVICE_MAX_FRAMES * 256U)

RT_ASSERT(sizeof(struct rt_sample_device_header) == 128, sample_device_header);
RT_ASSERT(offsetof(struct rt_sample_device_batch, samples) == 128, sample_device_samples);
RT_ASSERT(_IOC_SIZE(RT_SAMPLE_DEVICE_READ_BATCH) == 128, sample_device_ioctl_size);

#endif
