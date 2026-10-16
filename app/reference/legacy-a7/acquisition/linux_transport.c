// SPDX-License-Identifier: GPL-2.0
#include <linux/err.h>
#include <linux/io.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include "linux_transport.h"
#include "os_sample_device.h"
#include "os_wave_stream.h"
#include "rt_shared_ring.h"

static void __iomem *region;
static struct rt_ring sample_ring;
static struct os_sample_device *sample_device;
static struct task_struct *worker;
static struct rt_sample_v1 *diag_batch;
static int (*status_snapshot)(struct amp_acq_status *);
static int (*resource_active)(void);
static bool app_owner, bound;
static u32 consumer_generation = 1, serial, errors, gaps, discarded;
static u64 previous_sequence, previous_tick;
static u32 bound_epoch, enabled_mask;
static DEFINE_MUTEX(consumer_lock);
static struct rt_io io;

int amp_transport_stop_remote(void)
{
    struct rt_ring command_ring, event_ring;
    struct rt_object object;
    struct rt_message_v1 request = {0}, reply;
    struct amp_acq_status state;
    u64 deadline = ktime_get_ns() + 1500000000ULL;
    int result;
    if (status_snapshot(&state))
        return -EAGAIN;
    result = rt_find_object(&io, RT_OBJ_COMMAND, RT_FEATURES_V1, &object);
    if (!result) result = rt_ring_bind(&io, &object, &command_ring);
    if (!result) result = rt_find_object(&io, RT_OBJ_EVENT, RT_FEATURES_V1, &object);
    if (!result) result = rt_ring_bind(&io, &object, &event_ring);
    if (result) return -EPROTO;
    request.id = RT_MSG_STOP;
    request.request_id = 0xfffffffeU;
    request.session_epoch = state.epoch;
    request.flags = RT_MSG_CRC_FLAG;
    request.payload_crc32 = rt_crc32(request.payload, 0);
    if (rt_ring_push(&command_ring, &request, sizeof(request)))
        return -EBUSY;
    do {
        result = rt_ring_pop(&event_ring, &reply, sizeof(reply));
        if (!result && reply.id == request.id && reply.request_id == request.request_id &&
            reply.session_epoch == request.session_epoch && reply.flags == RT_MSG_ACK_FLAG) {
            if (status_snapshot(&state) || state.epoch != request.session_epoch || !state.stopped)
                return -EBUSY;
            return reply.status == 0 || (reply.status == RT_REPLY_BAD_EPOCH && !state.run_generation) ? 0 : -EBUSY;
        }
        if (result != RT_OK && result != RT_EMPTY)
            return -EPROTO;
        usleep_range(1000, 1500);
    } while (ktime_get_ns() < deadline);
    return -ETIMEDOUT;
}

static u32 load(void *context, u32 offset)
{
    u32 value = readl((void __iomem *)context + offset);
    rmb();
    return value;
}
static void store(void *context, u32 offset, u32 value)
{
    mb();
    writel(value, (void __iomem *)context + offset);
}
static void from(void *context, void *out, u32 offset, u32 bytes)
{
    memcpy_fromio(out, (void __iomem *)context + offset, bytes);
    rmb();
}
static void to(void *context, u32 offset, const void *in, u32 bytes)
{
    memcpy_toio((void __iomem *)context + offset, in, bytes);
    wmb();
}
static struct rt_io io = {
    .mapped_bytes = RT_REGION_BYTES, .load_acquire = load, .store_release = store,
    .copy_from = from, .copy_to = to,
};

static int bind_ring(void)
{
    struct rt_object object;
    int result;
    if (bound)
        return 0;
    result = rt_find_object(&io, RT_OBJ_SAMPLE, RT_FEATURES_V1, &object);
    if (!result)
        result = rt_ring_bind(&io, &object, &sample_ring);
    if (!result)
        bound = true;
    return result ? -EPROTO : 0;
}

static int fill_status(struct rt_sample_device_header *h)
{
    struct amp_acq_status state;
    u32 read, write, used;
    if (status_snapshot(&state) || bind_ring())
        return -EAGAIN;
    if (bound_epoch && state.epoch != bound_epoch)
        return -ESTALE;
    bound_epoch = state.epoch;
    enabled_mask = state.enabled_mask;
    read = load(region, sample_ring.consumer_offset);
    write = load(region, sample_ring.producer_offset);
    used = write - read;
    if (used > sample_ring.capacity)
        return -EPROTO;
    memset(h, 0, sizeof(*h));
    h->version = RT_SAMPLE_DEVICE_VERSION;
    h->header_bytes = sizeof(*h);
    h->record_bytes = sizeof(struct rt_sample_v1);
    h->max_frames = RT_SAMPLE_CAPACITY;
    h->flags = (state.state == RT_STATE_RUNNING && state.qualified == 128 && !state.faults ?
                RT_SAMPLE_DEVICE_PEER_READY : 0) | (app_owner ? RT_SAMPLE_DEVICE_APP_OWNER : 0);
    if (errors || gaps || state.faults)
        h->flags |= RT_SAMPLE_DEVICE_BATCH_FAULT;
    h->batch_serial = serial;
    h->pending_at_entry = used;
    h->capacity = sample_ring.capacity;
    h->headroom_at_entry = sample_ring.capacity - used;
    h->read_index_entry = h->read_index_after = read;
    h->write_index_entry = write;
    h->ring_dropped = load(region, sample_ring.producer_offset + 4);
    h->remote_epoch = state.epoch;
    h->host_epoch = load(region, RT_HOST_STATUS_OFF + offsetof(struct rt_peer_status, session_epoch));
    h->run_generation = state.run_generation;
    h->consumer_generation = consumer_generation;
    h->config_generation = load(region, RT_REMOTE_STATUS_OFF + offsetof(struct rt_peer_status, active_config_generation));
    h->period_ticks = state.period;
    h->remote_state = state.state;
    h->remote_faults = state.faults;
    h->bad_frames = errors;
    h->sequence_gaps = gaps;
    h->undelivered_frames = discarded;
    return 0;
}

/* 公开字符设备既有read/mmap协议的提供者；诊断线程与应用通过同一把本地锁交接。 */
static int status(void *context, struct rt_sample_device_header *header)
{
    int result;
    mutex_lock(&consumer_lock);
    result = fill_status(header);
    mutex_unlock(&consumer_lock);
    return result;
}

static int valid(const struct rt_sample_v1 *s, const struct rt_sample_device_header *h)
{
    u64 sequence = rt_words_value(s->sample_sequence), tick = rt_words_value(s->sample_ticks);
    int result = 0;
    if (s->session_epoch != h->remote_epoch || s->config_generation != h->config_generation ||
        s->analog_count != 80 || s->period_ticks != h->period_ticks ||
        s->valid_chip_mask != enabled_mask ||
        !(s->flags & RT_SAMPLE_ADC_VALID) || (s->flags & RT_SAMPLE_ADC_ESTIMATED) ||
        !(s->flags & RT_SAMPLE_PAYLOAD_CRC) || rt_crc32(s->analog, 176) != s->payload_crc32) {
        if (errors != U32_MAX) errors++;
        result = -EBADMSG;
    }
    if (previous_sequence && (sequence != previous_sequence + 1 || tick <= previous_tick ||
        tick - previous_tick < h->period_ticks - 96 || tick - previous_tick > h->period_ticks + 96)) {
        if (gaps != U32_MAX) gaps++;
        result = -EBADMSG;
    }
    previous_sequence = sequence;
    previous_tick = tick;
    return result;
}

static int read_batch_locked(struct rt_sample_device_header *h, struct rt_sample_v1 *records, u32 max)
{
    u64 started = ktime_get_ns();
    u32 count, i;
    int result = fill_status(h);
    if (result)
        return result;
    count = min(max, h->pending_at_entry);
    h->sample_flags_and = count ? U32_MAX : 0;
    for (i = 0; i < count; i++) {
        result = rt_ring_pop(&sample_ring, &records[i], sizeof(*records));
        if (result) {
            /* 前面已弹出的帧未交付给调用方，不能把这次部分失败记作零丢弃。 */
            discarded = i > U32_MAX - discarded ? U32_MAX : discarded + i;
            if (errors != U32_MAX) errors++;
            return -EPROTO;
        }
        if (valid(&records[i], h))
            h->flags |= RT_SAMPLE_DEVICE_BATCH_FAULT;
        else
            os_wave_publish(&records[i]);
        h->sample_flags_or |= records[i].flags;
        h->sample_flags_and &= records[i].flags;
    }
    h->frame_count = count;
    h->read_index_after = h->read_index_entry + count;
    h->bad_frames = errors;
    h->sequence_gaps = gaps;
    h->batch_serial = ++serial;
    h->read_start_ns = rt_words_make(started);
    h->read_end_ns = rt_words_make(ktime_get_ns());
    return 0;
}

static int read_batch(void *context, struct rt_sample_device_header *h, struct rt_sample_v1 *records, u32 max)
{
    int result;
    mutex_lock(&consumer_lock);
    result = app_owner ? read_batch_locked(h, records, max) : -EACCES;
    mutex_unlock(&consumer_lock);
    return result;
}
static int claim(void *context)
{
    int result = 0;
    mutex_lock(&consumer_lock);
    if (app_owner || !resource_active())
        result = -EBUSY;
    else {
        app_owner = true;
        consumer_generation++;
    }
    mutex_unlock(&consumer_lock);
    return result;
}
static void release(void *context)
{
    mutex_lock(&consumer_lock);
    app_owner = false;
    consumer_generation++;
    mutex_unlock(&consumer_lock);
}
static void discard(void *context, u32 count)
{
    mutex_lock(&consumer_lock);
    discarded = count > U32_MAX - discarded ? U32_MAX : discarded + count;
    mutex_unlock(&consumer_lock);
}
static int wave_snapshot(struct rt_peer_status *peer)
{
    struct amp_acq_status state;
    if (status_snapshot(&state))
        return -EAGAIN;
    memset(peer, 0, sizeof(*peer));
    peer->bytes = sizeof(*peer);
    peer->session_epoch = state.epoch;
    peer->state = state.state;
    peer->fault_bits = state.faults | ((READ_ONCE(errors) || READ_ONCE(gaps)) ? RT_FAULT_PROTOCOL : 0);
    peer->monotonic_tick_hz = peer->pwm_clock_hz = 24000000;
    peer->active_config_generation = load(region, RT_REMOTE_STATUS_OFF + offsetof(struct rt_peer_status, active_config_generation));
    return 0;
}
static int drain_worker(void *unused)
{
    struct rt_sample_device_header h;
    while (!kthread_should_stop()) {
        mutex_lock(&consumer_lock);
        if (!app_owner && resource_active())
            read_batch_locked(&h, diag_batch, 64);
        mutex_unlock(&consumer_lock);
        usleep_range(1000, 1500);
    }
    return 0;
}
int amp_transport_start(int (*snapshot)(struct amp_acq_status *), int (*active)(void))
{
    struct os_sample_device_ops ops = {
        .claim = claim, .release = release, .status = status, .read_batch = read_batch, .discard = discard,
    };
    int result;
    status_snapshot = snapshot;
    resource_active = active;
    /* v2共享DDR与A7一致使用Normal NC；状态页和外设仍为Device。 */
    region = ioremap_wc(AMP_ACQ_RING_ADDRESS, RT_REGION_BYTES);
    if (!region)
        return -ENOMEM;
    io.context = (void *)region;
    diag_batch = kmalloc_array(64, sizeof(*diag_batch), GFP_KERNEL);
    if (!diag_batch) { result = -ENOMEM; goto unmap; }
    result = os_wave_start(0, wave_snapshot);
    if (result)
        goto buffer;
    sample_device = os_sample_device_start(&ops);
    if (IS_ERR(sample_device)) { result = PTR_ERR(sample_device); goto wave; }
    worker = kthread_run(drain_worker, NULL, "rt_amp_diag");
    if (!IS_ERR(worker))
        return 0;
    result = PTR_ERR(worker);
    os_sample_device_stop(sample_device);
wave:
    os_wave_stop();
buffer:
    kfree(diag_batch);
unmap:
    iounmap(region);
    return result;
}
void amp_transport_stop(void)
{
    kthread_stop(worker);
    os_sample_device_stop(sample_device);
    os_wave_stop();
    kfree(diag_batch);
    iounmap(region);
}
