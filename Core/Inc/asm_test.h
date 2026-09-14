#ifndef __ASM_TEST_H__
#define __ASM_TEST_H__


#include <stdint.h>


#ifndef NAKED
#define NAKED __attribute__((naked))
#endif



uint32_t asm_increment(uint32_t value);

void asm_printf(const char *fmt);

uint32_t asm_pass_args(uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4);

#endif
