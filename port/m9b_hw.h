// Ultima VII on the m9b SoC: the CPU-specific bits the port shares
// (interrupt masking, data cache cleaning, start-up). Elsewhere (the host
// test build) they do nothing. (The same as ../../sshock/port/m9b_hw.h.)
#ifndef M9B_HW_H
#define M9B_HW_H

#include <stdint.h>
#include "m9b_map.h"

#ifdef __cplusplus
extern "C" void trap_entry(void);   // ../../core/runtime/trap_entry.S
#else
extern void trap_entry(void);
#endif

#define CLOCK_CNT    (*(volatile uint32_t*)(M9B_TIMING + 0xCu))   // 36 MHz
#define CLOCK_PER_MS 36000u
#define PSRAM_BASE   M9B_PSRAM_BASE

// Masks interrupts; returns what irq_restore() needs.
static inline uint32_t irq_off(void) {
#ifdef __riscv
    uint32_t m;
    asm volatile("csrrci %0, mstatus, 8" : "=r"(m));
    return m & 8u;
#else
    return 0;
#endif
}

static inline void irq_restore(uint32_t m) {
#ifdef __riscv
    if (m) asm volatile("csrsi mstatus, 8");
#else
    (void)m;
#endif
}

// Writes p[0..len) back to PSRAM, for the video and audio engines' DMA.
static inline void cache_clean(const void *p, uint32_t len) {
#ifdef __riscv
    for (uintptr_t a = (uintptr_t)p & ~63u; a < (uintptr_t)p + len; a += 64u)
        asm volatile("cbo.clean (%0)" :: "r"(a) : "memory");
#else
    (void)p; (void)len;
#endif
}

// The trap handler (../../core/runtime/trap_entry.S: misaligned accesses,
// interrupts), the given interrupt sources, interrupts on.
static inline void hw_start(uint32_t irqs) {
#define IRQ_PENDING_REG (*(volatile uint32_t*)(M9B_CTRL + 0x4u))
#define IRQ_ENABLE_REG  (*(volatile uint32_t*)(M9B_CTRL + 0x8u))
#ifdef __riscv
    asm volatile("csrw mtvec, %0" :: "r"(&trap_entry));
#endif
    IRQ_PENDING_REG = 0xFu;
    IRQ_ENABLE_REG |= irqs;
#ifdef __riscv
    asm volatile("csrs mie, %0" :: "r"(1u << 11));
    asm volatile("csrs mstatus, %0" :: "r"(1u << 3));
#endif
}

#endif
