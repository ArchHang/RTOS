/**
 * @file uart_pipeline.c
 * @brief Framed telemetry and CNN inference over the real F446 USART2 link.
 */
#include "uart_pipeline.h"

#include <float.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ai_app.h"
#include "audit_regressions.h"
#include "main.h"
#include "rtos.h"
#include "usart.h"

enum {
    PIPE_HEADER_SIZE = 7U, PIPE_CRC_SIZE = 2U, PIPE_SYNC_SIZE = 2U,
    PIPE_RX_CAPACITY = 1024U, PIPE_SLOT_COUNT = 4U, PIPE_REPLY_COUNT = 8U,
    PIPE_STATS_COUNT = 15U, PIPE_REPLY_SIZE = PIPE_STATS_COUNT * sizeof(uint32_t),
    PIPE_SYNC_FIRST = 0xA5U, PIPE_SYNC_SECOND = 0x5AU,
    PIPE_TRANSFORM = 1U, PIPE_INFER = 2U, PIPE_STATUS = 3U, PIPE_ERROR = 0xE0U,
    PIPE_ERROR_ARGUMENT = 1U, PIPE_ERROR_BUSY = 2U, PIPE_ERROR_MODEL = 3U,
};

_Static_assert(sizeof(float) == 4U && FLT_RADIX == 2 && FLT_MANT_DIG == 24,
               "wire logits require binary32");
_Static_assert(AI_OUTPUT_SIZE == 4, "wire contract has four logits");

typedef struct pipeline_stats {
    uint64_t rx_irq_cycles;
    uint64_t model_cycles;
    uint64_t parser_cycles;
    uint32_t rx_bytes;
    uint32_t rx_irq_peak;
    uint32_t rx_uart_errors;
    uint32_t rx_queue_drops;
    uint32_t bad_crc;
    uint32_t rejected;
    uint32_t transform_done;
    uint32_t infer_done;
    uint32_t tx_failures;
} pipeline_stats_t;

typedef struct pipeline_parser {
    uint8_t wire[PIPE_HEADER_SIZE + AI_INPUT_SIZE + PIPE_CRC_SIZE];
    uint16_t used;
    uint16_t expected;
    uint8_t sync_count;
} pipeline_parser_t;

typedef struct pipeline_job {
    uint32_t request_id;
    uint8_t slot;
} pipeline_job_t;

typedef struct pipeline_reply {
    uint32_t request_id;
    uint8_t opcode;
    uint8_t size;
    uint8_t payload[PIPE_REPLY_SIZE];
} pipeline_reply_t;

static pipeline_stats_t stats;
static pipeline_parser_t parser;
static bool is_ready;
static bool has_rx_gap;
static rtos_queue_t rx_queue, free_queue, job_queue, reply_queue;
static uint8_t rx_storage[PIPE_RX_CAPACITY];
static uint8_t free_storage[PIPE_SLOT_COUNT];
static uint8_t model_inputs[PIPE_SLOT_COUNT][AI_INPUT_SIZE];
static pipeline_job_t job_storage[PIPE_SLOT_COUNT];
static pipeline_reply_t reply_storage[PIPE_REPLY_COUNT];
static rtos_tcb_t model_tcb, tx_tcb;
static _Alignas(8) rtos_stack_t model_stack[768U], tx_stack[384U];

static uint16_t read_le16(const uint8_t *data)
{
    return (uint16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8U));
}

static uint32_t read_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8U) |
           ((uint32_t)data[2] << 16U) | ((uint32_t)data[3] << 24U);
}

static void write_le32(uint8_t *data, uint32_t value)
{
    for (uint32_t index = 0U; index < sizeof(value); ++index) {
        data[index] = (uint8_t)(value >> (index * 8U));
    }
}

static uint16_t wire_crc(const uint8_t *data, uint32_t size)
{
    uint32_t crc = 0xFFFFU;
    for (uint32_t index = 0U; index < size; ++index) {
        crc ^= (uint32_t)data[index] << 8U;
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = ((crc << 1U) ^ (((crc & 0x8000U) != 0U) ? 0x1021U : 0U)) & 0xFFFFU;
        }
    }
    return (uint16_t)crc;
}

static void note_tx_failure(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    ++stats.tx_failures;
    RTOS_PORT_EXIT_CRITICAL();
}

static void submit_reply(const pipeline_reply_t *reply)
{
    if (rtos_queue_send(&reply_queue, reply, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK) {
        note_tx_failure();
    }
}

static void reply_error(uint32_t request_id, uint32_t error)
{
    RTOS_PORT_ENTER_CRITICAL();
    ++stats.rejected;
    RTOS_PORT_EXIT_CRITICAL();
    pipeline_reply_t reply = { .request_id = request_id, .opcode = PIPE_ERROR, .size = 4U };
    write_le32(reply.payload, error);
    submit_reply(&reply);
}

static void reply_status(uint32_t request_id)
{
    RTOS_PORT_ENTER_CRITICAL();
    const uint32_t fields[PIPE_STATS_COUNT] = {
        stats.rx_bytes, (uint32_t)stats.rx_irq_cycles, stats.rx_irq_peak, stats.rx_uart_errors,
        stats.rx_queue_drops, stats.bad_crc, stats.rejected, stats.transform_done,
        stats.infer_done, (uint32_t)stats.model_cycles, (uint32_t)stats.parser_cycles,
        stats.tx_failures, (uint32_t)(stats.rx_irq_cycles >> 32U),
        (uint32_t)(stats.model_cycles >> 32U), (uint32_t)(stats.parser_cycles >> 32U),
    };
    RTOS_PORT_EXIT_CRITICAL();

    pipeline_reply_t reply = {
        .request_id = request_id, .opcode = 0x80U | PIPE_STATUS, .size = PIPE_REPLY_SIZE,
    };
    for (uint32_t index = 0U; index < PIPE_STATS_COUNT; ++index) {
        write_le32(&reply.payload[index * sizeof(uint32_t)], fields[index]);
    }
    submit_reply(&reply);
}

static void reply_transform(uint32_t request_id, const uint8_t *data, uint32_t size)
{
    uint32_t sum = 0U;
    uint32_t fnv = UINT32_C(2166136261);
    for (uint32_t index = 0U; index < size; ++index) {
        sum += data[index];
        fnv = (fnv ^ data[index]) * UINT32_C(16777619);
    }
    pipeline_reply_t reply = {
        .request_id = request_id, .opcode = 0x80U | PIPE_TRANSFORM, .size = 8U,
    };
    write_le32(reply.payload, sum);
    write_le32(&reply.payload[4U], fnv);
    ++stats.transform_done;
    submit_reply(&reply);
}

static void submit_inference(uint32_t request_id, const uint8_t *data)
{
    pipeline_job_t job = { .request_id = request_id };
    if (rtos_queue_recv(&free_queue, &job.slot, RTOS_NO_WAIT) != RTOS_OK) {
        reply_error(request_id, PIPE_ERROR_BUSY);
        return;
    }
    memcpy(model_inputs[job.slot], data, AI_INPUT_SIZE);
    if (rtos_queue_send(&job_queue, &job, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK) {
        if (rtos_queue_send(&free_queue, &job.slot, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK) {
            note_tx_failure();
        }
        reply_error(request_id, PIPE_ERROR_BUSY);
    }
}

static void dispatch_frame(const uint8_t *wire, uint32_t size)
{
    const uint32_t body_size = size - PIPE_CRC_SIZE;
    if (wire_crc(wire, body_size) != read_le16(&wire[body_size])) {
        ++stats.bad_crc;
        return;
    }
    const uint32_t length = read_le16(&wire[1U]);
    const uint32_t request_id = read_le32(&wire[3U]);
    const uint8_t *payload = &wire[PIPE_HEADER_SIZE];
    if ((wire[0] == PIPE_TRANSFORM) && (length <= 64U)) {
        reply_transform(request_id, payload, length);
    } else if ((wire[0] == PIPE_INFER) && (length == AI_INPUT_SIZE)) {
        submit_inference(request_id, payload);
    } else if ((wire[0] == PIPE_STATUS) && (length == 0U)) {
        reply_status(request_id);
    } else {
        reply_error(request_id, PIPE_ERROR_ARGUMENT);
    }
}

static bool is_header_supported(uint8_t opcode, uint16_t length)
{
    return ((opcode == PIPE_TRANSFORM) && (length <= 64U)) ||
           ((opcode == PIPE_INFER) && (length == AI_INPUT_SIZE)) ||
           ((opcode == PIPE_STATUS) && (length == 0U));
}

static void parse_byte(uint8_t byte)
{
    if (parser.sync_count < PIPE_SYNC_SIZE) {
        if ((parser.sync_count == 1U) && (byte == PIPE_SYNC_SECOND)) {
            parser.sync_count = PIPE_SYNC_SIZE;
        } else {
            parser.sync_count = (byte == PIPE_SYNC_FIRST) ? 1U : 0U;
        }
        return;
    }
    parser.wire[parser.used++] = byte;
    if (parser.used == PIPE_HEADER_SIZE) {
        uint16_t length = read_le16(&parser.wire[1U]);
        if (!is_header_supported(parser.wire[0], length)) {
            /* Reject an impossible length before it can consume the next frame. */
            ++stats.rejected;
            parser.used = 0U;
            parser.sync_count = 0U;
            return;
        }
        parser.expected = (uint16_t)(PIPE_HEADER_SIZE + length + PIPE_CRC_SIZE);
    }
    if ((parser.used >= PIPE_HEADER_SIZE) && (parser.used == parser.expected)) {
        dispatch_frame(parser.wire, parser.used);
        parser.used = 0U;
        parser.sync_count = 0U;
    }
}

static bool discard_gap(void)
{
    RTOS_PORT_ENTER_CRITICAL();
    const bool has_gap = has_rx_gap;
    RTOS_PORT_EXIT_CRITICAL();
    if (has_gap) {
        /* IRQ rejects new bytes while the parser discards the tainted backlog. */
        uint8_t byte;
        while (rtos_queue_recv(&rx_queue, &byte, RTOS_NO_WAIT) == RTOS_OK) {
        }
        RTOS_PORT_ENTER_CRITICAL();
        has_rx_gap = false;
        RTOS_PORT_EXIT_CRITICAL();
    }
    return has_gap;
}

void uart_pipeline_receive_irq(void)
{
    const uint32_t started = DWT->CYCCNT;
    const uint32_t status = USART2->SR;
    const uint32_t errors = USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE;
    if ((status & (USART_SR_RXNE | errors)) == 0U) {
        return;
    }
    const uint8_t byte = (uint8_t)USART2->DR; /* SR + DR clear RXNE and errors. */
    if (!is_ready) {
        /* The base BSP may have armed receive before the application owns USART2. */
        return;
    }
    if ((status & USART_SR_RXNE) != 0U) {
        ++stats.rx_bytes;
    }
    if ((status & errors) != 0U) {
        ++stats.rx_uart_errors;
        if (!has_rx_gap) {
            has_rx_gap = true;
            /* Wake an empty-queue parser now. It discards this byte with the tainted backlog. */
            const rtos_status_t wake = rtos_queue_send(&rx_queue, &byte, RTOS_NO_WAIT, RTOS_FALSE);
            if ((wake != RTOS_OK) && (wake != RTOS_ERR_TIMEOUT)) {
                ++stats.rx_queue_drops;
            }
        }
    } else if (has_rx_gap ||
               (rtos_queue_send(&rx_queue, &byte, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK)) {
        ++stats.rx_queue_drops;
        has_rx_gap = true;
    }
    const uint32_t elapsed = DWT->CYCCNT - started;
    stats.rx_irq_cycles += elapsed;
    if (elapsed > stats.rx_irq_peak) {
        stats.rx_irq_peak = elapsed;
    }
}

static void run_model(void *context)
{
    (void)context;
    pipeline_job_t job;
    while (rtos_queue_recv(&job_queue, &job, RTOS_WAIT_FOREVER) == RTOS_OK) {
        float logits[AI_OUTPUT_SIZE];
        uint32_t class_id;
        const uint32_t started = DWT->CYCCNT;
        const int32_t status = (int32_t)ai_forward_u8(model_inputs[job.slot], logits, &class_id);
        const uint32_t elapsed = DWT->CYCCNT - started;
        if (rtos_queue_send(&free_queue, &job.slot, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK) {
            note_tx_failure();
        }
        if (status != 0) {
            reply_error(job.request_id, PIPE_ERROR_MODEL);
            continue;
        }
        pipeline_reply_t reply = {
            .request_id = job.request_id, .opcode = 0x80U | PIPE_INFER, .size = 20U,
        };
        write_le32(reply.payload, class_id);
        for (uint32_t index = 0U; index < AI_OUTPUT_SIZE; ++index) {
            uint32_t bits;
            memcpy(&bits, &logits[index], sizeof(bits));
            write_le32(&reply.payload[(index + 1U) * sizeof(uint32_t)], bits);
        }
        RTOS_PORT_ENTER_CRITICAL();
        stats.model_cycles += elapsed;
        ++stats.infer_done;
        RTOS_PORT_EXIT_CRITICAL();
        submit_reply(&reply);
    }
    note_tx_failure();
}

static void send_reply(const pipeline_reply_t *reply)
{
    uint8_t wire[PIPE_SYNC_SIZE + PIPE_HEADER_SIZE + PIPE_REPLY_SIZE + PIPE_CRC_SIZE];
    wire[0] = PIPE_SYNC_FIRST;
    wire[1] = PIPE_SYNC_SECOND;
    wire[2] = reply->opcode;
    wire[3] = reply->size;
    wire[4] = 0U;
    write_le32(&wire[5U], reply->request_id);
    memcpy(&wire[PIPE_SYNC_SIZE + PIPE_HEADER_SIZE], reply->payload, reply->size);
    const uint32_t crc_offset = PIPE_SYNC_SIZE + PIPE_HEADER_SIZE + reply->size;
    const uint16_t crc = wire_crc(&wire[PIPE_SYNC_SIZE], PIPE_HEADER_SIZE + reply->size);
    wire[crc_offset] = (uint8_t)crc;
    wire[crc_offset + 1U] = (uint8_t)(crc >> 8U);
    if (HAL_UART_Transmit(&huart2, wire, (uint16_t)(crc_offset + PIPE_CRC_SIZE), 100U) != HAL_OK) {
        note_tx_failure();
    }
}

static void run_tx(void *context)
{
    (void)context;
    pipeline_reply_t reply;
    while (rtos_queue_recv(&reply_queue, &reply, RTOS_WAIT_FOREVER) == RTOS_OK) {
        send_reply(&reply);
    }
    note_tx_failure();
}

static bool setup_objects(void)
{
    if ((rtos_queue_init(&rx_queue, rx_storage, 1U, PIPE_RX_CAPACITY) != RTOS_OK) ||
        (rtos_queue_init(&free_queue, free_storage, 1U, PIPE_SLOT_COUNT) != RTOS_OK) ||
        (rtos_queue_init(&job_queue, job_storage, sizeof(pipeline_job_t), PIPE_SLOT_COUNT)
         != RTOS_OK) ||
        (rtos_queue_init(&reply_queue, reply_storage, sizeof(pipeline_reply_t), PIPE_REPLY_COUNT)
         != RTOS_OK)) {
        return false;
    }
    for (uint8_t slot = 0U; slot < PIPE_SLOT_COUNT; ++slot) {
        if (rtos_queue_send(&free_queue, &slot, RTOS_NO_WAIT, RTOS_FALSE) != RTOS_OK) {
            return false;
        }
    }
    return (rtos_task_create_static(&model_tcb, model_stack, 768U, run_model, NULL, 12U,
                                    "pipeline-model") == RTOS_OK) &&
           (rtos_task_create_static(&tx_tcb, tx_stack, 384U, run_tx, NULL, 10U,
                                    "pipeline-tx") == RTOS_OK);
}

void audit_pre_start(void)
{
    if (HAL_UART_AbortReceive(&huart2) != HAL_OK) {
        return;
    }
    huart2.Init.BaudRate = UART_PIPELINE_BAUD;
    if (HAL_UART_Init(&huart2) != HAL_OK) {
        return;
    }
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    is_ready = setup_objects();
}

void audit_regressions_task(void *context)
{
    (void)context;
    if (!is_ready) {
        (void)printf("PIPE|FAIL|startup\r\n");
        return;
    }
    (void)printf("PIPE|READY|baud=%lu\r\n", (unsigned long)UART_PIPELINE_BAUD);
    (void)USART2->SR;
    (void)USART2->DR;
    USART2->CR3 |= USART_CR3_EIE;
    USART2->CR1 |= USART_CR1_RXNEIE;
    for (;;) {
        uint8_t byte;
        const rtos_status_t status = rtos_queue_recv(&rx_queue, &byte, 50U);
        if (discard_gap() || (status != RTOS_OK)) {
            parser.used = 0U;
            parser.sync_count = 0U;
        } else {
            const uint32_t started = DWT->CYCCNT;
            parse_byte(byte);
            stats.parser_cycles += DWT->CYCCNT - started;
        }
    }
}
