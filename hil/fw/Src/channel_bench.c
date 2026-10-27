/**
 * @file channel_bench.c
 * @brief Equivalent payload work compares copy, pool+pointer queue and loan IPC.
 *
 * IPC rows omit payload fill/check for every path. E2E rows perform the same
 * direct fill and complete in-place verification for every message. Each batch
 * fills the configured capacity, then drains it. Interrupts remain enabled;
 * cycles include interrupts, barriers and loop overhead, not a claimed WCET.
 */
#include "audit_regressions.h"
#include "channel_fixture.h"

#define CH_ARENA_BYTES (16384U)
#define CH_MAX_CAPACITY (16U)
#define CH_BATCHES (128U)
#define CH_REPETITIONS (3U)

typedef enum ch_bench_path {
    CH_BENCH_COPY = 0,
    CH_BENCH_POOL,
    CH_BENCH_LOAN
} ch_bench_path_t;

typedef struct ch_bench_config {
    uint32_t bytes;
    uint32_t capacity;
    uint32_t end_to_end;
    ch_bench_path_t path;
} ch_bench_config_t;

typedef struct ch_bench_result {
    uint32_t valid;
    uint32_t cycles;
    uint32_t ticks;
} ch_bench_result_t;

static uint8_t queue_storage[CH_ARENA_BYTES] __attribute__((aligned(8)));
/* Pool and loan paths run sequentially; neither owns this arena during the other. */
static uint8_t arena[CH_ARENA_BYTES] __attribute__((aligned(8)));
/* Copy producer/consumer run in separate batch phases, so one scratch is sufficient. */
static uint8_t scratch[CH_ARENA_BYTES] __attribute__((aligned(8)));
static void *addresses[CH_MAX_CAPACITY];
static rtos_channel_slot_t slots[CH_MAX_CAPACITY];
static rtos_channel_t channel;
static rtos_queue_t queue;
/* Pool registration retains its descriptor, so this object has static lifetime. */
static rtos_mempool_t pool;

static uint32_t run_copy_batch(const ch_bench_config_t *config, uint32_t round)
{
    for (uint32_t index = 0U; index < config->capacity; index++) {
        if (config->end_to_end != 0U) {
            ch_fill(scratch, config->bytes, round * config->capacity + index);
        }
        if (rtos_queue_send(&queue, scratch, 0U, RTOS_FALSE) != RTOS_OK) {
            return 0U;
        }
    }
    for (uint32_t index = 0U; index < config->capacity; index++) {
        if (rtos_queue_recv(&queue, scratch, 0U) != RTOS_OK) {
            return 0U;
        }
        if (config->end_to_end != 0U && ch_verify(scratch, config->bytes,
            round * config->capacity + index) == 0U) {
            return 0U;
        }
    }
    return 1U;
}

static uint32_t run_pool_batch(const ch_bench_config_t *config, uint32_t round)
{
    for (uint32_t index = 0U; index < config->capacity; index++) {
        void *block = rtos_mempool_alloc(&pool);
        if (block == NULL) {
            return 0U;
        }
        addresses[index] = block;
        if (config->end_to_end != 0U) {
            ch_fill(block, config->bytes, round * config->capacity + index);
        }
        if (rtos_queue_send(&queue, &block, 0U, RTOS_FALSE) != RTOS_OK) {
            /* The queue never acquired this pointer if send failed. */
            ch_expect("bench_failed_send_pool_cleanup", rtos_mempool_free(&pool, block)
                == RTOS_OK);
            return 0U;
        }
    }
    for (uint32_t index = 0U; index < config->capacity; index++) {
        void *block = NULL;
        if (rtos_queue_recv(&queue, &block, 0U) != RTOS_OK) {
            return 0U;
        }
        uint32_t valid = block == addresses[index];
        if (config->end_to_end != 0U && valid != 0U) {
            valid &= ch_verify(block, config->bytes, round * config->capacity + index);
        }
        if (rtos_mempool_free(&pool, block) != RTOS_OK || valid == 0U) {
            return 0U;
        }
    }
    return 1U;
}

static uint32_t run_loan_batch(const ch_bench_config_t *config, uint32_t round)
{
    for (uint32_t index = 0U; index < config->capacity; index++) {
        rtos_channel_writer_t writer = {0};
        if (rtos_channel_acquire(&channel, &writer, 0U) != RTOS_OK) {
            return 0U;
        }
        addresses[index] = writer.data;
        if (config->end_to_end != 0U) {
            ch_fill(writer.data, config->bytes, round * config->capacity + index);
        }
        if (rtos_channel_publish(&channel, &writer, config->bytes, index) != RTOS_OK) {
            ch_expect("bench_failed_publish_loan_cleanup", rtos_channel_cancel(&channel,
                &writer) == RTOS_OK);
            return 0U;
        }
    }
    for (uint32_t index = 0U; index < config->capacity; index++) {
        rtos_channel_reader_t reader = {0};
        if (rtos_channel_receive(&channel, &reader, 0U) != RTOS_OK) {
            return 0U;
        }
        uint32_t valid = reader.data == addresses[index] && reader.tag == index
            && reader.length == config->bytes;
        if (config->end_to_end != 0U && valid != 0U) {
            valid &= ch_verify(reader.data, config->bytes, round * config->capacity + index);
        }
        if (rtos_channel_release(&channel, &reader) != RTOS_OK || valid == 0U) {
            return 0U;
        }
    }
    return 1U;
}

static uint32_t prepare_path(const ch_bench_config_t *config)
{
    rtos_status_t status = RTOS_ERR_PARAM;

    switch (config->path) {
        case CH_BENCH_COPY:
            ch_fill(scratch, config->bytes, 0U);
            status = rtos_queue_init(&queue, queue_storage, config->bytes, config->capacity);
            break;
        case CH_BENCH_POOL:
            status = rtos_mempool_init(&pool, arena, config->bytes, config->capacity);
            if (status == RTOS_OK) {
                status = rtos_queue_init(&queue, queue_storage, sizeof(void *),
                    config->capacity);
            }
            break;
        case CH_BENCH_LOAN: {
            rtos_channel_config_t channel_config = {arena, sizeof(arena), slots,
                sizeof(slots), config->bytes, config->capacity};
            status = rtos_channel_init(&channel, &channel_config);
            break;
        }
        default:
            break;
    }
    ch_expect("bench_path_prepared", status == RTOS_OK);
    return status == RTOS_OK;
}

static void release_path(const ch_bench_config_t *config)
{
    if (config->path == CH_BENCH_LOAN) {
        rtos_channel_stats_t stats = {0};
        uint32_t messages = CH_BATCHES * config->capacity;
        ch_expect("bench_loan_ledger_read", rtos_channel_get_stats(&channel, &stats)
            == RTOS_OK);
        ch_expect("bench_loan_all_four_apis_counted", stats.acquired == messages
            && stats.published == messages && stats.received == messages
            && stats.released == messages && stats.free == config->capacity
            && stats.writing == 0U && stats.queued == 0U && stats.reading == 0U
            && ch_conserved(&stats));
        ch_expect("bench_loan_close", rtos_channel_close(&channel) == RTOS_OK);
        ch_expect("bench_loan_deinit", rtos_channel_deinit(&channel) == RTOS_OK);
    } else {
        ch_expect("bench_queue_drained", rtos_queue_get_count(&queue) == 0U);
        ch_expect("bench_queue_deinit", rtos_queue_deinit(&queue) == RTOS_OK);
        if (config->path == CH_BENCH_POOL) {
            ch_expect("bench_pool_all_returned", rtos_mempool_get_free(&pool)
                == config->capacity);
        }
    }
}

static ch_bench_result_t measure_path(const ch_bench_config_t *config)
{
    ch_bench_result_t result = {1U, 0U, 0U};
    rtos_tick_t start_tick = rtos_sched_get_tick_count();
    __DSB();
    __ISB();
    uint32_t start_cycle = DWT->CYCCNT;

    for (uint32_t round = 0U; round < CH_BATCHES; round++) {
        uint32_t valid;
        switch (config->path) {
            case CH_BENCH_COPY:
                valid = run_copy_batch(config, round);
                break;
            case CH_BENCH_POOL:
                valid = run_pool_batch(config, round);
                break;
            case CH_BENCH_LOAN:
                valid = run_loan_batch(config, round);
                break;
            default:
                valid = 0U;
                break;
        }
        if (valid == 0U) {
            result.valid = 0U;
            break;
        }
    }
    __DSB();
    result.cycles = DWT->CYCCNT - start_cycle;
    result.ticks = rtos_sched_get_tick_count() - start_tick;
    /* At 168MHz a one-second budget excludes any multiple CYCCNT wrap. */
    result.valid &= result.ticks < 1000U && result.cycles != 0U;
    return result;
}

static const char *path_name(ch_bench_path_t path)
{
    switch (path) {
        case CH_BENCH_COPY:
            return "copy";
        case CH_BENCH_POOL:
            return "pool";
        case CH_BENCH_LOAN:
            return "loan";
        default:
            return "invalid";
    }
}

static void run_case(const ch_bench_config_t *config, uint32_t repetition)
{
    if (prepare_path(config) == 0U) {
        return;
    }
    ch_bench_result_t result = measure_path(config);
    uint32_t messages = CH_BATCHES * config->capacity;
    ch_expect("bench_api_and_payload_valid", result.valid != 0U);
    (void)printf("CH|BENCH|path=%s bytes=%lu capacity=%lu mode=%s repetition=%lu "
        "messages=%lu cycles=%lu cycles_per_message=%lu ticks=%lu valid=%lu\r\n",
        path_name(config->path), (unsigned long)config->bytes,
        (unsigned long)config->capacity, config->end_to_end != 0U ? "e2e" : "ipc",
        (unsigned long)repetition, (unsigned long)messages, (unsigned long)result.cycles,
        (unsigned long)(result.cycles / messages), (unsigned long)result.ticks,
        (unsigned long)result.valid);
    release_path(config);
}

static void run_shape(uint32_t bytes, uint32_t capacity)
{
    ch_expect("bench_shape_fits_fixed_arenas", bytes <= CH_ARENA_BYTES / capacity);
    (void)printf("CH|CASE|bytes=%lu capacity=%lu batches=%u messages=%lu "
        "repetitions=%u modes=ipc,e2e paths=copy,pool,loan\r\n", (unsigned long)bytes,
        (unsigned long)capacity, (unsigned int)CH_BATCHES,
        (unsigned long)(CH_BATCHES * capacity), (unsigned int)CH_REPETITIONS);
    for (uint32_t end_to_end = 0U; end_to_end < 2U; end_to_end++) {
        for (uint32_t repetition = 0U; repetition < CH_REPETITIONS; repetition++) {
            /* Rotate path order to expose IRQ phase bias across repeats. */
            for (uint32_t path = 0U; path < 3U; path++) {
                ch_bench_config_t config = {bytes, capacity, end_to_end,
                    (ch_bench_path_t)((path + repetition) % 3U)};
                run_case(&config, repetition);
            }
        }
    }
}

/** @brief State benchmark work equivalence and real cycle timing limitations. */
void audit_pre_start(void)
{
    (void)printf("CH|META|mode=bench sizes=16,64,256,1024 capacities=1,3,16 "
        "large_shapes=4096x1,4096x3,16384x1 "
        "paths=copy,pool,loan work=ipc,e2e repetitions=%u batches=%u "
        "interrupts=enabled cycle_counter=DWT core_hz=%lu rows=270\r\n",
        (unsigned int)CH_REPETITIONS, (unsigned int)CH_BATCHES,
        (unsigned long)SystemCoreClock);
}

/** @brief Run equal message counts through every path within the fixed 16KiB arenas. */
void audit_regressions_task(void *argument)
{
    (void)argument;
    static const uint32_t sizes[] = {16U, 64U, 256U, 1024U};
    static const uint32_t capacities[] = {1U, 3U, 16U};

    rtos_perf_init_cycles();
    ch_expect("bench_real_dwt_enabled", (CoreDebug->DEMCR & CoreDebug_DEMCR_TRCENA_Msk)
        != 0U && (DWT->CTRL & DWT_CTRL_CYCCNTENA_Msk) != 0U);
    for (uint32_t size = 0U; size < sizeof(sizes) / sizeof(sizes[0]); size++) {
        for (uint32_t capacity = 0U; capacity < sizeof(capacities) / sizeof(capacities[0]);
            capacity++) {
            run_shape(sizes[size], capacities[capacity]);
        }
    }
    run_shape(4096U, 1U);
    run_shape(4096U, 3U);
    run_shape(16384U, 1U);
    ch_finish("bench");
}
