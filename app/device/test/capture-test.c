/* 用实际采集状态机验证冷准备、回卷、满深度与错误提交，不模拟物理带宽。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../firmware/main.c"

enum {
    TEST_WORD_MASK = 0xffffU, /* 独立的逐点输入计数图案。 */
    TEST_COUNT = 4096U, /* 跨越四个提交批次。 */
    TEST_FAULT_INDEX = 2500U, /* 在第三批内部注入故障。 */
    TEST_PREFIX_COUNT = 2048U, /* 故障前已核验的完整前缀。 */
    TEST_READ_TICKS = 10U, /* 正常读取用时，留出等待余量。 */
    TEST_COLD_TICKS = 60U, /* 冷准备超过一微秒，不能冒充正式点。 */
    TEST_ROLLOVER_LOW = 0xfffffff0U, /* 起始点靠近低 32 位回卷。 */
    TEST_MODE_NORMAL, TEST_MODE_DEADLINE, TEST_MODE_STICKY
};
static uint64_t clock_tick;
static uint32_t formal_reads, test_mode, sticky_error, observed_count;

uint64_t l16_ticks(void) {
    return clock_tick++;
}
int l16_board_prepare(void) {
    return L16_OK;
}
int l16_board_check(void) {
    return sticky_error ? L16_BUS_ERROR : L16_OK;
}
uint16_t l16_board_probe_inputs(void) {
    return TEST_WORD_MASK;
}

int l16_board_read(uint16_t *word, volatile uint32_t *stop)
{
    uint32_t warming = local.priming_reads < L16_PRIMING_READS;
    assert(!*stop);
    assert(l16_status.count >= observed_count);
    assert(!(l16_status.count & (L16_PROGRESS_SAMPLES - 1U)));
    observed_count = l16_status.count;
    if (warming) {
        assert(l16_status.count == 0);
        clock_tick += TEST_COLD_TICKS;
        *word = TEST_WORD_MASK;
        return L16_OK;
    }
    *word = (uint16_t)(formal_reads & TEST_WORD_MASK);
    if (formal_reads == TEST_FAULT_INDEX && test_mode == TEST_MODE_DEADLINE)
        clock_tick += L16_MIN_PERIOD;
    else clock_tick += TEST_READ_TICKS;
    if (formal_reads == TEST_FAULT_INDEX && test_mode == TEST_MODE_STICKY) sticky_error = 1;
    formal_reads++;
    return L16_OK;
}

static void reset_test(uint32_t mode)
{
    memset(&local, 0, sizeof(local));
    memset((void *)&l16_status, 0, sizeof(l16_status));
    shutdown_requested = 0;
    formal_reads = 0;
    sticky_error = 0;
    observed_count = 0;
    test_mode = mode;
    clock_tick = TEST_ROLLOVER_LOW;
    local.magic = L16_MAGIC;
    local.version = L16_VERSION;
    local.bytes = sizeof(local);
    local.cpu = L16_CPU;
    local.clock_hz = L16_CLOCK;
    l16_status.command_gen = TEST_COUNT;
}

static void verify_words(uint32_t count)
{
    const uint16_t *samples = (const uint16_t *)(uintptr_t)L16_SAMPLE_ADDRESS;
    uint32_t index;
    for (index = 0; index < count; index++) assert(samples[index] == (index & TEST_WORD_MASK));
    assert(local.count == l16_status.count);
    assert(!(l16_status.seq & 1U));
    assert(local.priming_reads == L16_PRIMING_READS);
    assert(local.priming_max_ticks > L16_MIN_PERIOD);
    assert(l16_status.command_ack == l16_status.command_gen);
}

int main(void)
{
    void *samples = mmap((void *)(uintptr_t)L16_SAMPLE_ADDRESS, L16_SAMPLE_BYTES,
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    uint64_t first, last;
    assert(samples == (void *)(uintptr_t)L16_SAMPLE_ADDRESS);
    reset_test(TEST_MODE_NORMAL);
    acquire(L16_MIN_PERIOD, TEST_COUNT, 0);
    assert(local.state == L16_FULL && local.error == L16_OK && local.dropped == 0);
    verify_words(TEST_COUNT);
    first = ((uint64_t)local.first_hi << L16_U32_HIGH_SHIFT) | local.first_lo;
    last = ((uint64_t)local.last_hi << L16_U32_HIGH_SHIFT) | local.last_lo;
    assert(local.first_hi == 1U && last - first == (TEST_COUNT - 1U) * L16_MIN_PERIOD);

    enable_monitor();
    assert(local.live_status == L16_LIVE_ENABLED);
    monitor_once();
    assert((local.live_status & L16_LIVE_WORD_MASK) == TEST_COUNT);
    assert(local.live_status & L16_LIVE_VALID);
    assert(local.heartbeat == 1U && local.state == L16_FULL);
    verify_words(TEST_COUNT);
    sticky_error = 1;
    monitor_once();
    assert(local.live_status == (L16_BUS_ERROR << L16_LIVE_ERROR_SHIFT));
    assert(local.error == L16_OK && local.count == TEST_COUNT);
    observed_count = formal_reads;
    monitor_once();
    assert(formal_reads == observed_count);

    reset_test(TEST_MODE_DEADLINE);
    acquire(L16_MIN_PERIOD, TEST_COUNT, 0);
    assert(local.state == L16_FAULT && local.error == L16_DEADLINE && local.dropped == 1U);
    verify_words(TEST_FAULT_INDEX);

    reset_test(TEST_MODE_STICKY);
    acquire(L16_MIN_PERIOD, TEST_COUNT, 0);
    assert(local.state == L16_FAULT && local.error == L16_BUS_ERROR);
    verify_words(TEST_PREFIX_COUNT);
    assert(munmap(samples, L16_SAMPLE_BYTES) == 0);
    puts("PASS: cold preparation, tick rollover, full depth, fault prefix; live reads preserve frozen data and stop on bus error");
    return 0;
}
