/**
 * @file rtos_channel.c
 * @brief Static loan channels with generation-checked ownership transitions.
 *
 * Payload bytes remain in caller storage. All metadata transitions and output
 * descriptors are committed under the kernel critical section. A sleeping
 * operation retains an integer address and incarnation, never a dereferenceable
 * object reference: close/deinit/reinit may happen before that operation runs.
 */
#include "rtos_channel.h"

#if RTOS_CONFIG_USE_CHANNEL

#include "rtos_internal.h"
#include "rtos_task.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(RTOS_CONFIG_CHANNEL_MAX_CHANNELS > 0U, "Channel registry must be nonempty");
_Static_assert(RTOS_CONFIG_CHANNEL_MAX_BLOCKS > 0U, "Channel block limit must be nonzero");
_Static_assert(RTOS_CONFIG_CHANNEL_MAX_CHANNELS <= UINT32_MAX / 3U,
               "Channel region count must fit uint32_t");

#define CHANNEL_REGION_CAPACITY (3U * RTOS_CONFIG_CHANNEL_MAX_CHANNELS)

typedef struct channel_region {
    uintptr_t begin;
    uintptr_t end;
} channel_region_t;

typedef struct channel_owned_region {
    uintptr_t begin;
    uintptr_t end;
    rtos_channel_t *owner;
} channel_owned_region_t;

/* Nonempty, disjoint half-open spans, sorted by begin. Endpoints are also
 * increasing. Extents include every declared byte, including unused tails. */
static channel_owned_region_t channel_regions[CHANNEL_REGION_CAPACITY];
static uint32_t channel_region_count;
static uint64_t channel_last_incarnation;

static void channel_increment(uint64_t *counter)
{
    if (*counter != UINT64_MAX) {
        (*counter)++;
    }
}

static rtos_status_t channel_context_permitted(rtos_bool_t allow_isr)
{
    if (RTOS_PORT_IN_ISR() == RTOS_FALSE) {
        return RTOS_OK;
    }
    if ((allow_isr == RTOS_FALSE) ||
        (rtos_port_isr_priority() < RTOS_CONFIG_MAX_SYSCALL_INTERRUPT_PRIORITY)) {
        return RTOS_ERR_ISR;
    }
    return RTOS_OK;
}

/* Caller holds the critical section. Only the control span has begin equal to
 * its owner address; data/metadata spans cannot impersonate a control block. */
static rtos_channel_t *channel_find(uintptr_t address)
{
    uint32_t first = 0U;
    uint32_t last = channel_region_count;

    while (first < last) {
        uint32_t middle = first + (last - first) / 2U;
        if (channel_regions[middle].begin < address) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    if ((first == channel_region_count) || (channel_regions[first].begin != address) ||
        ((uintptr_t)channel_regions[first].owner != address)) {
        return NULL;
    }
    return channel_regions[first].owner;
}

static rtos_bool_t channel_make_region(const void *base, size_t size,
                                       channel_region_t *region)
{
    uintptr_t begin = (uintptr_t)base;
    if ((base == NULL) || (size == 0U) || (size > UINTPTR_MAX - begin)) {
        return RTOS_FALSE;
    }
    region->begin = begin;
    region->end = begin + size;
    return RTOS_TRUE;
}

static rtos_bool_t channel_regions_overlap(const channel_region_t *left,
                                           const channel_region_t *right)
{
    return ((left->begin < right->end) && (right->begin < left->end))
        ? RTOS_TRUE : RTOS_FALSE;
}

static rtos_bool_t channel_storage_product(size_t count, size_t element_size, size_t *size)
{
    if ((element_size == 0U) || (count > SIZE_MAX / element_size)) {
        return RTOS_FALSE;
    }
    *size = count * element_size;
    return RTOS_TRUE;
}

/* Disjoint spans sorted by begin also have increasing ends. The first span
 * ending after query.begin is the only candidate needed for an overlap test. */
static rtos_bool_t channel_index_overlaps(const channel_region_t *query)
{
    uint32_t first = 0U;
    uint32_t last = channel_region_count;

    while (first < last) {
        uint32_t middle = first + (last - first) / 2U;
        if (channel_regions[middle].end <= query->begin) {
            first = middle + 1U;
        } else {
            last = middle;
        }
    }
    return ((first < channel_region_count) && (channel_regions[first].begin < query->end))
        ? RTOS_TRUE : RTOS_FALSE;
}

/* Initialization has validated all three spans and reserved their capacity.
 * Insertion cannot fail and never inspects the owner or payload contents. */
static void channel_insert_region(const channel_region_t *region, rtos_channel_t *owner)
{
    uint32_t position = channel_region_count;

    while ((position != 0U) && (channel_regions[position - 1U].begin > region->begin)) {
        channel_regions[position] = channel_regions[position - 1U];
        position--;
    }
    channel_regions[position].begin = region->begin;
    channel_regions[position].end = region->end;
    channel_regions[position].owner = owner;
    channel_region_count++;
}

/* Descriptor/configuration storage cannot alias any registered owned region. */
static rtos_bool_t channel_external_view(const void *view, size_t size, size_t alignment)
{
    channel_region_t view_region;
    if ((((uintptr_t)view % alignment) != 0U) ||
        (channel_make_region(view, size, &view_region) == RTOS_FALSE)) {
        return RTOS_FALSE;
    }
    return channel_index_overlaps(&view_region) == RTOS_FALSE ? RTOS_TRUE : RTOS_FALSE;
}

static rtos_bool_t channel_configuration_valid(uintptr_t address,
                                               const rtos_channel_config_t *config,
                                               channel_region_t regions[3])
{
    if ((config->block_size == 0U) || (config->block_count == 0U) ||
        (config->block_count > RTOS_CONFIG_CHANNEL_MAX_BLOCKS) ||
        (config->block_size > UINT32_MAX - (RTOS_CHANNEL_ALIGNMENT - 1U)) ||
        ((address % _Alignof(rtos_channel_t)) != 0U) ||
        (((uintptr_t)config->slots % _Alignof(rtos_channel_slot_t)) != 0U) ||
        (((uintptr_t)config->storage % RTOS_CHANNEL_ALIGNMENT) != 0U)) {
        return RTOS_FALSE;
    }
    uint32_t stride = (config->block_size + RTOS_CHANNEL_ALIGNMENT - 1U)
        & ~(RTOS_CHANNEL_ALIGNMENT - 1U);
    size_t storage_needed;
    size_t slots_needed;
    if ((channel_storage_product(config->block_count, stride, &storage_needed) == RTOS_FALSE) ||
        (channel_storage_product(config->block_count, sizeof(rtos_channel_slot_t),
                                  &slots_needed) == RTOS_FALSE) ||
        (config->storage_size < storage_needed) || (config->slots_size < slots_needed) ||
        (channel_make_region((const void *)address, sizeof(rtos_channel_t),
                             &regions[0]) == RTOS_FALSE) ||
        (channel_make_region(config->storage, config->storage_size,
                             &regions[1]) == RTOS_FALSE) ||
        (channel_make_region(config->slots, config->slots_size,
                             &regions[2]) == RTOS_FALSE)) {
        return RTOS_FALSE;
    }
    for (uint32_t first = 0U; first < 3U; first++) {
        for (uint32_t second = first + 1U; second < 3U; second++) {
            if (channel_regions_overlap(&regions[first], &regions[second]) == RTOS_TRUE) {
                return RTOS_FALSE;
            }
        }
    }
    return RTOS_TRUE;
}

rtos_status_t rtos_channel_init(rtos_channel_t *channel, const rtos_channel_config_t *config)
{
    if (channel_context_permitted(RTOS_FALSE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((channel == NULL) || (config == NULL)) {
        return RTOS_ERR_PARAM;
    }

    RTOS_PORT_ENTER_CRITICAL();
    if (channel_find((uintptr_t)channel) != NULL) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_ALREADY;
    }
    if (channel_external_view(config, sizeof(*config), _Alignof(rtos_channel_config_t))
        == RTOS_FALSE) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    rtos_channel_config_t configuration = *config;
    channel_region_t regions[3];
    if (channel_configuration_valid((uintptr_t)channel, &configuration, regions) == RTOS_FALSE) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    channel_region_t config_region;
    if (channel_make_region(config, sizeof(*config), &config_region) == RTOS_FALSE) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    for (uint32_t region = 0U; region < 3U; region++) {
        if (channel_regions_overlap(&config_region, &regions[region]) == RTOS_TRUE) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_ERR_PARAM;
        }
    }

    for (uint32_t region = 0U; region < 3U; region++) {
        if (channel_index_overlaps(&regions[region]) == RTOS_TRUE) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_ERR_PARAM;
        }
    }
    if (channel_region_count > CHANNEL_REGION_CAPACITY - 3U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_NO_MEM;
    }
    if (channel_last_incarnation == UINT64_MAX) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_OVERFLOW;
    }

    memset(channel, 0, sizeof(*channel));
    channel->incarnation = ++channel_last_incarnation;
    channel->storage = (uint8_t *)configuration.storage;
    channel->slots = configuration.slots;
    channel->block_size = configuration.block_size;
    channel->stride = (configuration.block_size + RTOS_CHANNEL_ALIGNMENT - 1U)
        & ~(RTOS_CHANNEL_ALIGNMENT - 1U);
    channel->stats.capacity = configuration.block_count;
    channel->stats.free = configuration.block_count;
    channel->free_head = 0U;
    channel->ready_head = RTOS_CHANNEL_INDEX_NONE;
    channel->ready_tail = RTOS_CHANNEL_INDEX_NONE;
    for (uint32_t index = 0U; index < configuration.block_count; index++) {
        rtos_channel_slot_t *slot = &configuration.slots[index];
        memset(slot, 0, sizeof(*slot));
        slot->state = RTOS_CHANNEL_SLOT_FREE;
        slot->next = index + 1U < configuration.block_count
            ? index + 1U : RTOS_CHANNEL_INDEX_NONE;
    }
    for (uint32_t region = 0U; region < 3U; region++) {
        channel_insert_region(&regions[region], channel);
    }
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

static void *channel_slot_data(const rtos_channel_t *channel, uint32_t index)
{
    return channel->storage + (size_t)index * channel->stride;
}

static void channel_fill_ticket(const rtos_channel_t *channel, uint32_t index,
                                 rtos_channel_ticket_t *ticket)
{
    ticket->incarnation = channel->incarnation;
    ticket->generation = channel->slots[index].generation;
    ticket->channel_address = (uintptr_t)channel;
    ticket->slot = index;
}

static rtos_status_t channel_take_free(rtos_channel_t *channel, rtos_channel_writer_t *writer)
{
    uint32_t index = channel->free_head;
    if ((index >= channel->stats.capacity) ||
        (channel->slots[index].state != RTOS_CHANNEL_SLOT_FREE)) {
        return RTOS_ERR_PARAM;
    }
    if (channel->next_ticket > UINT64_MAX - 2U) {
        return RTOS_ERR_OVERFLOW;
    }

    rtos_channel_slot_t *slot = &channel->slots[index];
    channel->free_head = slot->next;
    slot->generation = channel->next_ticket + 1U;
    channel->next_ticket += 2U;
    slot->next = RTOS_CHANNEL_INDEX_NONE;
    slot->length = 0U;
    slot->tag = 0U;
    slot->sequence = 0U;
    slot->state = RTOS_CHANNEL_SLOT_WRITING;
    channel->stats.free--;
    channel->stats.writing++;
    uint32_t in_use = channel->stats.capacity - channel->stats.free;
    if (in_use > channel->stats.in_use_peak) {
        channel->stats.in_use_peak = in_use;
    }
    channel_increment(&channel->stats.acquired);
    memset(writer, 0, sizeof(*writer));
    channel_fill_ticket(channel, index, &writer->ticket);
    writer->data = channel_slot_data(channel, index);
    writer->capacity = channel->block_size;
    return RTOS_OK;
}

static rtos_status_t channel_take_ready(rtos_channel_t *channel, rtos_channel_reader_t *reader)
{
    uint32_t index = channel->ready_head;
    if ((index >= channel->stats.capacity) ||
        (channel->slots[index].state != RTOS_CHANNEL_SLOT_QUEUED) ||
        (channel->slots[index].generation == UINT64_MAX)) {
        return RTOS_ERR_PARAM;
    }

    rtos_channel_slot_t *slot = &channel->slots[index];
    channel->ready_head = slot->next;
    if (channel->ready_head == RTOS_CHANNEL_INDEX_NONE) {
        channel->ready_tail = RTOS_CHANNEL_INDEX_NONE;
    }
    slot->next = RTOS_CHANNEL_INDEX_NONE;
    slot->generation++;
    slot->state = RTOS_CHANNEL_SLOT_READING;
    channel->stats.queued--;
    channel->stats.reading++;
    channel_increment(&channel->stats.received);
    memset(reader, 0, sizeof(*reader));
    channel_fill_ticket(channel, index, &reader->ticket);
    reader->data = channel_slot_data(channel, index);
    reader->length = slot->length;
    reader->tag = slot->tag;
    reader->sequence = slot->sequence;
    return RTOS_OK;
}

static rtos_bool_t channel_timeout_valid(rtos_tick_t timeout)
{
    return ((timeout <= 0x7FFFFFFFU) || (timeout == RTOS_WAIT_FOREVER))
        ? RTOS_TRUE : RTOS_FALSE;
}

/* Every object read is preceded by registry lookup and incarnation validation.
 * After EXIT only the still-running task's wait_result may be read. An old
 * address cannot revive a blocked operation when a new channel occupies it. */
static rtos_status_t channel_borrow(uintptr_t address, void *view, rtos_tick_t timeout,
                                   rtos_bool_t receive)
{
    uint64_t incarnation = 0U;
    rtos_tick_t started = 0U;
    rtos_status_t wake_result = RTOS_OK;
    rtos_bool_t waited = RTOS_FALSE;
    uint32_t caller_mask = rtos_port_get_interrupt_mask();
    uint32_t caller_global_mask = rtos_port_get_global_interrupt_mask();

    for (;;) {
        RTOS_PORT_ENTER_CRITICAL();
        rtos_channel_t *channel = channel_find(address);
        if ((channel == NULL) ||
            ((incarnation != 0U) && (channel->incarnation != incarnation))) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, RTOS_ERR_DELETED);
        }
        size_t view_size = receive == RTOS_TRUE
            ? sizeof(rtos_channel_reader_t) : sizeof(rtos_channel_writer_t);
        size_t view_alignment = receive == RTOS_TRUE
            ? _Alignof(rtos_channel_reader_t) : _Alignof(rtos_channel_writer_t);
        if (channel_external_view(view, view_size, view_alignment) == RTOS_FALSE) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, RTOS_ERR_PARAM);
        }
        if (incarnation == 0U) {
            incarnation = channel->incarnation;
            started = rtos_kernel.tick_count;
        }
        if (wake_result != RTOS_OK) {
            if (wake_result == RTOS_ERR_TIMEOUT) {
                channel_increment(receive == RTOS_TRUE ? &channel->stats.receive_timeouts
                                                       : &channel->stats.acquire_timeouts);
            } else if (wake_result == RTOS_ERR_CLOSED) {
                channel_increment(&channel->stats.closed_rejections);
            }
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, wake_result);
        }
        if ((channel->stats.closed != 0U) &&
            ((receive == RTOS_FALSE) || (channel->stats.queued == 0U))) {
            channel_increment(&channel->stats.closed_rejections);
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, RTOS_ERR_CLOSED);
        }
        /* A wakeup does not reserve a block. Once waiting began, neither late
         * availability nor delayed CPU service may extend the grant window. */
        if ((waited == RTOS_TRUE) && (timeout != RTOS_WAIT_FOREVER) &&
            ((rtos_tick_t)(rtos_kernel.tick_count - started) >= timeout)) {
            channel_increment(receive == RTOS_TRUE ? &channel->stats.receive_timeouts
                                                   : &channel->stats.acquire_timeouts);
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, RTOS_ERR_TIMEOUT);
        }
        if ((receive == RTOS_FALSE) && (channel->next_ticket > UINT64_MAX - 2U)) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, RTOS_ERR_OVERFLOW);
        }
        if (((receive == RTOS_TRUE) && (channel->stats.queued != 0U)) ||
            ((receive == RTOS_FALSE) && (channel->stats.free != 0U))) {
            rtos_status_t status = receive == RTOS_TRUE
                ? channel_take_ready(channel, (rtos_channel_reader_t *)view)
                : channel_take_free(channel, (rtos_channel_writer_t *)view);
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, status);
        }

        rtos_tick_t remaining = timeout;
        if (timeout == RTOS_NO_WAIT) {
            channel_increment(receive == RTOS_TRUE ? &channel->stats.receive_misses
                                                   : &channel->stats.acquire_misses);
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, RTOS_ERR_TIMEOUT);
        }
        if (timeout != RTOS_WAIT_FOREVER) {
            rtos_tick_t elapsed = rtos_kernel.tick_count - started;
            if (elapsed >= timeout) {
                channel_increment(receive == RTOS_TRUE ? &channel->stats.receive_timeouts
                                                       : &channel->stats.acquire_timeouts);
                RTOS_PORT_EXIT_CRITICAL();
                return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                    receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                         : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                    rtos_kernel.current_tcb, RTOS_ERR_TIMEOUT);
            }
            remaining = timeout - elapsed;
        }
        if ((caller_mask != 0U) || (caller_global_mask != 0U) ||
            (rtos_port_crit_nest != 1U) ||
            (rtos_internal_block_permitted() != RTOS_OK)) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, RTOS_ERR_SCHED);
        }
        rtos_tcb_t *current = rtos_kernel.current_tcb;
        rtos_wait_node_t **waiters = receive == RTOS_TRUE ? &channel->readers : &channel->writers;
        rtos_status_t status = rtos_internal_block_current_on(channel, waiters, remaining);
        if (status != RTOS_OK) {
            RTOS_PORT_EXIT_CRITICAL();
            return RTOS_INTERNAL_DIAG_RETURN_IF(waited,
                receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE
                                     : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
                rtos_kernel.current_tcb, status);
        }
        RTOS_DIAG_RECORD(RTOS_DIAG_EVENT_WAIT,
            receive == RTOS_TRUE ? RTOS_DIAG_OP_CHANNEL_RECEIVE : RTOS_DIAG_OP_CHANNEL_ACQUIRE,
            current, channel, remaining, timeout, RTOS_OK, 0U);
        channel_increment(receive == RTOS_TRUE ? &channel->stats.receive_waits
                                               : &channel->stats.acquire_waits);
        waited = RTOS_TRUE;
        RTOS_PORT_EXIT_CRITICAL();
        wake_result = current->wait_result;
    }
}

rtos_status_t rtos_channel_acquire(rtos_channel_t *channel, rtos_channel_writer_t *writer,
                                   rtos_tick_t timeout)
{
    if (channel_context_permitted(timeout == RTOS_NO_WAIT ? RTOS_TRUE : RTOS_FALSE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((channel == NULL) || (writer == NULL) || (channel_timeout_valid(timeout) == RTOS_FALSE)) {
        return RTOS_ERR_PARAM;
    }
    return channel_borrow((uintptr_t)channel, writer, timeout, RTOS_FALSE);
}

rtos_status_t rtos_channel_receive(rtos_channel_t *channel, rtos_channel_reader_t *reader,
                                   rtos_tick_t timeout)
{
    if (channel_context_permitted(timeout == RTOS_NO_WAIT ? RTOS_TRUE : RTOS_FALSE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((channel == NULL) || (reader == NULL) || (channel_timeout_valid(timeout) == RTOS_FALSE)) {
        return RTOS_ERR_PARAM;
    }
    return channel_borrow((uintptr_t)channel, reader, timeout, RTOS_TRUE);
}

static rtos_bool_t channel_ticket_valid(const rtos_channel_t *channel,
                                        const rtos_channel_ticket_t *ticket, const void *data,
                                        rtos_channel_slot_state_t state)
{
    if ((ticket->channel_address != (uintptr_t)channel) ||
        (ticket->incarnation != channel->incarnation) ||
        (ticket->slot >= channel->stats.capacity)) {
        return RTOS_FALSE;
    }
    const rtos_channel_slot_t *slot = &channel->slots[ticket->slot];
    return ((slot->generation == ticket->generation) && (slot->state == state) &&
            (data == channel_slot_data(channel, ticket->slot))) ? RTOS_TRUE : RTOS_FALSE;
}

rtos_status_t rtos_channel_publish(rtos_channel_t *channel, rtos_channel_writer_t *writer,
                                   uint32_t length, uint32_t tag)
{
    if (channel_context_permitted(RTOS_TRUE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((channel == NULL) || (writer == NULL)) {
        return RTOS_ERR_PARAM;
    }
    RTOS_PORT_ENTER_CRITICAL();
    rtos_channel_t *registered = channel_find((uintptr_t)channel);
    if (registered == NULL) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_DELETED;
    }
    channel = registered;
    if (channel_external_view(writer, sizeof(*writer), _Alignof(rtos_channel_writer_t))
        == RTOS_FALSE) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    if (channel_ticket_valid(channel, &writer->ticket, writer->data,
                              RTOS_CHANNEL_SLOT_WRITING) == RTOS_FALSE) {
        channel_increment(&channel->stats.stale_rejections);
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_STALE;
    }
    if (channel->stats.closed != 0U) {
        channel_increment(&channel->stats.closed_rejections);
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_CLOSED;
    }
    if (length > channel->block_size) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    if ((channel->ready_tail != RTOS_CHANNEL_INDEX_NONE) &&
        ((channel->ready_tail >= channel->stats.capacity) ||
         (channel->slots[channel->ready_tail].state != RTOS_CHANNEL_SLOT_QUEUED))) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }

    uint32_t index = writer->ticket.slot;
    rtos_channel_slot_t *slot = &channel->slots[index];
    slot->length = length;
    slot->tag = tag;
    slot->next = RTOS_CHANNEL_INDEX_NONE;
    slot->state = RTOS_CHANNEL_SLOT_QUEUED;
    channel_increment(&channel->stats.published);
    slot->sequence = channel->stats.published;
    if (channel->ready_tail == RTOS_CHANNEL_INDEX_NONE) {
        channel->ready_head = index;
    } else {
        channel->slots[channel->ready_tail].next = index;
    }
    channel->ready_tail = index;
    channel->stats.writing--;
    channel->stats.queued++;
    if (channel->stats.queued > channel->stats.queued_peak) {
        channel->stats.queued_peak = channel->stats.queued;
    }
    memset(writer, 0, sizeof(*writer));
    /* Broadcast avoids losing the sole wakeup if a READY recipient is deleted.
     * No loan is granted until that recipient actually executes borrow again. */
    rtos_internal_wake_all(&channel->readers, RTOS_OK);
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

static rtos_status_t channel_return(uintptr_t address, void *view, rtos_bool_t reader)
{
    RTOS_PORT_ENTER_CRITICAL();
    rtos_channel_t *channel = channel_find(address);
    if (channel == NULL) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_DELETED;
    }
    size_t size = reader == RTOS_TRUE
        ? sizeof(rtos_channel_reader_t) : sizeof(rtos_channel_writer_t);
    size_t alignment = reader == RTOS_TRUE
        ? _Alignof(rtos_channel_reader_t) : _Alignof(rtos_channel_writer_t);
    if (channel_external_view(view, size, alignment) == RTOS_FALSE) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    const rtos_channel_ticket_t *ticket = reader == RTOS_TRUE
        ? &((rtos_channel_reader_t *)view)->ticket : &((rtos_channel_writer_t *)view)->ticket;
    const void *data = reader == RTOS_TRUE
        ? ((rtos_channel_reader_t *)view)->data : ((rtos_channel_writer_t *)view)->data;
    rtos_channel_slot_state_t state = reader == RTOS_TRUE
        ? RTOS_CHANNEL_SLOT_READING : RTOS_CHANNEL_SLOT_WRITING;
    if (channel_ticket_valid(channel, ticket, data, state) == RTOS_FALSE) {
        channel_increment(&channel->stats.stale_rejections);
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_STALE;
    }

    uint32_t index = ticket->slot;
    rtos_channel_slot_t *slot = &channel->slots[index];
    slot->state = RTOS_CHANNEL_SLOT_FREE;
    slot->next = channel->free_head;
    channel->free_head = index;
    channel->stats.free++;
    if (reader == RTOS_TRUE) {
        channel->stats.reading--;
        channel_increment(&channel->stats.released);
    } else {
        channel->stats.writing--;
        channel_increment(&channel->stats.cancelled);
    }
    memset(view, 0, size);
    rtos_internal_wake_all(&channel->writers, RTOS_OK);
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_channel_cancel(rtos_channel_t *channel, rtos_channel_writer_t *writer)
{
    if (channel_context_permitted(RTOS_TRUE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((channel == NULL) || (writer == NULL)) {
        return RTOS_ERR_PARAM;
    }
    return channel_return((uintptr_t)channel, writer, RTOS_FALSE);
}

rtos_status_t rtos_channel_release(rtos_channel_t *channel, rtos_channel_reader_t *reader)
{
    if (channel_context_permitted(RTOS_TRUE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((channel == NULL) || (reader == NULL)) {
        return RTOS_ERR_PARAM;
    }
    return channel_return((uintptr_t)channel, reader, RTOS_TRUE);
}

rtos_status_t rtos_channel_close(rtos_channel_t *channel)
{
    if (channel_context_permitted(RTOS_FALSE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if (channel == NULL) {
        return RTOS_ERR_PARAM;
    }
    RTOS_PORT_ENTER_CRITICAL();
    rtos_channel_t *registered = channel_find((uintptr_t)channel);
    if (registered == NULL) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_DELETED;
    }
    channel = registered;
    if (channel->stats.closed != 0U) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_ALREADY;
    }
    channel->stats.closed = 1U;
    rtos_internal_wake_all(&channel->writers, RTOS_ERR_CLOSED);
    rtos_internal_wake_all(&channel->readers, RTOS_ERR_CLOSED);
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

rtos_status_t rtos_channel_deinit(rtos_channel_t *channel)
{
    if (channel_context_permitted(RTOS_FALSE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if (channel == NULL) {
        return RTOS_ERR_PARAM;
    }
    RTOS_PORT_ENTER_CRITICAL();
    rtos_channel_t *registered = channel_find((uintptr_t)channel);
    if (registered == NULL) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_DELETED;
    }
    channel = registered;
    if ((channel->stats.closed == 0U) || (channel->stats.free != channel->stats.capacity) ||
        (channel->stats.writing != 0U) || (channel->stats.reading != 0U) ||
        (channel->stats.queued != 0U) || (channel->writers != NULL) ||
        (channel->readers != NULL)) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_BUSY;
    }
    uint32_t retained = 0U;
    for (uint32_t index = 0U; index < channel_region_count; index++) {
        if (channel_regions[index].owner != channel) {
            channel_regions[retained] = channel_regions[index];
            retained++;
        }
    }
    memset(&channel_regions[retained], 0,
        (size_t)(channel_region_count - retained) * sizeof(channel_regions[0]));
    channel_region_count = retained;
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

static uint32_t channel_waiter_count(const rtos_wait_node_t *node)
{
    uint32_t count = 0U;
    while (node != NULL) {
        if (count == UINT32_MAX) {
            break;
        }
        count++;
        node = node->next;
    }
    return count;
}

rtos_status_t rtos_channel_get_stats(const rtos_channel_t *channel, rtos_channel_stats_t *stats)
{
    if (channel_context_permitted(RTOS_FALSE) != RTOS_OK) {
        return RTOS_ERR_ISR;
    }
    if ((channel == NULL) || (stats == NULL)) {
        return RTOS_ERR_PARAM;
    }
    RTOS_PORT_ENTER_CRITICAL();
    rtos_channel_t *registered = channel_find((uintptr_t)channel);
    if (registered == NULL) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_DELETED;
    }
    channel = registered;
    if (channel_external_view(stats, sizeof(*stats), _Alignof(rtos_channel_stats_t))
        == RTOS_FALSE) {
        RTOS_PORT_EXIT_CRITICAL();
        return RTOS_ERR_PARAM;
    }
    *stats = channel->stats;
    stats->waiting_writers = channel_waiter_count(channel->writers);
    stats->waiting_readers = channel_waiter_count(channel->readers);
    RTOS_PORT_EXIT_CRITICAL();
    return RTOS_OK;
}

#endif /* RTOS_CONFIG_USE_CHANNEL */
