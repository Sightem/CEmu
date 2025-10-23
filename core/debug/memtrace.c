#include "memtrace.h"
#include "../atomics.h"
#include "../defines.h"

#include <stdlib.h>
#include <string.h>

static uint8_t *mt_buf;
static uint32_t mt_mask;
#ifndef MEMTRACE_CACHELINE
#define MEMTRACE_CACHELINE 64
#endif

static _Alignas(MEMTRACE_CACHELINE) _Atomic(uint32_t) mt_head;  /* producer cursor */
static _Alignas(MEMTRACE_CACHELINE) _Atomic(uint32_t) mt_tail;  /* consumer cursor */
static uint64_t mt_dropped;
static int mt_enabled;

uint64_t memtrace_track_pages[4096/64];

ALWAYS_INLINE uint32_t mt_capacity(void) { return mt_mask + 1u; }

ALWAYS_INLINE uint32_t mt_used_relaxed(const uint32_t h, const uint32_t t) { return h - t; }

static uint32_t roundup_pow2_u32(uint32_t v) {
    if (v < 2)
        return 2;
    v--; v |= v >> 1; v |= v >> 2; v |= v >> 4; v |= v >> 8; v |= v >> 16; v++;
    return v;
}

void memtrace_init(const size_t capacity_bytes) {
    if (mt_buf)
        return;

    const uint32_t cap = roundup_pow2_u32((uint32_t)(capacity_bytes ? capacity_bytes : (1u << 20))); /* default 1 MiB */

    mt_buf = (uint8_t*)malloc(cap);
    if (!mt_buf) {
        mt_mask = 0;
        return;
    }
    mt_mask = cap - 1u;
    mt_head = mt_tail = 0;
    mt_dropped = 0;

    memset(memtrace_track_pages, 0, sizeof(memtrace_track_pages));

    mt_enabled = 0;
}

void memtrace_free(void) {
    free(mt_buf);
    mt_buf = NULL;
    mt_mask = 0;
    mt_head = 0;
    mt_tail = 0;
    mt_dropped = 0;
    mt_enabled = 0;
}

void memtrace_enable(bool on) { mt_enabled = on ? 1 : 0; }
bool memtrace_is_enabled(void) { return mt_enabled != 0; }

void memtrace_clear(void) {
    /* acquire head, then publish tail */
    const uint32_t h = atomic_load_explicit(&mt_head, memory_order_acquire);
    mt_tail = h; /* seq_cst store is fine, release would be sufficient */
    mt_dropped = 0;
}

uint64_t memtrace_dropped(void) { return mt_dropped; }

void memtrace_subscribe_all(bool enable) {
    memset(memtrace_track_pages, enable ? 0xFF : 0x00, sizeof(memtrace_track_pages));
}

void memtrace_subscribe_range(uint32_t low, uint32_t high, bool enable) {
    const uint32_t p0 = (low  >> 12) & 0xFFFu;
    const uint32_t p1 = (high >> 12) & 0xFFFu;
    const uint32_t w0 = p0 >> 6;
    const uint32_t w1 = p1 >> 6;
    const uint32_t b0 = p0 & 63u;
    const uint32_t b1 = p1 & 63u;
    const uint64_t fill = enable ? ~0ull : 0ull;

    if (w0 == w1) {
        /* range fits within a single 64 bit word */
        const uint64_t start_mask = (~0ull) << b0;
        const uint64_t end_mask = (b1 == 63u) ? ~0ull : ((1ull << (b1 + 1u)) - 1ull);
        const uint64_t mask = start_mask & end_mask;
        if (enable) memtrace_track_pages[w0] |= mask;
        else        memtrace_track_pages[w0] &= ~mask;
        return;
    }

    /* head partial word [b0..63] */
    {
        const uint64_t mask0 = (~0ull) << b0;
        if (enable)
            memtrace_track_pages[w0] |= mask0;
        else
            memtrace_track_pages[w0] &= ~mask0;
    }

    /* middle full words */
    for (uint32_t w = w0 + 1; w < w1; ++w) {
        memtrace_track_pages[w] = fill;
    }

    /* tail partial word [0..b1] */
    {
        const uint64_t mask1 = (b1 == 63u) ? ~0ull : ((1ull << (b1 + 1u)) - 1ull);
        if (enable)
            memtrace_track_pages[w1] |= mask1;
        else
            memtrace_track_pages[w1] &= ~mask1;
    }
}

ALWAYS_INLINE void mt_copy_in(uint32_t pos, const void *src, uint32_t len) {
    const uint32_t cap = mt_capacity();
    const uint32_t i = pos & mt_mask;
    const uint32_t first = (i + len <= cap) ? len : (cap - i);

    memcpy(mt_buf + i, src, first);

    if (first < len)
        memcpy(mt_buf, (const uint8_t*)src + first, len - first);
}

static int mt_try_write(const void *src, const uint32_t len) {
    if (unlikely(!mt_buf))
        return 0;

    const uint32_t t = atomic_load_explicit(&mt_tail, memory_order_acquire);
    const uint32_t h = atomic_load_explicit(&mt_head, memory_order_relaxed);
    const uint32_t cap = mt_capacity();
    const uint32_t free_bytes = cap - mt_used_relaxed(h, t);
    if (free_bytes <= len)
    {
        mt_dropped++;
        return 0;
    }

    mt_copy_in(h, src, len);

    mt_head = h + len;
    return 1;
}

size_t memtrace_drain(void *dst, size_t max_bytes) {
    if (unlikely(!mt_buf))
        return 0;

    /* consumer: acquire head to see what has been published */
    const uint32_t h = atomic_load_explicit(&mt_head, memory_order_acquire);
    const uint32_t t = atomic_load_explicit(&mt_tail, memory_order_relaxed);
    const uint32_t cap = mt_capacity();
    const uint32_t avail = h - t;

    if (!avail)
        return 0;

    const uint32_t want = (uint32_t)(max_bytes < avail ? max_bytes : avail);
    const uint32_t i = t & mt_mask;
    const uint32_t first = (i + want <= cap) ? want : (cap - i);

    memcpy(dst, mt_buf + i, first);

    if (first < want)
        memcpy((uint8_t*)dst + first, mt_buf, want - first);

    /* publish consumption */
    mt_tail = t + want;
    return want;
}

typedef struct {
    uint8_t  type;     /* MT_EVT_* */
    uint8_t  _pad;
    uint16_t len;      /* for RUN, payload length; for BYTE=1, ERASE=size in bytes low 16 bits */
    uint32_t addr;     /* absolute 24-bit in low bits */
    uint32_t cycles;   /* cpu.cycles snapshot */
} mt_hdr_t;

void memtrace_emit_byte(uint32_t addr, uint8_t value, uint32_t cycles) {
    if (unlikely(!mt_enabled))
        return;

    mt_hdr_t h;
    h.type = MT_EVT_BYTE;
    h._pad = 0; h.len = 1;
    h.addr = addr;
    h.cycles = cycles;

    uint8_t buf[sizeof(h) + 1];
    memcpy(buf, &h, sizeof(h));

    buf[sizeof(h)] = value;
    (void)mt_try_write(buf, (uint32_t)(sizeof(h) + 1));
}

void memtrace_emit_run(uint32_t addr, const uint8_t *src, uint32_t len, uint32_t cycles) {
    if (unlikely(!mt_enabled) || !len) return;

    const uint32_t CHUNK = 1024;
    uint32_t off = 0;

    uint32_t h_local = atomic_load_explicit(&mt_head, memory_order_relaxed);
    while (off < len) {
        uint32_t n = len - off;

        if (n > CHUNK)
            n = CHUNK;

        mt_hdr_t h;
        h.type = MT_EVT_RUN;
        h._pad = 0;
        h.len = (uint16_t)n;
        h.addr = addr + off;
        h.cycles = cycles;

        const uint32_t t = atomic_load_explicit(&mt_tail, memory_order_acquire);
        const uint32_t cap = mt_capacity();
        const uint32_t need = (uint32_t)sizeof(h) + n;
        const uint32_t free_bytes = cap - mt_used_relaxed(h_local, t);

        if (free_bytes <= need) {
            mt_dropped++;
            return;
        }

        /* write header+payload at local head, then advance local head */
        mt_copy_in(h_local, &h, (uint32_t)sizeof(h));
        h_local += (uint32_t)sizeof(h);

        mt_copy_in(h_local, src + off, n);
        h_local += n;

        off += n;
    }


    mt_head = h_local;
}

void memtrace_emit_erase(const uint32_t addr, const uint32_t len, const uint32_t cycles) {
    if (unlikely(!mt_enabled))
        return;

    mt_hdr_t h;
    h.type = MT_EVT_ERASE;
    h._pad = 0;
    h.len = (uint16_t)(len & 0xFFFFu);
    h.addr = addr;
    h.cycles = cycles;

    (void)mt_try_write(&h, (uint32_t)sizeof(h));
}

