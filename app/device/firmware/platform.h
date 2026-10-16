#ifndef OPEN_LOGIC16_PLATFORM_H
#define OPEN_LOGIC16_PLATFORM_H
#include <stdint.h>
#include "../logic16.h"

#ifdef L16_HOST_TEST
uint32_t l16_read32(uint32_t address);
void l16_write32(uint32_t address, uint32_t value);
uint64_t l16_ticks(void);
static inline void l16_barrier(void) { }
#else
static inline uint32_t l16_read32(uint32_t address) { return *(volatile uint32_t *)(uintptr_t)address; }
static inline void l16_write32(uint32_t address, uint32_t value) { *(volatile uint32_t *)(uintptr_t)address = value; }
static inline void l16_barrier(void) { __asm__ volatile("dmb sy" ::: "memory"); }
static inline uint64_t l16_ticks(void)
{
    uint32_t lo, hi;
    __asm__ volatile("isb\nmrrc p15, 0, %0, %1, c14" : "=r"(lo), "=r"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}
#endif

int l16_board_prepare(void);
int l16_board_read(uint16_t *word, volatile uint32_t *stop);
int l16_board_check(void);
int l16_board_restore(void);
uint16_t l16_board_probe_inputs(void);
void l16_memory_enable(void);
void l16_memory_disable(void);
extern uint32_t amp_psci_cpu_off(void);
#endif
