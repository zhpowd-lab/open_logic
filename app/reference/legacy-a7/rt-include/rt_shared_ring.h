/* 平台只实现有序的共享区访问；环形所有权和边界检查由公共代码处理。 */
#ifndef RT_SHARED_RING_H
#define RT_SHARED_RING_H
#include "rt_shared_abi.h"

#define RT_OK       0
#define RT_EMPTY    1
#define RT_FULL     2
#define RT_INVALID -1
#define RT_BOUNDS  -2
#define RT_VERSION -3
#define RT_CORRUPT -4

/* 本地共享区访问依赖；映射界限与 acquire/release、缓存维护由平台负责，回调指针不进入跨核 ABI。 */
struct rt_io {
    /* 仅本核可解释的访问上下文，通常为映射首址；禁止把它写进共享记录。 */
    void *context;
    /* 实际映射字节上限；目录、环及复制范围不能超出它。 */
    rt_u32 mapped_bytes;
    /* 32 位对齐访问必须原子；acquire/release 不替代非一致缓存的维护。
     * release 也必须保证先前的读取完成，再归还消费者游标，不能只排序写入。 */
    rt_u32 (*load_acquire)(void *context, rt_u32 offset);
    void (*store_release)(void *context, rt_u32 offset, rt_u32 value);
    /* offset 为共享偏移；读取前平台维护缓存，复制结果由消费者独占。 */
    void (*copy_from)(void *context, void *destination, rt_u32 offset, rt_u32 bytes);
    /* 写完整内容并维护缓存；随后 store_release 才发布可读记录。 */
    void (*copy_to)(void *context, rt_u32 offset, const void *source, rt_u32 bytes);
};

/* 本地句柄不进入共享区；初始化完成后描述符必须保持不变。 */
/* 验证布局后建立的本地环句柄，所有 offset 均相对映射基址；不跨核传递原生地址。 */
struct rt_ring {
    const struct rt_io *io;
    rt_u32 data_offset, capacity, record_bytes, format;
    rt_u32 producer_offset, consumer_offset;
};

/* 一次批量读取只覆盖开始时已发布的游标范围。 */
/* 消费者固定入口快照，end_index 为当时生产游标，remaining 保证新帧留给下一次批处理。 */
struct rt_ring_batch {
    rt_u32 end_index;
    rt_u32 remaining;
};

/* 仅在 E907 已停止、Linux 独占共享区时调用；最后发布 magic。 */
/* 主端在 R 核停止且读者退出后清共享区、建立目录/配置/三环，最后发布 magic 表示布局完整。 */
int rt_region_format(const struct rt_io *io, rt_u32 session_epoch,
                      rt_u32 offered_features);
/* 核查共享头、特性及目录的边界/重复/重叠后定位对象；输出只在全表验证通过后提交。 */
int rt_find_object(const struct rt_io *io, rt_u32 id, rt_u32 supported_features,
                    struct rt_object *result);
/* 核查对象与环描述符一致性、容量及数据边界，建立本地句柄；不传递原生指针到共享 ABI。 */
int rt_ring_bind(const struct rt_io *io, const struct rt_object *object,
                  struct rt_ring *ring);
/* 每条环只允许一个 push 执行者和一个 pop 执行者；本函数不等待、不分配内存。 */
/* 唯一生产者写一条完整记录再 release 发布；满环只计丢弃不覆盖未归还槽，RAW80 补偏移链。 */
int rt_ring_push(struct rt_ring *ring, const void *record, rt_u32 bytes);
/* records 是可写的连续本地记录；RAW80 使用 rt_sample_v1 数组并在本地补齐 next_offset。
 * 整批空间不足时不改记录、不写数据；全部复制完成后只发布一次写游标。
 * 与 push 共用唯一生产者，调用期间不得嵌套另一个生产者。 */
/* 唯一生产者预检整批空间，分环尾最多两段复制后只发布一次游标；失败不交付半批。 */
int rt_ring_push_many(struct rt_ring *ring, void *records, rt_u32 count);
/* 唯一消费者 acquire 检查可读槽及偏移链，完整复制到本地后归还游标；空环与损坏分别返回。 */
int rt_ring_pop(struct rt_ring *ring, void *record, rt_u32 bytes);
/* 冻结入口时已发布的结束游标与剩余数，新追加帧留给下一轮，避免批处理无界追赶。 */
int rt_ring_batch_begin(struct rt_ring *ring, struct rt_ring_batch *batch);
/* 在固定批范围内取一帧，并核查消费游标没有被另一消费者改动；成功才减少 remaining。 */
int rt_ring_batch_pop(struct rt_ring *ring, struct rt_ring_batch *batch,
                       void *record, rt_u32 bytes);
/* 按标准反射多项式计算字节 CRC32，初值/终值均取反；用于配置与可选记录载荷校验。 */
rt_u32 rt_crc32(const void *data, rt_u32 bytes);
#endif
