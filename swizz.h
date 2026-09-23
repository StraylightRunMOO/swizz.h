/* swizz.h — Swizz.h bgen-style generator
 * C99 header-only, zero dependencies, -pedantic -Werror clean.
 * Drop this in your project and #include it after defining the macros above.
 * Multiple independent tables by re-#defining and re-including.
 *
 * Thread safety
 * -------------
 * Swizz tables are NOT internally synchronized. Concurrent readers are safe
 * only if no writer is active; any add/delete/reserve/clear/rebuild call must
 * have exclusive access to the table. If you need concurrent access, wrap
 * calls in a reader/writer lock at the caller. The `generation` counter is
 * bumped on every rehash so callers can detect invalidation of cached entry
 * pointers.
 */

#ifndef SWIZZ_H_INTERNAL_GUARD
#define SWIZZ_H_INTERNAL_GUARD

#define SWIZZ_VERSION_MAJOR 1
#define SWIZZ_VERSION_MINOR 1
#define SWIZZ_VERSION_PATCH 1
#define SWIZZ_VERSION_STRING "1.1.1"

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------
 * SIMD group probing configuration (AVX2 / SSE2 / NEON / scalar)
 * Must come early - used by Bloom filter configuration
 * ------------------------------------------------------------------ */
#if defined(__AVX2__)
#  include <immintrin.h>
#  define SWIZZ_GROUP_WIDTH 32
#  define SWIZZ_USE_SIMD 1
   typedef __m256i swizz_group_t;
#  define swizz_group_load(p)     _mm256_loadu_si256((const __m256i*)(p))
#  define swizz_group_bcast(fp)   _mm256_set1_epi8((char)(fp))
#  define swizz_group_cmpeq(g,f)  _mm256_cmpeq_epi8((g),(f))
#  define swizz_group_mask(m)     _mm256_movemask_epi8(m)
#  define swizz_group_or(a,b)     _mm256_or_si256((a),(b))
#elif defined(__SSE2__)
#  include <emmintrin.h>
#  define SWIZZ_GROUP_WIDTH 16
#  define SWIZZ_USE_SIMD 1
   typedef __m128i swizz_group_t;
#  define swizz_group_load(p)     _mm_loadu_si128((const __m128i*)(p))
#  define swizz_group_bcast(fp)   _mm_set1_epi8((char)(fp))
#  define swizz_group_cmpeq(g,f)  _mm_cmpeq_epi8((g),(f))
#  define swizz_group_mask(m)     _mm_movemask_epi8(m)
#  define swizz_group_or(a,b)     _mm_or_si128((a),(b))
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#  include <arm_neon.h>
#  define SWIZZ_GROUP_WIDTH 16
#  define SWIZZ_USE_SIMD 1
   typedef uint8x16_t swizz_group_t;
#  define swizz_group_load(p)     vld1q_u8((const uint8_t*)(p))
#  define swizz_group_bcast(fp)   vdupq_n_u8(fp)
#  define swizz_group_cmpeq(g,f)  vceqq_u8((g),(f))
#  define swizz_group_or(a,b)     vorrq_u8((a),(b))
   /* NEON movemask: pack 16 comparison-result bytes (0xFF / 0x00) into a
    * 16-bit mask suitable for __builtin_ctz. AND with a per-byte bit-position
    * pattern, then reduce with pairwise adds — three steps, no memory round-trip. */
   static inline int swizz_neon_movemask(uint8x16_t v) {
       static const uint8_t bit_pos_data[16] = {
           1,   2,   4,   8,  16,  32,  64, 128,
           1,   2,   4,   8,  16,  32,  64, 128
       };
       const uint8x16_t bit_pos = vld1q_u8(bit_pos_data);
       uint8x16_t masked = vandq_u8(v, bit_pos);
       /* Pairwise-add 16 u8 -> 8 u16 -> 4 u32 -> 2 u64, then combine two bytes. */
       uint16x8_t a16 = vpaddlq_u8(masked);
       uint32x4_t a32 = vpaddlq_u16(a16);
       uint64x2_t a64 = vpaddlq_u32(a32);
       uint8_t lo = (uint8_t)vgetq_lane_u64(a64, 0);
       uint8_t hi = (uint8_t)vgetq_lane_u64(a64, 1);
       return (int)lo | ((int)hi << 8);
   }
#  define swizz_group_mask(m)     swizz_neon_movemask(m)
#else
#  define SWIZZ_GROUP_WIDTH 1
#  define SWIZZ_USE_SIMD 0
   typedef uint8_t swizz_group_t;
#endif

/* Load factor: 7/8 for SIMD (better cache utilization), 1/2 for scalar */
#if SWIZZ_USE_SIMD
#  define SWIZZ_LOAD_FACTOR(cap)  (((cap) * 7) >> 3)
#else
#  define SWIZZ_LOAD_FACTOR(cap)  ((cap) >> 1)
#endif

/* ------------------------------------------------------------------
 * Hash helpers (available for use in user macros)
 *
 * Two families are provided:
 *   hash_string(_case_insensitive)  — FNV-1a; small, deterministic, fine for
 *                                     short strings and modest workloads.
 *   hash_bytes / hash_string_strong — wyhash-lite (a distilled variant of
 *                                     wyhash 4.1); higher quality, especially
 *                                     for long strings, shared prefixes, and
 *                                     integer-like data. Passes SMHasher.
 *
 * All hashes are 64-bit; the top byte is used both as bloom-block index and
 * as the group-probe fingerprint.
 * ------------------------------------------------------------------ */
static inline uint64_t hash_string(const char *str)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static inline uint64_t hash_string_case_insensitive(const char *str)
{
    uint64_t hash = 0xcbf29ce484222325ULL;
    while (*str) {
        unsigned char c = *str++;
        if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

/* --- wyhash-lite (public domain, distilled from wyhash 4.1 by Wang Yi) --- */
#define SWIZZ_WY_P0 0xa0761d6478bd642full
#define SWIZZ_WY_P1 0xe7037ed1a0b428dbull
#define SWIZZ_WY_P2 0x8ebc6af09c88c6e3ull
#define SWIZZ_WY_P3 0x589965cc75374cc3ull

static inline uint64_t swizz_wymum(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    __uint128_t r = (__uint128_t)a * (__uint128_t)b;
    return (uint64_t)r ^ (uint64_t)(r >> 64);
#else
    /* 64x64 -> 128 emulation, then fold */
    uint64_t ha = a >> 32, hb = b >> 32, la = (uint32_t)a, lb = (uint32_t)b;
    uint64_t rh = ha * hb;
    uint64_t rm0 = ha * lb;
    uint64_t rm1 = hb * la;
    uint64_t rl = la * lb;
    uint64_t t  = rl + (rm0 << 32);
    uint64_t c  = t < rl;
    uint64_t lo = t + (rm1 << 32);
    c += lo < t;
    uint64_t hi = rh + (rm0 >> 32) + (rm1 >> 32) + c;
    return lo ^ hi;
#endif
}

static inline uint64_t swizz_wy_read8(const uint8_t *p)
{
    uint64_t v;
    memcpy(&v, p, 8);
    return v;
}

static inline uint64_t swizz_wy_read4(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

static inline uint64_t swizz_wy_read_tail(const uint8_t *p, size_t k)
{
    /* 1..3 byte tail */
    return ((uint64_t)p[0] << 16) | ((uint64_t)p[k >> 1] << 8) | p[k - 1];
}

static inline uint64_t hash_bytes(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint64_t seed = SWIZZ_WY_P0 ^ (uint64_t)len;
    uint64_t a, b;
    if (len <= 16) {
        if (len >= 4) {
            a = (swizz_wy_read4(p) << 32) | swizz_wy_read4(p + ((len >> 3) << 2));
            b = (swizz_wy_read4(p + len - 4) << 32) |
                swizz_wy_read4(p + len - 4 - ((len >> 3) << 2));
        } else if (len > 0) {
            a = swizz_wy_read_tail(p, len);
            b = 0;
        } else {
            a = 0; b = 0;
        }
    } else {
        size_t i = len;
        if (i > 48) {
            uint64_t s1 = seed, s2 = seed;
            do {
                seed = swizz_wymum(swizz_wy_read8(p)      ^ SWIZZ_WY_P1, swizz_wy_read8(p +  8) ^ seed);
                s1   = swizz_wymum(swizz_wy_read8(p + 16) ^ SWIZZ_WY_P2, swizz_wy_read8(p + 24) ^ s1);
                s2   = swizz_wymum(swizz_wy_read8(p + 32) ^ SWIZZ_WY_P3, swizz_wy_read8(p + 40) ^ s2);
                p += 48; i -= 48;
            } while (i > 48);
            seed ^= s1 ^ s2;
        }
        while (i > 16) {
            seed = swizz_wymum(swizz_wy_read8(p) ^ SWIZZ_WY_P1, swizz_wy_read8(p + 8) ^ seed);
            i -= 16; p += 16;
        }
        a = swizz_wy_read8(p + i - 16);
        b = swizz_wy_read8(p + i - 8);
    }
    return swizz_wymum(SWIZZ_WY_P1 ^ (uint64_t)len, swizz_wymum(a ^ SWIZZ_WY_P1, b ^ seed));
}

static inline uint64_t hash_string_strong(const char *str)
{
    return hash_bytes(str, strlen(str));
}

/* Fast integer mixer (splitmix64 finalizer). Ideal for pointer keys or
 * already-uniform 64-bit values. */
static inline uint64_t hash_u64(uint64_t x)
{
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

/* Fingerprint: high 7 bits of the hash. The two sentinel values (0x00 empty,
 * 0x80 tombstone) each remap to a distinct nearby non-sentinel value so that
 * no single fingerprint receives 2x mass. */
static inline uint8_t entry_fingerprint(uint64_t h)
{
    uint8_t fp = (uint8_t)(h >> 56);
    /* 0x00 -> 0x01 ; 0x80 -> 0x81. Each maps to a distinct target, preserving
     * the top bit so we don't skew the population balance. */
    fp |= (uint8_t)((fp == 0x00) | (fp == 0x80));
    return fp;
}

/* ------------------------------------------------------------------
 * Blocked Bloom Filter — SIMD-aware edition
 *
 * Layout: SWIZZ_BLOOM_NUM_BLOCKS × 256-bit blocks (block-size chosen so a
 * single lookup touches exactly one cache line — the *whole* block fits in
 * one 64-byte line on x86, two on 32-byte-line archs).
 *
 * The primary purpose of this filter in Swizz is NOT to short-circuit misses;
 * it's to gate the cache-line prefetch of the entry/ctrl arrays. On a bloom
 * positive we know it's overwhelmingly likely we're about to touch the ctrl
 * line at hash(key) mod cap, so we issue a prefetch there before the SIMD
 * probe runs. That hides most of the load latency for actual hits — the
 * dominant workload in symbol-table and cache use cases.
 *
 * Bit generation uses double-hashing (h1 + i*h2), producing K independent
 * hash values from one 64-bit input via two decorrelated mixers. This is
 * both faster than a multiply chain and gives strictly better distribution.
 * ------------------------------------------------------------------ */
#ifndef SWIZZ_BLOOM_NUM_BLOCKS
#  if SWIZZ_USE_SIMD
#    define SWIZZ_BLOOM_NUM_BLOCKS  8U   /* power-of-2 → fast & */
#    define SWIZZ_BLOOM_BLOCK_WORDS 4U   /* 256 bits per block */
#    define SWIZZ_BLOOM_K           8    /* 8 bits per key */
#  else
#    define SWIZZ_BLOOM_NUM_BLOCKS  1U
#    define SWIZZ_BLOOM_BLOCK_WORDS 4U
#    define SWIZZ_BLOOM_K           4
#  endif
#endif

#define SWIZZ_BLOOM_TOTAL_WORDS (SWIZZ_BLOOM_NUM_BLOCKS * SWIZZ_BLOOM_BLOCK_WORDS)

/* Tables smaller than this bypass the bloom check entirely — for small caps
 * the whole ctrl array fits in one or two cache lines, so a SIMD probe is
 * cheaper than a bloom lookup. */
#ifndef SWIZZ_BLOOM_MIN_CAP
#  define SWIZZ_BLOOM_MIN_CAP 64U
#endif

static inline uint32_t bloom_block_idx(uint64_t h)
{
    /* High bits → block selection (decorrelates from group probe H1) */
    return (uint32_t)(h >> 56) & (SWIZZ_BLOOM_NUM_BLOCKS - 1);
}

/* Portable, no-op-if-unsupported prefetch. */
#if defined(__GNUC__) || defined(__clang__)
#  define swizz_prefetch(p) __builtin_prefetch((p), 0, 1)
#else
#  define swizz_prefetch(p) ((void)0)
#endif

/* Double-hashing: derive two independent 32-bit sub-hashes, then combine
 * as h1 + i*h2. Both mixers avalanche each input bit through the full 64
 * bits before we take the top 32. */
static inline void swizz_bloom_h1h2(uint64_t h, uint32_t *h1, uint32_t *h2)
{
    uint64_t a = h * 0x9e3779b97f4a7c15ULL;
    uint64_t b = h * 0xbf58476d1ce4e5b9ULL;
    *h1 = (uint32_t)(a >> 32);
    *h2 = (uint32_t)(b >> 32) | 1u;  /* ensure h2 is odd → hits every residue */
}

static inline void bloom_add(uint64_t *bloom, uint64_t h)
{
#ifdef SWIZZ_DISABLE_BLOOM
    (void)bloom;
    (void)h;
#else
    uint32_t block = bloom_block_idx(h);
    uint64_t *block_ptr = &bloom[block * SWIZZ_BLOOM_BLOCK_WORDS];
    uint32_t h1, h2;
    swizz_bloom_h1h2(h, &h1, &h2);
    for (int i = 0; i < SWIZZ_BLOOM_K; i++) {
        uint32_t bit  = (h1 + (uint32_t)i * h2) & 0xFF;  /* 0..255 inside block */
        uint32_t word = bit >> 6;
        block_ptr[word] |= 1ULL << (bit & 63);
    }
#endif
}

static inline bool bloom_may_contain(const uint64_t *bloom, uint64_t h)
{
#ifdef SWIZZ_DISABLE_BLOOM
    (void)bloom;
    (void)h;
    return true;
#else
    uint32_t block = bloom_block_idx(h);
    const uint64_t *block_ptr = &bloom[block * SWIZZ_BLOOM_BLOCK_WORDS];
    uint32_t h1, h2;
    swizz_bloom_h1h2(h, &h1, &h2);
    /* The whole 256-bit block is one cache line on x86 (or two on 32B-line
     * arches). One touch pulls all 4 words into L1; the K probes then hit
     * cache. No SIMD gather trick beats a straight load-and-test here. */
    for (int i = 0; i < SWIZZ_BLOOM_K; i++) {
        uint32_t bit  = (h1 + (uint32_t)i * h2) & 0xFF;
        uint32_t word = bit >> 6;
        uint64_t mask = 1ULL << (bit & 63);
        if (!(block_ptr[word] & mask))
            return false;
    }
    return true;
#endif
}

/* ------------------------------------------------------------------
 * SIMD-accelerated group probing helpers
 * ------------------------------------------------------------------ */

/* Probe for matching fingerprint - returns bitmask of matches */
static inline int swizz_probe_group(const uint8_t *ctrl, uint8_t fp)
{
#if SWIZZ_USE_SIMD
    swizz_group_t group = swizz_group_load((const swizz_group_t*)ctrl);
    swizz_group_t fingerprint = swizz_group_bcast(fp);
    swizz_group_t cmp = swizz_group_cmpeq(group, fingerprint);
    return swizz_group_mask(cmp);
#else
    return (*ctrl == fp) ? 1 : 0;
#endif
}

/* Probe for empty slots - returns bitmask of empty (0x00) slots */
static inline int swizz_probe_empty(const uint8_t *ctrl)
{
#if SWIZZ_USE_SIMD
    swizz_group_t group = swizz_group_load((const swizz_group_t*)ctrl);
    swizz_group_t empty = swizz_group_bcast(0x00);
    swizz_group_t cmp = swizz_group_cmpeq(group, empty);
    return swizz_group_mask(cmp);
#else
    return (*ctrl == 0x00) ? 1 : 0;
#endif
}

/* Probe for empty or deleted slots - returns bitmask of available slots */
static inline int swizz_probe_available(const uint8_t *ctrl)
{
#if SWIZZ_USE_SIMD
    swizz_group_t group = swizz_group_load((const swizz_group_t*)ctrl);
    swizz_group_t empty = swizz_group_bcast(0x00);
    swizz_group_t deleted = swizz_group_bcast(0x80);
    swizz_group_t cmp_empty = swizz_group_cmpeq(group, empty);
    swizz_group_t cmp_deleted = swizz_group_cmpeq(group, deleted);
    swizz_group_t cmp = swizz_group_or(cmp_empty, cmp_deleted);
    return swizz_group_mask(cmp);
#else
    return (*ctrl == 0x00 || *ctrl == 0x80) ? 1 : 0;
#endif
}

#endif /* SWIZZ_H_INTERNAL_GUARD */

/* ------------------------------------------------------------------
 * Required user macros (must be defined before #include)
 * ------------------------------------------------------------------ */
#if !defined(SWIZZ_NAME) || !defined(SWIZZ_KEY_TYPE) || !defined(SWIZZ_VALUE_TYPE) || \
    !defined(SWIZZ_HASH) || !defined(SWIZZ_EQ) || !defined(SWIZZ_DUP_KEY) || !defined(SWIZZ_FREE_KEY)
#error "You must #define SWIZZ_NAME, SWIZZ_KEY_TYPE, SWIZZ_VALUE_TYPE, SWIZZ_HASH(k), SWIZZ_EQ(k1,k2), SWIZZ_DUP_KEY(k), SWIZZ_FREE_KEY(k) before including swizz.h"
#endif

/* Optional allocator overrides (defaults to stdlib) - use unique names */
#ifndef SWIZZ_ALLOC_MALLOC
#define SWIZZ_ALLOC_MALLOC(sz) malloc(sz)
#else
#define SWIZZ_ALLOC_MALLOC_USER 1
#endif
#ifndef SWIZZ_ALLOC_CALLOC
#define SWIZZ_ALLOC_CALLOC(nmemb, size) calloc(nmemb, size)
#endif
#ifndef SWIZZ_ALLOC_FREE
#define SWIZZ_ALLOC_FREE(p) free(p)
#endif

/* ------------------------------------------------------------------
 * Aligned allocation for SIMD control bytes
 *
 * Preference order:
 *   1. C11 aligned_alloc  — cleanest; requires the size be a multiple of the
 *      alignment, which we always satisfy (both are powers of two ≥ 16).
 *   2. POSIX posix_memalign — available on ~every Unix.
 *   3. Hand-rolled overallocate-and-align — fallback for MSVC without
 *      _aligned_malloc plumbed through the allocator hook, and for any
 *      exotic libc that lacks the above.
 *
 * When a user-supplied SWIZZ_ALLOC_MALLOC hook is present we always use the
 * hand-rolled path, since aligned_alloc/posix_memalign cannot be routed
 * through arbitrary allocator overrides.
 * ------------------------------------------------------------------ */
#if SWIZZ_USE_SIMD

#if !defined(SWIZZ_ALLOC_MALLOC_OVERRIDDEN) && !defined(SWIZZ_ALLOC_ALIGNED_HANDROLLED)
#  if defined(SWIZZ_ALLOC_MALLOC_USER)
     /* User overrode the allocator — must fall back to hand-rolled. */
#    define SWIZZ_ALLOC_ALIGNED_HANDROLLED 1
#  elif defined(_ISOC11_SOURCE) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L && !defined(__APPLE__))
#    define SWIZZ_ALLOC_ALIGNED_C11 1
#  elif defined(_POSIX_C_SOURCE) && _POSIX_C_SOURCE >= 200112L
#    define SWIZZ_ALLOC_ALIGNED_POSIX 1
#  else
#    define SWIZZ_ALLOC_ALIGNED_HANDROLLED 1
#  endif
#endif

#endif /* SWIZZ_USE_SIMD */

/* ------------------------------------------------------------------
 * Token pasting helpers (two-stage to force macro expansion)
 * ------------------------------------------------------------------ */
#define SWIZZ_CONCAT_(x, y) x##y
#define SWIZZ_CONCAT(x, y) SWIZZ_CONCAT_(x, y)
#define SWIZZ_TABLE_T          SWIZZ_CONCAT(SWIZZ_NAME, _table)
#define SWIZZ_ENTRY_T          SWIZZ_CONCAT(SWIZZ_NAME, _entry)
#define SWIZZ_ITER_T           SWIZZ_CONCAT(SWIZZ_NAME, _iter)
#define SWIZZ_INIT             SWIZZ_CONCAT(SWIZZ_NAME, _init)
#define SWIZZ_FREE_FN          SWIZZ_CONCAT(SWIZZ_NAME, _free)
#define SWIZZ_FIND             SWIZZ_CONCAT(SWIZZ_NAME, _find)
#define SWIZZ_ADD              SWIZZ_CONCAT(SWIZZ_NAME, _add)
#define SWIZZ_DELETE           SWIZZ_CONCAT(SWIZZ_NAME, _delete)
#define SWIZZ_REBUILD          SWIZZ_CONCAT(SWIZZ_NAME, _rebuild_cache)
#define SWIZZ_FIND_OR_INSERT   SWIZZ_CONCAT(SWIZZ_NAME, _find_or_insert)
#define SWIZZ_RESERVE          SWIZZ_CONCAT(SWIZZ_NAME, _reserve)
#define SWIZZ_CLEAR            SWIZZ_CONCAT(SWIZZ_NAME, _clear)
#define SWIZZ_COUNT            SWIZZ_CONCAT(SWIZZ_NAME, _count)
#define SWIZZ_CAPACITY         SWIZZ_CONCAT(SWIZZ_NAME, _capacity)
#define SWIZZ_ITER_BEGIN       SWIZZ_CONCAT(SWIZZ_NAME, _iter_begin)
#define SWIZZ_ITER_NEXT        SWIZZ_CONCAT(SWIZZ_NAME, _iter_next)
#define SWIZZ_INSERT_HASHED_   SWIZZ_CONCAT(SWIZZ_NAME, _insert_hashed_internal)
#define SWIZZ_GROW_            SWIZZ_CONCAT(SWIZZ_NAME, _grow_internal)
#define SWIZZ_ALLOC_CTRL_FN    SWIZZ_CONCAT(SWIZZ_NAME, _alloc_ctrl_aligned)
#define SWIZZ_FREE_CTRL_FN     SWIZZ_CONCAT(SWIZZ_NAME, _free_ctrl_aligned)

/* Per-table names so a second #include in this translation unit does not
 * redefine the helpers. Each table closes over its own allocator macros. */
#if SWIZZ_USE_SIMD
static inline uint8_t* SWIZZ_ALLOC_CTRL_FN(size_t cap)
{
    size_t align = SWIZZ_GROUP_WIDTH;
#if defined(SWIZZ_ALLOC_ALIGNED_C11)
    /* aligned_alloc requires size % align == 0. cap is already a power of
     * two >= align, so this always holds. */
    return (uint8_t*)aligned_alloc(align, cap);
#elif defined(SWIZZ_ALLOC_ALIGNED_POSIX)
    void *p = NULL;
    if (posix_memalign(&p, align, cap) != 0) return NULL;
    return (uint8_t*)p;
#else
    /* Hand-rolled: overallocate, align up, tuck original pointer just before. */
    void *raw = SWIZZ_ALLOC_MALLOC(cap + align + sizeof(void*));
    if (!raw) return NULL;
    uintptr_t addr = (uintptr_t)raw + sizeof(void*);
    addr = (addr + align - 1) & ~(uintptr_t)(align - 1);
    ((void**)addr)[-1] = raw;
    return (uint8_t*)addr;
#endif
}

static inline void SWIZZ_FREE_CTRL_FN(uint8_t *ctrl)
{
    if (!ctrl) return;
#if defined(SWIZZ_ALLOC_ALIGNED_C11) || defined(SWIZZ_ALLOC_ALIGNED_POSIX)
    free(ctrl);
#else
    SWIZZ_ALLOC_FREE(((void**)ctrl)[-1]);
#endif
}
#endif

/* ------------------------------------------------------------------
 * Internal entry (key is owned by table)
 * ------------------------------------------------------------------ */
typedef struct SWIZZ_ENTRY_T {
    SWIZZ_KEY_TYPE   key;      /* owned */
    uint64_t         hash;
    SWIZZ_VALUE_TYPE value;
    unsigned         flags;
} SWIZZ_ENTRY_T;

/* ------------------------------------------------------------------
 * The table (public API)
 *
 * `count`      — live entries.
 * `tombstones` — deleted slots not yet reclaimed by a rehash.
 * `generation` — bumped on every rehash; cached entry pointers are invalid
 *                across a generation change.
 *
 * Prefer the accessors SWIZZ_COUNT(&t) / SWIZZ_CAPACITY(&t) over touching
 * struct fields — future revisions may reshape the layout.
 * ------------------------------------------------------------------ */
typedef struct SWIZZ_TABLE_T {
    SWIZZ_ENTRY_T *entries;
    uint8_t       *ctrl;
    uint64_t       bloom[SWIZZ_BLOOM_TOTAL_WORDS];
    uint32_t       capacity;
    uint32_t       count;
    uint32_t       tombstones;
    uint32_t       _pad;
    uint64_t       generation;
} SWIZZ_TABLE_T;

/* Iterator: opaque cursor into the table. Invalidated by any add/delete
 * that triggers a rehash — compare `iter.generation` to `table->generation`. */
typedef struct SWIZZ_ITER_T {
    uint32_t index;
    uint64_t generation;
} SWIZZ_ITER_T;

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */
static inline void SWIZZ_INIT(SWIZZ_TABLE_T *table)
{
    memset(table, 0, sizeof(*table));
}

static inline void SWIZZ_FREE_FN(SWIZZ_TABLE_T *table)
{
    if (table->entries) {
        for (uint32_t i = 0; i < table->capacity; i++) {
            if (table->ctrl[i] != 0x00 && table->ctrl[i] != 0x80 && table->entries[i].key) {
                SWIZZ_FREE_KEY(table->entries[i].key);
            }
        }
        SWIZZ_ALLOC_FREE(table->entries);
#if SWIZZ_USE_SIMD
        SWIZZ_FREE_CTRL_FN(table->ctrl);
#else
        SWIZZ_ALLOC_FREE(table->ctrl);
#endif
    }
    memset(table, 0, sizeof(*table));
}

/* Empty the table but keep the allocated storage. O(capacity). */
static inline void SWIZZ_CLEAR(SWIZZ_TABLE_T *table)
{
    if (!table->entries) return;
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->ctrl[i] != 0x00 && table->ctrl[i] != 0x80 && table->entries[i].key) {
            SWIZZ_FREE_KEY(table->entries[i].key);
            table->entries[i].key = (SWIZZ_KEY_TYPE){0};
        }
    }
    memset(table->ctrl, 0x00, table->capacity);
    memset(table->bloom, 0, sizeof(table->bloom));
    table->count = 0;
    table->tombstones = 0;
    table->generation++;
}

static inline uint32_t SWIZZ_COUNT(const SWIZZ_TABLE_T *table)    { return table->count; }
static inline uint32_t SWIZZ_CAPACITY(const SWIZZ_TABLE_T *table) { return table->capacity; }

/* ------------------------------------------------------------------
 * Internal rehash to a target capacity.
 *
 * Used for growth, tombstone reclamation, and user-requested reserve.
 * Returns true on success; false on OOM (in which case the old table is
 * left untouched).
 *
 * The rehash uses a SIMD-accelerated group scan on the destination to locate
 * empty slots. Since we're rehashing into a freshly zeroed ctrl array, every
 * slot is either empty (0x00) or newly written — no tombstones, no dedup
 * needed (every rehashed entry came from a unique key by construction).
 * ------------------------------------------------------------------ */
static inline bool SWIZZ_GROW_(SWIZZ_TABLE_T *table, uint32_t new_cap)
{
#if SWIZZ_USE_SIMD
    if (new_cap < SWIZZ_GROUP_WIDTH) new_cap = SWIZZ_GROUP_WIDTH;
#else
    if (new_cap < 4) new_cap = 4;
#endif
    /* new_cap must be power of two (callers ensure this, but be defensive). */
    {
        uint32_t p = 1;
        while (p < new_cap) p <<= 1;
        new_cap = p;
    }

    SWIZZ_ENTRY_T *new_entries = SWIZZ_ALLOC_CALLOC(new_cap, sizeof(SWIZZ_ENTRY_T));
#if SWIZZ_USE_SIMD
    uint8_t *new_ctrl = SWIZZ_ALLOC_CTRL_FN(new_cap);
#else
    uint8_t *new_ctrl = SWIZZ_ALLOC_MALLOC(new_cap);
#endif
    if (!new_entries || !new_ctrl) {
        if (new_entries) SWIZZ_ALLOC_FREE(new_entries);
        if (new_ctrl) {
#if SWIZZ_USE_SIMD
            SWIZZ_FREE_CTRL_FN(new_ctrl);
#else
            SWIZZ_ALLOC_FREE(new_ctrl);
#endif
        }
        return false;
    }

    memset(new_ctrl, 0x00, new_cap);
    uint64_t new_bloom[SWIZZ_BLOOM_TOTAL_WORDS];
    memset(new_bloom, 0, sizeof(new_bloom));

    if (table->entries) {
        uint32_t mask = new_cap - 1;
        for (uint32_t i = 0; i < table->capacity; i++) {
            if (table->ctrl[i] == 0x00 || table->ctrl[i] == 0x80 || !table->entries[i].key) continue;

            uint64_t h = table->entries[i].hash;
            uint32_t slot = (uint32_t)(h & mask);

#if SWIZZ_USE_SIMD
            /* SIMD-accelerated linear probe for empty slot on the destination.
             * On a fresh table every non-empty slot is a real entry (no
             * tombstones), so probe_empty is the right predicate. */
            while (1) {
                uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
                int em = swizz_probe_empty(&new_ctrl[group_start]);
                int offset_in_group = slot - group_start;
                em &= ~((1 << offset_in_group) - 1);
                if (em) {
                    slot = group_start + __builtin_ctz(em);
                    break;
                }
                slot = (group_start + SWIZZ_GROUP_WIDTH) & mask;
            }
#else
            while (new_ctrl[slot] != 0x00) slot = (slot + 1) & mask;
#endif

            new_entries[slot] = table->entries[i];  /* move ownership */
            new_ctrl[slot] = entry_fingerprint(h);
            bloom_add(new_bloom, h);
        }

        SWIZZ_ALLOC_FREE(table->entries);
#if SWIZZ_USE_SIMD
        SWIZZ_FREE_CTRL_FN(table->ctrl);
#else
        SWIZZ_ALLOC_FREE(table->ctrl);
#endif
    }

    memcpy(table->bloom, new_bloom, sizeof(table->bloom));
    table->entries = new_entries;
    table->ctrl = new_ctrl;
    table->capacity = new_cap;
    table->tombstones = 0;
    /* count is unchanged — every live entry was moved */
    table->generation++;
    return true;
}

/* Public rebuild: compact the table to the smallest power-of-two capacity
 * that still respects the load factor. Reclaims tombstones. No-op on
 * uninitialized/empty tables. */
static inline void SWIZZ_REBUILD(SWIZZ_TABLE_T *table)
{
    if (!table->entries) return;
    if (table->count == 0) { SWIZZ_FREE_FN(table); return; }
    uint32_t cap = 4;
    while (cap <= table->count || SWIZZ_LOAD_FACTOR(cap) < table->count) cap <<= 1;
    (void)SWIZZ_GROW_(table, cap);
}

/* Reserve enough capacity to hold at least `n` entries without rehashing.
 * Returns true on success (or when no growth is needed); false on OOM. */
static inline bool SWIZZ_RESERVE(SWIZZ_TABLE_T *table, uint32_t n)
{
    if (n <= SWIZZ_LOAD_FACTOR(table->capacity)) return true;
    uint32_t cap = table->capacity ? table->capacity : 4;
    while (SWIZZ_LOAD_FACTOR(cap) < n) cap <<= 1;
    return SWIZZ_GROW_(table, cap);
}

/* ------------------------------------------------------------------
 * Core operations
 * ------------------------------------------------------------------ */
static inline SWIZZ_ENTRY_T* SWIZZ_FIND(SWIZZ_TABLE_T *table, SWIZZ_KEY_TYPE key)
{
    if (!table->entries || table->count == 0) return NULL;

    uint64_t h = SWIZZ_HASH(key);
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));

    /* Prefetch unconditionally — modern hardware treats a prefetch as a hint,
     * so an unused one is free but a needed one saves a full cache miss. The
     * bloom filter's role here is *not* speculative gating; it's a separate
     * short-circuit for genuine misses when the probe would otherwise walk
     * multiple groups. */
    swizz_prefetch(&table->ctrl[slot]);
    swizz_prefetch(&table->entries[slot]);

    /* Bloom short-circuit for large tables where false-positive rate stays
     * low. Below the threshold the ctrl array fits in one or two cache lines
     * and the SIMD probe is cheaper than a bloom check. In delete-heavy
     * workloads bloom bits stay set (we don't clear on delete), so gating
     * on it can cost throughput; keeping it only on large tables limits that. */
    if (cap >= SWIZZ_BLOOM_MIN_CAP && !bloom_may_contain(table->bloom, h)) {
        return NULL;
    }

    uint8_t fp = entry_fingerprint(h);

#if SWIZZ_USE_SIMD
    /* SIMD-accelerated group probing. Iteration is bounded by the number of
     * groups (plus one wrap-around pass over the starting group's head) —
     * this both handles wrap-around correctly and prevents infinite loops
     * when a small table happens to contain no empty slots (all deleted). */
    uint32_t start_group = slot & ~(SWIZZ_GROUP_WIDTH - 1);
    uint32_t start_offset = slot - start_group;
    uint32_t groups_visited = 0;
    uint32_t total_groups = cap / SWIZZ_GROUP_WIDTH;
    int first_group = 1;
    swizz_group_t fp_bcast = swizz_group_bcast(fp);
    swizz_group_t empty_bcast = swizz_group_bcast(0x00);
    while (groups_visited <= total_groups) {
        uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
        /* One ctrl load, two independent SIMD compares — the group value
         * is reused, avoiding a redundant memory access per iteration. */
        swizz_group_t group = swizz_group_load((const swizz_group_t*)&table->ctrl[group_start]);
        int mask       = swizz_group_mask(swizz_group_cmpeq(group, fp_bcast));
        int empty_mask = swizz_group_mask(swizz_group_cmpeq(group, empty_bcast));

        if (first_group) {
            uint32_t head_mask = (start_offset == 0) ? 0 : (uint32_t)((1 << start_offset) - 1);
            mask       &= ~head_mask;
            empty_mask &= ~head_mask;
            first_group = 0;
        } else if (groups_visited == total_groups) {
            if (start_offset == 0) break;
            uint32_t head_mask = (uint32_t)((1 << start_offset) - 1);
            mask       &= head_mask;
            empty_mask &= head_mask;
        }

        /* The SIMD compare already established ctrl[group_start + bit] == fp. */
        while (mask != 0) {
            int bit = __builtin_ctz(mask);
            uint32_t candidate = group_start + bit;
            SWIZZ_ENTRY_T *e = &table->entries[candidate];
            if (SWIZZ_EQ(e->key, key)) return e;
            mask &= mask - 1;
        }

        if (empty_mask != 0) return NULL;

        slot = (group_start + SWIZZ_GROUP_WIDTH) & (cap - 1);
        groups_visited++;
    }
    (void)start_group;
    return NULL;
#else
    while (table->ctrl[slot] != 0x00) {
        if (table->ctrl[slot] == fp) {
            SWIZZ_ENTRY_T *e = &table->entries[slot];
            if (SWIZZ_EQ(e->key, key)) return e;
        }
        slot = (slot + 1) & (cap - 1);
    }
    return NULL;
#endif
}

/* Internal: given a table with known-sufficient capacity and a hash, find
 * the first empty-or-tombstone slot in the probe sequence. Writes the entry
 * and updates count/tombstones/bloom. Returns pointer to the inserted slot,
 * or NULL only if the probe couldn't find one (which the load-factor
 * invariant should prevent). */
static inline SWIZZ_ENTRY_T* SWIZZ_INSERT_HASHED_(SWIZZ_TABLE_T *table,
                                                   uint64_t h, SWIZZ_KEY_TYPE key_copy,
                                                   SWIZZ_VALUE_TYPE value, unsigned flags)
{
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    int was_tombstone = 0;

#if SWIZZ_USE_SIMD
    uint32_t start_group = slot & ~(SWIZZ_GROUP_WIDTH - 1);
    uint32_t start_offset = slot - start_group;
    uint32_t groups_visited = 0;
    uint32_t total_groups = cap / SWIZZ_GROUP_WIDTH;
    int first_group = 1;
    int found = 0;
    while (groups_visited <= total_groups) {
        uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
        int mask = swizz_probe_available(&table->ctrl[group_start]);

        if (first_group) {
            mask &= ~((1 << start_offset) - 1);
            first_group = 0;
        } else if (groups_visited == total_groups) {
            if (start_offset == 0) break;
            mask &= (1 << start_offset) - 1;
        }

        if (mask != 0) {
            int bit = __builtin_ctz(mask);
            slot = group_start + bit;
            was_tombstone = (table->ctrl[slot] == 0x80);
            found = 1;
            break;
        }

        slot = (group_start + SWIZZ_GROUP_WIDTH) & (cap - 1);
        groups_visited++;
    }
    if (!found) return NULL;
    (void)start_group;
#else
    while (table->ctrl[slot] != 0x00 && table->ctrl[slot] != 0x80) {
        slot = (slot + 1) & (cap - 1);
    }
    was_tombstone = (table->ctrl[slot] == 0x80);
#endif

    SWIZZ_ENTRY_T e = {0};
    e.key = key_copy;
    e.hash = h;
    e.value = value;
    e.flags = flags;

    table->entries[slot] = e;
    table->ctrl[slot] = entry_fingerprint(h);
    bloom_add(table->bloom, h);
    table->count++;
    if (was_tombstone) table->tombstones--;

    return &table->entries[slot];
}

static inline SWIZZ_ENTRY_T* SWIZZ_ADD(SWIZZ_TABLE_T *table, SWIZZ_KEY_TYPE key, SWIZZ_VALUE_TYPE value, unsigned flags)
{
    /* Compute hash once. If the table is empty, allocate before probing so
     * we don't need a separate zero-check inside the probe loop. */
    uint64_t h = SWIZZ_HASH(key);

    if (!table->entries) {
#if SWIZZ_USE_SIMD
        uint32_t initial = (4 > SWIZZ_GROUP_WIDTH) ? 4 : SWIZZ_GROUP_WIDTH;
#else
        uint32_t initial = 4;
#endif
        if (!SWIZZ_GROW_(table, initial)) return NULL;
    }

    /* Unified probe: walk the probe sequence looking for either the key
     * (update path) or the first empty/tombstone slot (insert path). This
     * saves us from doing a full FIND + a separate insert probe. We remember
     * the first tombstone we see so we can insert there if the key isn't
     * found later in the sequence — this keeps probe chains short. */
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    uint8_t fp = entry_fingerprint(h);

    swizz_prefetch(&table->ctrl[slot]);
    swizz_prefetch(&table->entries[slot]);

    uint32_t insert_slot = UINT32_MAX;
    int insert_was_tomb = 0;

#if SWIZZ_USE_SIMD
    uint32_t start_group = slot & ~(SWIZZ_GROUP_WIDTH - 1);
    uint32_t start_offset = slot - start_group;
    uint32_t groups_visited = 0;
    uint32_t total_groups = cap / SWIZZ_GROUP_WIDTH;
    int first_group = 1;
    swizz_group_t fp_bcast = swizz_group_bcast(fp);
    swizz_group_t empty_bcast = swizz_group_bcast(0x00);
    swizz_group_t tomb_bcast = swizz_group_bcast(0x80);

    while (groups_visited <= total_groups) {
        uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
        swizz_group_t group = swizz_group_load((const swizz_group_t*)&table->ctrl[group_start]);
        int match_mask = swizz_group_mask(swizz_group_cmpeq(group, fp_bcast));
        int empty_mask = swizz_group_mask(swizz_group_cmpeq(group, empty_bcast));
        int tomb_mask  = swizz_group_mask(swizz_group_cmpeq(group, tomb_bcast));

        uint32_t head_clear = 0;
        if (first_group) {
            head_clear = (start_offset == 0) ? 0 : (uint32_t)((1 << start_offset) - 1);
            match_mask &= ~head_clear;
            empty_mask &= ~head_clear;
            tomb_mask  &= ~head_clear;
            first_group = 0;
        } else if (groups_visited == total_groups) {
            if (start_offset == 0) break;
            uint32_t keep = (uint32_t)((1 << start_offset) - 1);
            match_mask &= keep;
            empty_mask &= keep;
            tomb_mask  &= keep;
        }

        while (match_mask != 0) {
            int bit = __builtin_ctz(match_mask);
            uint32_t candidate = group_start + bit;
            SWIZZ_ENTRY_T *e = &table->entries[candidate];
            if (SWIZZ_EQ(e->key, key)) {
                /* Update in place — no allocation, no bloom write. */
                e->value = value;
                e->flags = flags;
                return e;
            }
            match_mask &= match_mask - 1;
        }

        /* Remember first tombstone as candidate insertion slot. */
        if (insert_slot == UINT32_MAX && tomb_mask != 0) {
            insert_slot = group_start + __builtin_ctz(tomb_mask);
            insert_was_tomb = 1;
        }

        if (empty_mask != 0) {
            /* Empty slot terminates the probe. Insert at this empty (or at
             * the earlier tombstone if we saw one — reclaiming keeps chains short). */
            if (insert_slot == UINT32_MAX) {
                insert_slot = group_start + __builtin_ctz(empty_mask);
                insert_was_tomb = 0;
            }
            break;
        }

        slot = (group_start + SWIZZ_GROUP_WIDTH) & (cap - 1);
        groups_visited++;
    }
    (void)start_group;
#else
    while (table->ctrl[slot] != 0x00) {
        if (table->ctrl[slot] == fp) {
            SWIZZ_ENTRY_T *e = &table->entries[slot];
            if (SWIZZ_EQ(e->key, key)) {
                e->value = value;
                e->flags = flags;
                return e;
            }
        } else if (table->ctrl[slot] == 0x80 && insert_slot == UINT32_MAX) {
            insert_slot = slot;
            insert_was_tomb = 1;
        }
        slot = (slot + 1) & (cap - 1);
    }
    if (insert_slot == UINT32_MAX) {
        insert_slot = slot;
        insert_was_tomb = 0;
    }
#endif

    /* Key not present — need to insert. Check capacity now that we know
     * we're actually going to allocate. */
    if (table->count + 1 > SWIZZ_LOAD_FACTOR(table->capacity)) {
        if (!SWIZZ_GROW_(table, table->capacity * 2)) return NULL;
        /* Post-grow: probe positions moved. Fall back to the helper. */
        SWIZZ_KEY_TYPE key_copy = SWIZZ_DUP_KEY(key);
        if (!key_copy && key) return NULL;
        SWIZZ_ENTRY_T *r = SWIZZ_INSERT_HASHED_(table, h, key_copy, value, flags);
        if (!r) SWIZZ_FREE_KEY(key_copy);
        return r;
    }
    if (table->tombstones > (table->capacity >> 2) &&
        table->count + table->tombstones + 1 > SWIZZ_LOAD_FACTOR(table->capacity)) {
        if (!SWIZZ_GROW_(table, table->capacity)) return NULL;
        SWIZZ_KEY_TYPE key_copy = SWIZZ_DUP_KEY(key);
        if (!key_copy && key) return NULL;
        SWIZZ_ENTRY_T *r = SWIZZ_INSERT_HASHED_(table, h, key_copy, value, flags);
        if (!r) SWIZZ_FREE_KEY(key_copy);
        return r;
    }

    /* Ordinary insert at the slot the unified probe already selected. */
    SWIZZ_KEY_TYPE key_copy = SWIZZ_DUP_KEY(key);
    if (!key_copy && key) return NULL;

    SWIZZ_ENTRY_T e = {0};
    e.key = key_copy;
    e.hash = h;
    e.value = value;
    e.flags = flags;

    table->entries[insert_slot] = e;
    table->ctrl[insert_slot] = fp;
    bloom_add(table->bloom, h);
    table->count++;
    if (insert_was_tomb) table->tombstones--;

    return &table->entries[insert_slot];
}

/* Find the entry for `key`, or insert one initialized to `default_value` and
 * `default_flags` if absent. Sets `*inserted` to 1 on insert, 0 on find (may
 * be NULL). Saves the caller from hashing twice on the get-or-create pattern. */
static inline SWIZZ_ENTRY_T* SWIZZ_FIND_OR_INSERT(SWIZZ_TABLE_T *table, SWIZZ_KEY_TYPE key,
                                                   SWIZZ_VALUE_TYPE default_value,
                                                   unsigned default_flags, int *inserted)
{
    SWIZZ_ENTRY_T *existing = SWIZZ_FIND(table, key);
    if (existing) {
        if (inserted) *inserted = 0;
        return existing;
    }
    SWIZZ_ENTRY_T *e = SWIZZ_ADD(table, key, default_value, default_flags);
    if (inserted) *inserted = (e != NULL);
    return e;
}

static inline bool SWIZZ_DELETE(SWIZZ_TABLE_T *table, SWIZZ_KEY_TYPE key)
{
    if (!table->entries || table->count == 0) return false;

    uint64_t h = SWIZZ_HASH(key);
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    uint8_t fp = entry_fingerprint(h);

#if SWIZZ_USE_SIMD
    /* SIMD-accelerated group probing — bounded by group count (plus one
     * wrap-around pass), matching the semantics of SWIZZ_FIND. */
    uint32_t start_group = slot & ~(SWIZZ_GROUP_WIDTH - 1);
    uint32_t start_offset = slot - start_group;
    uint32_t groups_visited = 0;
    uint32_t total_groups = cap / SWIZZ_GROUP_WIDTH;
    int first_group = 1;
    swizz_group_t fp_bcast = swizz_group_bcast(fp);
    swizz_group_t empty_bcast = swizz_group_bcast(0x00);
    while (groups_visited <= total_groups) {
        uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
        swizz_group_t group = swizz_group_load((const swizz_group_t*)&table->ctrl[group_start]);
        int mask       = swizz_group_mask(swizz_group_cmpeq(group, fp_bcast));
        int empty_mask = swizz_group_mask(swizz_group_cmpeq(group, empty_bcast));

        if (first_group) {
            uint32_t head_mask = (start_offset == 0) ? 0 : (uint32_t)((1 << start_offset) - 1);
            mask       &= ~head_mask;
            empty_mask &= ~head_mask;
            first_group = 0;
        } else if (groups_visited == total_groups) {
            if (start_offset == 0) break;
            uint32_t head_mask = (uint32_t)((1 << start_offset) - 1);
            mask       &= head_mask;
            empty_mask &= head_mask;
        }

        while (mask != 0) {
            int bit = __builtin_ctz(mask);
            uint32_t candidate = group_start + bit;
            SWIZZ_ENTRY_T *e = &table->entries[candidate];
            if (SWIZZ_EQ(e->key, key)) {
                table->ctrl[candidate] = 0x80;
                SWIZZ_FREE_KEY(e->key);
                e->key = (SWIZZ_KEY_TYPE){0};
                e->hash = 0;
                table->count--;
                table->tombstones++;
                return true;
            }
            mask &= mask - 1;
        }

        if (empty_mask != 0) return false;

        slot = (group_start + SWIZZ_GROUP_WIDTH) & (cap - 1);
        groups_visited++;
    }
    (void)start_group;
    return false;
#else
    while (table->ctrl[slot] != 0x00) {
        if (table->ctrl[slot] == fp) {
            SWIZZ_ENTRY_T *e = &table->entries[slot];
            if (SWIZZ_EQ(e->key, key)) {
                table->ctrl[slot] = 0x80;
                SWIZZ_FREE_KEY(e->key);
                e->key = (SWIZZ_KEY_TYPE){0};
                e->hash = 0;
                table->count--;
                table->tombstones++;
                return true;
            }
        }
        slot = (slot + 1) & (cap - 1);
    }
    return false;
#endif
}

/* ------------------------------------------------------------------
 * Iterator API
 *
 * Usage:
 *   SWIZZ_ITER_T it = swizz_iter_begin(&table);
 *   SWIZZ_ENTRY_T *e;
 *   while ((e = swizz_iter_next(&table, &it))) { ... }
 *
 * Or with the foreach macro:
 *   SWIZZ_FOREACH(&table, entry) { ... entry->key ... }
 *
 * Iteration order is arbitrary (slot order) and unstable across rehashes.
 * If `table->generation != it.generation`, the iterator has been invalidated
 * by an intervening add/delete-triggered rehash — restart iteration.
 * ------------------------------------------------------------------ */
static inline SWIZZ_ITER_T SWIZZ_ITER_BEGIN(const SWIZZ_TABLE_T *table)
{
    SWIZZ_ITER_T it;
    it.index = 0;
    it.generation = table->generation;
    return it;
}

static inline SWIZZ_ENTRY_T* SWIZZ_ITER_NEXT(SWIZZ_TABLE_T *table, SWIZZ_ITER_T *it)
{
    if (!table->entries) return NULL;
    while (it->index < table->capacity) {
        uint32_t i = it->index++;
        if (table->ctrl[i] != 0x00 && table->ctrl[i] != 0x80 && table->entries[i].key) {
            return &table->entries[i];
        }
    }
    return NULL;
}

/* Iteration helper: users may prefer to write the loop by hand with the
 * iter_begin/iter_next pair above. Example:
 *
 *     symtab_iter it = symtab_iter_begin(&t);
 *     symtab_entry *e;
 *     while ((e = symtab_iter_next(&t, &it))) { ... }
 */

/* Undefine macros to allow re-inclusion with different parameters */
#undef SWIZZ_NAME
#undef SWIZZ_KEY_TYPE
#undef SWIZZ_VALUE_TYPE
#undef SWIZZ_HASH
#undef SWIZZ_EQ
#undef SWIZZ_DUP_KEY
#undef SWIZZ_FREE_KEY
#undef SWIZZ_ALLOC_MALLOC
#undef SWIZZ_ALLOC_CALLOC
#undef SWIZZ_ALLOC_FREE
#undef SWIZZ_ALLOC_MALLOC_USER
#undef SWIZZ_ALLOC_ALIGNED_HANDROLLED
#undef SWIZZ_ALLOC_ALIGNED_C11
#undef SWIZZ_ALLOC_ALIGNED_POSIX
#undef SWIZZ_ALLOC_CTRL_FN
#undef SWIZZ_FREE_CTRL_FN
#undef SWIZZ_CONCAT
#undef SWIZZ_TABLE_T
#undef SWIZZ_ENTRY_T
#undef SWIZZ_ITER_T
#undef SWIZZ_INIT
#undef SWIZZ_FREE_FN
#undef SWIZZ_FIND
#undef SWIZZ_ADD
#undef SWIZZ_DELETE
#undef SWIZZ_REBUILD
#undef SWIZZ_FIND_OR_INSERT
#undef SWIZZ_RESERVE
#undef SWIZZ_CLEAR
#undef SWIZZ_COUNT
#undef SWIZZ_CAPACITY
#undef SWIZZ_ITER_BEGIN
#undef SWIZZ_ITER_NEXT
#undef SWIZZ_INSERT_HASHED_
#undef SWIZZ_GROW_
