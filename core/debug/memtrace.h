#ifndef MEMTRACE_H
#define MEMTRACE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "../defines.h"

#ifdef __cplusplus
extern "C" {
#endif


/* event types */
enum {
    MT_EVT_BYTE  = 1,  /* single byte write */
    MT_EVT_RUN   = 2,  /* contiguous run of bytes */
    MT_EVT_ERASE = 3,  /* erase range (flash) */
};

/* Public control API */
void memtrace_init(size_t capacity_bytes);   /* capacity rounded up to power of two, ctor */
void memtrace_free(void);
void memtrace_enable(bool on);
bool memtrace_is_enabled(void);
void memtrace_clear(void);

/* page subscription, 4KB pages across 16MB space (4096 pages) */
void memtrace_subscribe_range(uint32_t low, uint32_t high, bool enable);
void memtrace_subscribe_all(bool enable);

uint64_t memtrace_dropped(void);

/* drain into caller buffer. returns bytes copied */
size_t memtrace_drain(void *dst, size_t max_bytes);

/* hot path emit helpers (producer = emu thread) */
/* cycles is a 32-bit snapshot (cpu.cycles) */
void memtrace_emit_byte(uint32_t addr, uint8_t value, uint32_t cycles);
void memtrace_emit_run(uint32_t addr, const uint8_t *src, uint32_t len, uint32_t cycles);
void memtrace_emit_erase(uint32_t addr, uint32_t len, uint32_t cycles);

ALWAYS_INLINE bool memtrace_is_page_tracked(const uint32_t addr) {
    extern uint64_t memtrace_track_pages[4096/64];
    const uint32_t p = (addr >> 12) & 0xFFFu; /* 0..4095 */
    return (memtrace_track_pages[p >> 6] & (1ull << (p & 63))) != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* MEMTRACE_H */

