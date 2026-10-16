/* A7采集验收及常驻租约控制端；显式run/hold才取得资源，status只读。 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include "access.h"
#include "protocol.h"
#include "rates.h"
#include "native_progress.h"
#include "rt_shared_ring.h"
#include "rt_sample_device.h"
#include "realtime_io_ioctl.h"
#include "aux_boot_time.h"

static volatile sig_atomic_t interrupted;
static volatile struct amp_acq_status *page;
static struct rt_io io;
static struct rt_ring commands, events;
static struct rt_ring di_samples;
static rt_u32 request_id, remote_epoch, host_epoch;
static struct rt_sample_device_batch *batch;
static FILE *di_capture;
static struct rt_sample_v1 *di_capture_frames;
static rt_u64 di_capture_capacity, di_capture_used;
static rt_u64 di_min_interval, di_max_interval;
static rt_u32 di_period;

static void on_signal(int signal_number) { (void)signal_number; interrupted = 1; }
static rt_u64 monotonic_ns(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (rt_u64)now.tv_sec * 1000000000U + (rt_u32)now.tv_nsec;
}
static void pause_ms(void)
{
    struct timespec pause = {0, 1000000};
    nanosleep(&pause, NULL);
}
static int snapshot(struct amp_acq_status *out)
{
    unsigned i, retry;
    for (retry = 0; retry < 1000; retry++) {
        rt_u32 seq = page->seq_lock;
        if (seq & 1U) continue;
        __sync_synchronize();
        for (i = 0; i < sizeof(*out) / 4; i++)
            ((rt_u32 *)out)[i] = ((volatile rt_u32 *)page)[i];
        __sync_synchronize();
        if (seq == page->seq_lock && seq == out->seq_lock)
            return out->magic == AMP_ACQ_MAGIC && out->version == AMP_ACQ_VERSION &&
                   out->bytes == sizeof(*out) && out->cpu == 3 ? 0 : -1;
    }
    return -1;
}
static int di_snapshot(struct amp_bus_di_status *out)
{
    volatile const struct amp_bus_di_status *src = (volatile const struct amp_bus_di_status *)
        ((volatile const char *)page + AMP_BUS_DI_STATUS_OFFSET);
    for (unsigned retry = 0; retry < 1000; retry++) {
        rt_u32 seq = src->seq_lock;
        if (seq & 1U) continue;
        __sync_synchronize();
        for (unsigned i = 0; i < sizeof(*out) / 4; i++)
            ((rt_u32 *)out)[i] = ((volatile const rt_u32 *)src)[i];
        __sync_synchronize();
        if (seq == src->seq_lock && seq == out->seq_lock)
            return out->magic == AMP_BUS_DI_MAGIC && out->version == AMP_BUS_DI_VERSION &&
                out->bytes == sizeof(*out) ? 0 : -1;
    }
    return -1;
}
static int di_consume(rt_u64 *frames, rt_u64 *previous_sequence, rt_u64 *previous_ticks)
{
    struct rt_sample_v1 sample;
    for (unsigned count = 0; count < 1024U; count++) {
        int popped = rt_ring_pop(&di_samples, &sample, sizeof(sample));
        rt_u64 seq, tick;
        if (popped == RT_EMPTY) return 0;
        if (popped != RT_OK) return -1;
        seq = rt_words_value(sample.sample_sequence); tick = rt_words_value(sample.sample_ticks);
        if (sample.session_epoch != remote_epoch || sample.config_generation != 1 || sample.analog_count || sample.valid_chip_mask ||
            (sample.flags & (RT_SAMPLE_ADC_VALID | RT_SAMPLE_ADC_ESTIMATED)) ||
            sample.digital_words != 1U || sample.digital_valid[0] != AMP_BUS_DI_VALID || sample.digital_valid[1] ||
            !sample.period_ticks || sample.period_ticks != di_period ||
            !(sample.flags & RT_SAMPLE_PAYLOAD_CRC) || rt_crc32(sample.analog, 176U) != sample.payload_crc32 ||
            seq != *previous_sequence + 1U || (*previous_sequence && tick <= *previous_ticks)) {
            fputs("Invalid/missing digital-only frame\n", stderr); return -1;
        }
        if (*previous_sequence) {
            rt_u64 interval = tick - *previous_ticks;
            if (!di_min_interval || interval < di_min_interval) di_min_interval = interval;
            if (interval > di_max_interval) di_max_interval = interval;
        }
        /* 采样期间只写已预触页/锁定的有界RAM；STOP后再落盘，避免磁盘阻塞填满环。 */
        if (di_capture_frames) {
            if (di_capture_used >= di_capture_capacity) {
                fputs("DI capture RAM full; stopping without overwriting\n", stderr); return -1;
            }
            di_capture_frames[di_capture_used++] = sample;
        }
        *previous_sequence = seq; *previous_ticks = tick; (*frames)++;
    }
    return 0;
}
static rt_u32 load(void *context, rt_u32 offset)
{
    rt_u32 value = *(volatile rt_u32 *)((char *)context + offset);
    __sync_synchronize();
    return value;
}
static void store(void *context, rt_u32 offset, rt_u32 value)
{
    __sync_synchronize();
    *(volatile rt_u32 *)((char *)context + offset) = value;
    __sync_synchronize();
}
static void from(void *context, void *out, rt_u32 offset, rt_u32 bytes)
{
    rt_u8 *dst = out;
    volatile const rt_u8 *src = (volatile rt_u8 *)context + offset;
    /* 共享记录四字节对齐时按整字搬运，避免数字专测被逐字节NC访问限速。 */
    typedef rt_u32 alias_u32 __attribute__((__may_alias__));
    if (!(((uintptr_t)dst | (uintptr_t)src) & 3U)) {
        while (bytes >= 4U) {
            *(alias_u32 *)dst = *(volatile const alias_u32 *)src;
            dst += 4; src += 4; bytes -= 4;
        }
    }
    while (bytes--) *dst++ = *src++;
    __sync_synchronize();
}
static void to(void *context, rt_u32 offset, const void *in, rt_u32 bytes)
{
    volatile rt_u8 *dst = (volatile rt_u8 *)context + offset;
    const rt_u8 *src = in;
    while (bytes--) *dst++ = *src++;
    __sync_synchronize();
}
static void host_alive(void)
{
    rt_u32 offset = RT_HOST_STATUS_OFF;
    rt_u32 seq = load(io.context, offset);
    store(io.context, offset, seq + 1);
    store(io.context, offset + offsetof(struct rt_peer_status, bytes), sizeof(struct rt_peer_status));
    store(io.context, offset + offsetof(struct rt_peer_status, session_epoch), host_epoch);
    store(io.context, offset + offsetof(struct rt_peer_status, heartbeat), ++request_id);
    store(io.context, offset, seq + 2);
}
static int bind(rt_u32 id, struct rt_ring *ring)
{
    struct rt_object object;
    int result = rt_find_object(&io, id, RT_FEATURES_V1, &object);
    return result ? result : rt_ring_bind(&io, &object, ring);
}
static int command(rt_u32 id, const void *payload, rt_u32 bytes)
{
    struct rt_message_v1 request = {0}, reply;
    rt_u64 started = monotonic_ns();
    request.id = id;
    request.request_id = ++request_id;
    request.session_epoch = remote_epoch;
    request.flags = RT_MSG_CRC_FLAG;
    request.payload_bytes = bytes;
    if (bytes) memcpy(request.payload, payload, bytes);
    request.payload_crc32 = rt_crc32(request.payload, bytes);
    if (rt_ring_push(&commands, &request, sizeof(request))) return -1;
    do {
        int result = rt_ring_pop(&events, &reply, sizeof(reply));
        if (!result) {
            if (reply.id != id || reply.request_id != request.request_id ||
                reply.session_epoch != remote_epoch || reply.flags != RT_MSG_ACK_FLAG)
                return -1;
            return reply.status;
        }
        if (result != RT_EMPTY) return -1;
        pause_ms();
    } while (monotonic_ns() - started < 1000000000U);
    return -1;
}

static int report_status(const struct amp_acq_status *state)
{
    FILE *report = fopen("/run/rt_mpu_a7_status.json.tmp", "w");
    if (!report) { perror("status file"); return -1; }
    if (fprintf(report, "{\"backend\":\"a7_amp\",\"pid\":%ld,\"monotonic_ns\":%" PRIu64
        ",\"epoch\":%u,\"run_generation\":%u,\"state\":%u,\"period_ticks\":%u,\"mask\":%u,"
        "\"qualified\":%u,\"first_error\":%d,\"missed_cycles\":%u,\"read_errors\":%u,\"ring_errors\":%u,\"busy_timeouts\":%u,\"errors\":%u,\"faults\":%u,\"read_count\":%u,\"read_max_ticks\":%u,"
        "\"read_sum_ticks\":%" PRIu64 ",\"sample_sequence\":%" PRIu64 ",\"native_ticks\":%" PRIu64 "}\n",
        (long)getpid(), monotonic_ns(), state->epoch, state->run_generation, state->state, state->period,
        state->enabled_mask, state->qualified, state->first_error, state->missed_cycles, state->read_errors, state->ring_errors, state->busy_timeouts, state->errors, state->faults, state->read_count,
        state->read_max_ticks, rt_words_value(state->read_sum_ticks), rt_words_value(state->sample_sequence), rt_words_value(state->ticks)) < 0) {
        fclose(report); return -1;
    }
    if (fclose(report) || rename("/run/rt_mpu_a7_status.json.tmp", "/run/rt_mpu_a7_status.json")) return -1;
    return 0;
}

#if AMP_REALTIME_IO
static int bootstrap_time(void)
{
    struct amp_rtio_snapshot aux;
    struct amp_acq_status native;
    struct timespec host;
    rt_u64 restored=0,last_report=0;
    int result=-1,waiting=0,fd=open(AMP_AUX_DEVICE,O_RDONLY|O_CLOEXEC);
    if(fd<0) { perror("open A7 RTC"); return -1; }
    while(!interrupted) {
        host_alive();
        if(snapshot(&native) || native.epoch!=remote_epoch || native.errors || native.faults ||
           clock_gettime(CLOCK_REALTIME,&host) || ioctl(fd,AMP_AUX_SNAPSHOT,&aux)) break;
        result=amp_aux_boot_utc(&aux,remote_epoch,(rt_u64)host.tv_sec,&restored);
        if(result==0) break;
        if(result==1) {
            host.tv_sec=(time_t)(restored/1000000000ULL);
            host.tv_nsec=(long)(restored%1000000000ULL);
            result=clock_settime(CLOCK_REALTIME,&host);
            if(result) perror("restore Linux UTC from A7 RTC");
            break;
        }
        /* 辅助页在PREPARE后异步公布；此阶段ADC没有ARM，不伪造任何采样。 */
        if(result<0 && result!=-116) break;
        if(!waiting) { fputs("Waiting for valid A7 RTC or external Linux UTC; ADC not armed\n",stderr); waiting=1; }
        if(monotonic_ns()-last_report>=1000000000ULL) {
            if(report_status(&native)) { result=-1; break; }
            last_report=monotonic_ns();
        }
        pause_ms();
    }
    close(fd);
    return interrupted ? -1 : result;
}
#endif

int main(int argc, char **argv)
{
    struct amp_acq_status state = {0};
    int fd = -1, samples_fd = -1, claimed = 0, result = 2, status_valid = 0;
    int capture_saved = 0;
    void *mapping = MAP_FAILED;
    rt_u32 mask = 0, raw_channel = 0;
    unsigned seconds = 0, rate = 6400;
    int hold = 0, running = 0, di_run = 0;
    struct amp_bus_di_status di = {0};
    rt_u64 di_previous_sequence = 0, di_previous_ticks = 0;
    rt_u64 di_progress_at = 0, di_progress_count = 0;
    const char *config_path = NULL, *capture_path = NULL;
    rt_u64 frames = 0;
    double squares = 0;
    if (argc == 2 && !strcmp(argv[1], "status")) {}
    else if ((argc == 4 || argc == 6) && !strcmp(argv[1], "run-di") &&
             !strcmp(argv[3], "--exclusive-commissioning")) {
        char *end;
        unsigned long value = strtoul(argv[2], &end, 10);
        if (*end || value < 1 || value > 1800) goto usage;
        seconds = (unsigned)value;
        running = di_run = 1;
        mask = 31;
        if (argc == 6) {
            if (strcmp(argv[4], "--capture")) goto usage;
            capture_path = argv[5];
        }
    }
    else if (argc == 4 && !strcmp(argv[1], "hold-config") &&
             !strcmp(argv[3], "--exclusive-commissioning")) {
        hold = running = 1;
        config_path = argv[2];
    }
    else if (((argc == 5 || argc == 6) && !strcmp(argv[1], "run")) ||
             (argc == 5 && !strcmp(argv[1], "hold"))) {
        char *end;
        unsigned long value;
        if (strcmp(argv[argc - 1], "--exclusive-commissioning")) goto usage;
        running = 1;
        hold = !strcmp(argv[1], "hold");
        value = strtoul(argv[2], &end, 10);
        if (hold) {
            if (*end || value > 25600U || !amp_rate_period((unsigned)value)) goto usage;
            rate = (unsigned)value;
        } else {
            if (*end || value < 1 || value > 1800) goto usage;
            seconds = (unsigned)value;
            if (argc == 6) {
                value = strtoul(argv[4], &end, 10);
                if (*end || value > 25600U || !amp_rate_period((unsigned)value)) goto usage;
                rate = (unsigned)value;
            }
        }
        value = strtoul(argv[3], &end, 10);
        if (*end || (value != 1 && value != 2 && value != 4 && value != 8 && value != 16 && value != 31)) goto usage;
        mask = (rt_u32)value;
        while (!(mask & (1U << (raw_channel / 16U)))) raw_channel += 16U;
    } else goto usage;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    fd = open(AMP_TEST_DEVICE, O_RDWR | O_SYNC | O_CLOEXEC);
    if (fd < 0) { perror("open A7 experiment access"); goto done; }
    page = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, AMP_ACQ_STATUS_ADDRESS);
    if (page == MAP_FAILED) { perror("mmap status"); goto done; }
    if (snapshot(&state)) { fputs("Invalid A7 candidate status\n", stderr); goto done; }
    status_valid = 1;
    if (!running) { result = 0; goto done; }
    if (di_run && (di_snapshot(&di) || !di.di_only || di.epoch != state.epoch)) {
        fputs("Requires explicit DI-only firmware\n", stderr); goto done;
    }
    if (di_run) {
        di_period = di.target_period_ticks;
        if (!di_period) { fputs("DI mode requires a fixed period\n", stderr); goto done; }
        if (capture_path) {
            rt_u64 capacity = ((rt_u64)seconds * 24000000U + di_period - 1U) / di_period + 2U * RT_SAMPLE_CAPACITY;
            size_t bytes;
            void *capture_memory;
            int allocated;
            /* 原帧保留256字节兼容性，单次捕获最多64MiB，不能因逻辑分析耗尽装置内存。 */
            if (capacity > 64U * 1024U * 1024U / sizeof(struct rt_sample_v1)) {
                fputs("DI capture exceeds 64MiB; reduce capture duration\n", stderr); goto done;
            }
            bytes = (size_t)capacity * sizeof(struct rt_sample_v1);
            allocated = posix_memalign(&capture_memory, 64U, bytes);
            if (allocated) { errno = allocated; perror("allocate DI capture"); goto done; }
            di_capture_frames = capture_memory;
            di_capture_capacity = capacity;
            if (mlock(di_capture_frames, bytes)) { perror("lock DI capture RAM"); goto done; }
            memset(di_capture_frames, 0, bytes);
            int capture_fd = open(capture_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (capture_fd < 0) { perror("create DI capture"); goto done; }
            di_capture = fdopen(capture_fd, "wb");
            if (!di_capture) { close(capture_fd); goto done; }
            if (setvbuf(di_capture, NULL, _IOFBF, 65536U)) goto done;
        }
    }
    if (state.state != RT_STATE_STOPPED || !state.stopped || state.run_generation) {
        fputs("Requires a fresh stopped A7 candidate\n", stderr); goto done;
    }
    if (ioctl(fd, AMP_TEST_CLAIM, 0)) { perror("claim native resources"); goto done; }
    claimed = 1;
    mapping = mmap(NULL, RT_REGION_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, AMP_ACQ_RING_ADDRESS);
    if (mapping == MAP_FAILED) { perror("mmap ring"); goto done; }
    io = (struct rt_io){mapping, RT_REGION_BYTES, load, store, from, to};
    if (bind(RT_OBJ_COMMAND, &commands) || bind(RT_OBJ_EVENT, &events)) goto done;
    if (di_run) {
        /* 取得既有单消费者所有权，阻止诊断worker抢读；独立测试直接读取同一样点环。
         * 数字专用帧不进入要求80路ADC的保护/网络接口，绝不伪造ADC_VALID。 */
        samples_fd = open(RT_SAMPLE_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
        if (samples_fd < 0 || bind(RT_OBJ_SAMPLE, &di_samples)) goto done;
    }
    remote_epoch = state.epoch;
    host_epoch = (rt_u32)monotonic_ns() | 1U;
    if (command(RT_MSG_HELLO, &host_epoch, sizeof(host_epoch))) goto done;
    host_alive();
    {
        struct rt_acq_config_v1 config = {0};
        struct rt_config_request selected;
        struct rt_soft_rtc_anchor anchor = {0};
        struct timespec utc;
        struct amp_start_request claim = {AMP_ACQ_VERSION, sizeof(claim), AMP_OWNER_REQUIRED, host_epoch};
        config.abi_version = RT_ABI_V1;
        config.bytes = sizeof(config);
        config.generation = 1;
        config.initial_period_ticks = amp_rate_period(rate);
        config.enabled_chip_mask = mask;
        config.samples_per_cycle = amp_rate_points(config.initial_period_ticks);
        memset(config.range_code, 3, sizeof(config.range_code));
        config.crc32 = rt_crc32(&config, sizeof(config));
        if (config_path) {
            FILE *input = fopen(config_path, "rb");
            int valid;
            if (!input) { perror("open acquisition config"); goto done; }
            valid = fread(&config, 1, sizeof(config), input) == sizeof(config) && fgetc(input) == EOF && !ferror(input);
            if (fclose(input) || !valid || config.bytes != sizeof(config) ||
                config.abi_version != RT_ABI_V1 ||
                amp_rate_points(config.initial_period_ticks) != config.samples_per_cycle) {
                fputs("Invalid acquisition configuration\n", stderr); goto done;
            }
        }
        to(mapping, RT_CONFIG0_OFF, &config, sizeof(config));
        selected = (struct rt_config_request){RT_CONFIG0_OFF, sizeof(config), config.generation, config.crc32};
        if (command(RT_MSG_APPLY_CONFIG, &selected, sizeof(selected))) goto done;
#if AMP_REALTIME_IO
        if (command(AMP_MSG_PREPARE_RTIO, &claim, sizeof(claim)) || bootstrap_time()) goto done;
#endif
        if (snapshot(&state) || clock_gettime(CLOCK_REALTIME, &utc)) goto done;
        anchor.captured_ticks = state.ticks;
        anchor.utc_seconds = rt_words_make((rt_u64)utc.tv_sec);
        anchor.utc_nanoseconds = (rt_u32)utc.tv_nsec;
        anchor.time_epoch = 1;
        anchor.source = RT_RTC_SOURCE_LINUX;
        anchor.uncertainty_ns = 2000000U;
        if (command(RT_MSG_SET_SOFT_RTC, &anchor, sizeof(anchor))) goto done;
        /* START应答不确定也必须由RELEASE走一次匹配ACK的STOP清理，不能重复ARM。 */
        if (command(RT_MSG_START, &claim, sizeof(claim))) goto done;
    }
    if (!hold && !di_run) {
        batch = calloc(1, RT_SAMPLE_DEVICE_MAP_BYTES);
        if (!batch) goto done;
        samples_fd = open(RT_SAMPLE_DEVICE_PATH, O_RDONLY | O_CLOEXEC);
        if (samples_fd < 0) { perror("open existing sample API"); goto done; }
    }
    {
        rt_u64 deadline = monotonic_ns() + (rt_u64)seconds * 1000000000U;
        rt_u64 last_report = 0;
        struct amp_native_progress progress = {0};
        while (!interrupted && (hold || monotonic_ns() < deadline)) {
            rt_u32 i;
            host_alive();
            if (snapshot(&state) || state.epoch != remote_epoch || state.faults || state.errors) goto done;
            if (!di_run && amp_native_progress_check(&progress, &state, monotonic_ns())) {
                fputs("A7 native clock/sample progress stalled or changed session; stopping once\n", stderr);
                goto done;
            }
            if (di_run) {
                rt_u64 now = monotonic_ns();
                if (di_consume(&frames, &di_previous_sequence, &di_previous_ticks)) goto done;
                if (frames != di_progress_count || !di_progress_at) { di_progress_count = frames; di_progress_at = now; }
                if (now - di_progress_at > 500000000U) goto done;
                if (state.state != RT_STATE_RUNNING || state.qualified ||
                    di_snapshot(&di) || di.epoch != remote_epoch || !di.di_only || di.errors) goto done;
                pause_ms();
                continue;
            }
            if (hold) {
                /* 控制端只续租，不争用APP/诊断线程的样点消费权；状态写tmpfs供网页只读。 */
                rt_u64 now = monotonic_ns();
                if (now - last_report >= 1000000000U) {
                    if (report_status(&state)) goto done;
                    last_report = now;
                }
                pause_ms();
                continue;
            }
            memset(&batch->header, 0, sizeof(batch->header));
            batch->header.version = RT_SAMPLE_DEVICE_VERSION;
            batch->header.header_bytes = sizeof(batch->header);
            batch->header.record_bytes = sizeof(struct rt_sample_v1);
            batch->header.max_frames = RT_SAMPLE_CAPACITY;
            if (ioctl(samples_fd, RT_SAMPLE_DEVICE_READ_BATCH, batch)) {
                if (errno == EAGAIN) { pause_ms(); continue; }
                perror("read batch"); goto done;
            }
            if (batch->header.flags & RT_SAMPLE_DEVICE_BATCH_FAULT) goto done;
            for (i = 0; i < batch->header.frame_count; i++) {
                double code = batch->samples[i].analog[raw_channel];
                squares += code * code;
            }
            frames += batch->header.frame_count;
            pause_ms();
        }
    }
    result = hold ? 0 : (frames && !interrupted ? 0 : 3);
done:
    /* 先STOP确认，再交还样点消费者，避免数字专用帧被普通ADC诊断worker误消费。 */
    if (claimed && ioctl(fd, AMP_TEST_RELEASE, 0)) { perror("retain resources: stop unconfirmed"); result = 3; }
    if (di_run && claimed && di_samples.capacity) {
        /* STOP后的末批仍在同一环里；归还消费权前核对所有成功扫描都已交付。 */
        if (di_consume(&frames, &di_previous_sequence, &di_previous_ticks) ||
            di_snapshot(&di) || frames != rt_words_value(di.scans) || rt_words_value(di.missed_periods)) result = 3;
    }
    if (samples_fd >= 0) close(samples_fd);
    if (mapping != MAP_FAILED) munmap(mapping, RT_REGION_BYTES);
    if (page && page != MAP_FAILED) {
        status_valid = snapshot(&state) == 0;
        if (!status_valid) result = 3;
        /* 最后一次批读与STOP之间也可能发生源错，不能仅凭已读到若干帧判通过。 */
        if (running && status_valid && (state.errors || state.faults || (!di_run && state.qualified != 128U))) result = 3;
        if (di_run && di_snapshot(&di)) result = 3;
        /* 退出也发布最终状态，网页不能把末次正常报告当成当前运行。 */
        if (hold && status_valid && report_status(&state)) result = 3;
        munmap((void *)page, 4096);
    }
    if (fd >= 0) close(fd);
    if (di_capture) {
        if (status_valid && state.stopped &&
            fwrite(di_capture_frames, sizeof(*di_capture_frames), (size_t)di_capture_used, di_capture) == di_capture_used &&
            !fflush(di_capture)) capture_saved = 1;
        else { perror("write stopped DI capture"); result = 3; }
        if (fclose(di_capture)) { perror("close DI capture"); result = 3; capture_saved = 0; }
    }
    if (di_capture_frames) {
        munlock(di_capture_frames, (size_t)di_capture_capacity * sizeof(*di_capture_frames));
        free(di_capture_frames);
    }
    free(batch);
    if (di_run) {
        rt_u64 scans = rt_words_value(di.scans), elapsed = rt_words_value(di.last_scan_ticks) - rt_words_value(di.first_scan_ticks);
        printf("{\"di_only\":true,\"target_period_ticks\":%u,\"raw\":%u,\"valid_after_stop\":%u,\"scans\":%" PRIu64
               ",\"frames_delivered\":%" PRIu64 ",\"scan_rate_hz\":%.3f,\"bus_mean_us\":%.3f,\"bus_max_us\":%.3f,"
               "\"min_scan_interval_us\":%.3f,\"max_scan_interval_us\":%.3f,\"missed_periods\":%" PRIu64 ",\"di_errors\":%u,\"adc_read_count\":%u,\"capture_saved\":%s,\"exit\":%d}\n",
               di.target_period_ticks,di.raw,di.valid,scans,frames,scans > 1 && elapsed ? (double)(scans - 1U)*24000000.0/elapsed : 0.0,
               scans ? rt_words_value(di.sum_ticks)/(24.0*scans) : 0.0,di.max_ticks/24.0,
               di_min_interval/24.0,di_max_interval/24.0,rt_words_value(di.missed_periods),di.errors,state.read_count,
               capture_saved ? "true" : "false",result);
    }
    printf("{\"exit\":%d,\"status_valid\":%s,\"state\":%u,\"qualified\":%u,\"frames_delivered\":%" PRIu64
           ",\"errors\":%u,\"first_error\":%d,\"bus_read_count\":%u,\"bus_read_max_us\":%.3f,"
           "\"bus_read_mean_us\":%.3f,\"raw_channel_index\":%u,\"raw_rms\":%.3f,\"physical_protection_tested\":false}\n",
           result, status_valid ? "true" : "false", state.state, state.qualified, frames, state.errors, state.first_error, state.read_count,
           state.read_max_ticks / 24.0, state.read_count ? rt_words_value(state.read_sum_ticks) / (24.0 * state.read_count) : 0,
           raw_channel, frames ? sqrt(squares / frames) : 0);
    return result;
usage:
    fputs("Usage: amp_acquisition_test status | run-di SECONDS --exclusive-commissioning [--capture NEW_FILE] | run SECONDS MASK [RATE] --exclusive-commissioning | hold RATE MASK --exclusive-commissioning | hold-config FILE --exclusive-commissioning\n"
          "Rates: 6400,12800; explicit high-rate build adds 16000,19200,20000. Explicit 512-point build adds nominal 25600 (938 ticks, actual 25586.354Hz). High rates require the full DTU app stopped.\n", stderr);
    return 2;
}
