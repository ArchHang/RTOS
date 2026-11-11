/** @file heap.c
 * @brief Bounded newlib heap for the isolated HIL firmware.
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

enum { H743_LIBC_HEAP_SIZE = 2048U };

static _Alignas(8) uint8_t libc_heap[H743_LIBC_HEAP_SIZE];
static size_t libc_heap_used;

void *_sbrk(ptrdiff_t increment);

void *_sbrk(ptrdiff_t increment)
{
    const size_t previous = libc_heap_used;
    if (increment < 0) {
        const size_t released = (size_t)(-(increment + 1)) + 1U;
        if (released > previous) {
            errno = ENOMEM;
            return (void *)-1;
        }
        libc_heap_used = previous - released;
    } else {
        const size_t requested = (size_t)increment;
        if (requested > sizeof(libc_heap) - previous) {
            errno = ENOMEM;
            return (void *)-1;
        }
        libc_heap_used = previous + requested;
    }
    return &libc_heap[previous];
}
