/* swizz.h — SwizzTable bgen-style generator
 * C99 header-only, zero dependencies, -pedantic -Werror clean.
 * Drop this in your project and #include it after defining the macros above.
 * Multiple independent tables by re-#defining and re-including.
 */

#ifndef SWIZZ_H_INTERNAL_GUARD
#define SWIZZ_H_INTERNAL_GUARD

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------
 * Original helpers (available for use in user macros)
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

static inline uint8_t entry_fingerprint(uint64_t h)
{
    uint8_t fp = (uint8_t)(h >> 56);
    if (fp == 0x00 || fp == 0x80) fp |= 0x01;
    return fp;
}

static inline void bloom_add(uint64_t bloom[4], uint64_t h)
{
    for (int i = 0; i < 4; i++) {
        uint64_t k = h * (0x9e3779b97f4a7c15ULL + (uint64_t)i * 0x517cc1b727220a95ULL);
        uint32_t bit = (uint32_t)(k >> 32) & 0xFF;
        bloom[bit >> 6] |= (1ULL << (bit & 63));
    }
}

static inline bool bloom_may_contain(const uint64_t bloom[4], uint64_t h)
{
    for (int i = 0; i < 4; i++) {
        uint64_t k = h * (0x9e3779b97f4a7c15ULL + (uint64_t)i * 0x517cc1b727220a95ULL);
        uint32_t bit = (uint32_t)(k >> 32) & 0xFF;
        if (!(bloom[bit >> 6] & (1ULL << (bit & 63)))) return false;
    }
    return true;
}

/* ------------------------------------------------------------------
 * SIMD group probing configuration (AVX2 / SSE2 / NEON / scalar)
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
   /* NEON movemask: extract comparison results to 16-bit mask */
   static inline int swizz_neon_movemask(uint8x16_t v) {
       /* v has 0xFF for equal bytes - extract bit 7 from each byte */
       uint8_t bytes[16];
       vst1q_u8(bytes, v);
       int mask = 0;
       for (int i = 0; i < 16; i++) {
           mask |= ((bytes[i] >> 7) & 1) << i;
       }
       return mask;
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
#endif
#ifndef SWIZZ_ALLOC_CALLOC
#define SWIZZ_ALLOC_CALLOC(nmemb, size) calloc(nmemb, size)
#endif
#ifndef SWIZZ_ALLOC_FREE
#define SWIZZ_ALLOC_FREE(p) free(p)
#endif

/* ------------------------------------------------------------------
 * Aligned allocation for SIMD control bytes
 * ------------------------------------------------------------------ */
#if SWIZZ_USE_SIMD
/* Aligned allocation: stores original pointer at start for later free */
static inline uint8_t* swizz_alloc_ctrl_aligned(size_t cap)
{
    size_t align = SWIZZ_GROUP_WIDTH;
    /* Allocate extra space for alignment padding and original pointer storage */
    void *raw = SWIZZ_ALLOC_MALLOC(cap + align + sizeof(void*));
    if (!raw) return NULL;
    
    /* Calculate aligned address */
    uintptr_t addr = (uintptr_t)raw + sizeof(void*);
    addr = (addr + align - 1) & ~(align - 1);
    
    /* Store original pointer just before aligned address */
    void **storage = (void**)(addr - sizeof(void*));
    *storage = raw;
    
    return (uint8_t*)addr;
}

/* Free aligned control bytes - reads original pointer from storage */
static inline void swizz_free_ctrl_aligned(uint8_t *ctrl)
{
    if (!ctrl) return;
    void **storage = (void**)((uintptr_t)ctrl - sizeof(void*));
    SWIZZ_ALLOC_FREE(*storage);
}
#endif

/* ------------------------------------------------------------------
 * Token pasting helpers (two-stage to force macro expansion)
 * ------------------------------------------------------------------ */
#define SWIZZ_CONCAT_(x, y) x##y
#define SWIZZ_CONCAT(x, y) SWIZZ_CONCAT_(x, y)
#define SWIZZ_TABLE_T  SWIZZ_CONCAT(SWIZZ_NAME, _table)
#define SWIZZ_ENTRY_T  SWIZZ_CONCAT(SWIZZ_NAME, _entry)
#define SWIZZ_INIT     SWIZZ_CONCAT(SWIZZ_NAME, _init)
#define SWIZZ_FREE_FN  SWIZZ_CONCAT(SWIZZ_NAME, _free)
#define SWIZZ_FIND     SWIZZ_CONCAT(SWIZZ_NAME, _find)
#define SWIZZ_ADD      SWIZZ_CONCAT(SWIZZ_NAME, _add)
#define SWIZZ_DELETE   SWIZZ_CONCAT(SWIZZ_NAME, _delete)
#define SWIZZ_REBUILD  SWIZZ_CONCAT(SWIZZ_NAME, _rebuild_cache)

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
 * ------------------------------------------------------------------ */
typedef struct SWIZZ_TABLE_T {
    SWIZZ_ENTRY_T *entries;
    uint8_t       *ctrl;
    uint64_t       bloom[4];
    uint32_t       capacity;
    uint32_t       count;
    uint64_t       generation;
} SWIZZ_TABLE_T;

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
        swizz_free_ctrl_aligned(table->ctrl);
#else
        SWIZZ_ALLOC_FREE(table->ctrl);
#endif
    }
    memset(table, 0, sizeof(*table));
}

/* ------------------------------------------------------------------
 * Internal rebuild (compaction after heavy deletes)
 * ------------------------------------------------------------------ */
static inline void SWIZZ_REBUILD(SWIZZ_TABLE_T *table)
{
    if (!table->entries || table->count == 0) return;

    uint32_t valid_count = 0;
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->ctrl[i] != 0x00 && table->ctrl[i] != 0x80 && table->entries[i].key)
            valid_count++;
    }
    if (valid_count == 0) {
        SWIZZ_FREE_FN(table);
        return;
    }

    uint32_t cap = 4;
    while (cap < valid_count * 2) cap <<= 1;
#if SWIZZ_USE_SIMD
    if (cap < SWIZZ_GROUP_WIDTH) cap = SWIZZ_GROUP_WIDTH;
#endif

    SWIZZ_ENTRY_T *new_entries = SWIZZ_ALLOC_CALLOC(cap, sizeof(SWIZZ_ENTRY_T));
#if SWIZZ_USE_SIMD
    uint8_t *new_ctrl = swizz_alloc_ctrl_aligned(cap);
#else
    uint8_t *new_ctrl = SWIZZ_ALLOC_MALLOC(cap);
#endif
    if (!new_entries || !new_ctrl) return;  /* OOM — leave old table intact */

    memset(new_ctrl, 0x00, cap);
    memset(table->bloom, 0, sizeof(table->bloom));

    uint32_t new_count = 0;
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->ctrl[i] == 0x00 || table->ctrl[i] == 0x80 || !table->entries[i].key) continue;

        SWIZZ_ENTRY_T *p = &table->entries[i];
        uint64_t h = p->hash;
        uint32_t slot = (uint32_t)(h & (cap - 1));

        while (new_ctrl[slot] != 0x00) {
            if (new_ctrl[slot] != 0x80 && SWIZZ_EQ(new_entries[slot].key, p->key)) {
                new_entries[slot] = *p;  /* move ownership */
                goto next;
            }
            slot = (slot + 1) & (cap - 1);
        }

        new_entries[slot] = *p;
        new_ctrl[slot] = entry_fingerprint(h);
        bloom_add(table->bloom, h);
        new_count++;
    next:;
    }

    SWIZZ_ALLOC_FREE(table->entries);
#if SWIZZ_USE_SIMD
    swizz_free_ctrl_aligned(table->ctrl);
#else
    SWIZZ_ALLOC_FREE(table->ctrl);
#endif
    table->entries = new_entries;
    table->ctrl = new_ctrl;
    table->capacity = cap;
    table->count = new_count;
    table->generation++;
}

/* ------------------------------------------------------------------
 * Core operations
 * ------------------------------------------------------------------ */
static inline SWIZZ_ENTRY_T* SWIZZ_FIND(SWIZZ_TABLE_T *table, SWIZZ_KEY_TYPE key)
{
    if (!table->entries || table->count == 0) return NULL;

    uint64_t h = SWIZZ_HASH(key);
    if (!bloom_may_contain(table->bloom, h)) return NULL;

    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    uint8_t fp = entry_fingerprint(h);

#if SWIZZ_USE_SIMD
    /* SIMD-accelerated group probing */
    uint32_t start_slot = slot;
    while (1) {
        uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
        int mask = swizz_probe_group(&table->ctrl[group_start], fp);
        
        /* Check each matching slot in the group */
        while (mask != 0) {
            int bit = __builtin_ctz(mask);
            uint32_t candidate = group_start + bit;
            if (candidate >= cap) candidate -= cap; /* wrap around */
            
            if (table->ctrl[candidate] == fp) {
                SWIZZ_ENTRY_T *e = &table->entries[candidate];
                if (SWIZZ_EQ(e->key, key)) return e;
            }
            mask &= mask - 1; /* clear lowest bit */
        }
        
        /* Check for empty slot at or after the starting position within the group.
         * We only stop if we find an empty slot in the probe sequence.
         * For the first group, check from starting offset. For subsequent groups, check all. */
        int empty_mask = swizz_probe_empty(&table->ctrl[group_start]);
        if (slot == start_slot) {
            /* First group: check empty slots at or after start position */
            int offset_in_group = slot - group_start;
            empty_mask &= ~((1 << offset_in_group) - 1);
        }
        if (empty_mask != 0) return NULL;
        
        /* Move to next group */
        slot = (group_start + SWIZZ_GROUP_WIDTH) & (cap - 1);
        if (slot == start_slot) return NULL; /* full circle */
    }
#else
    /* Scalar fallback */
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

static inline SWIZZ_ENTRY_T* SWIZZ_ADD(SWIZZ_TABLE_T *table, SWIZZ_KEY_TYPE key, SWIZZ_VALUE_TYPE value, unsigned flags)
{
    SWIZZ_KEY_TYPE key_copy = SWIZZ_DUP_KEY(key);
    if (!key_copy && key) return NULL;

    uint64_t h = SWIZZ_HASH(key);  /* hash before possible dup failure */

    /* check for existing */
    SWIZZ_ENTRY_T *existing = SWIZZ_FIND(table, key);
    if (existing) {
        SWIZZ_FREE_KEY(key_copy);
        existing->value = value;
        existing->flags = flags;
        return existing;
    }

    /* first insert — allocate initial table */
    if (!table->entries) {
#if SWIZZ_USE_SIMD
        /* Minimum capacity must be at least group width for SIMD loads */
        table->capacity = (4 > SWIZZ_GROUP_WIDTH) ? 4 : SWIZZ_GROUP_WIDTH;
#else
        table->capacity = 4;
#endif
        table->entries = SWIZZ_ALLOC_CALLOC(table->capacity, sizeof(SWIZZ_ENTRY_T));
#if SWIZZ_USE_SIMD
        table->ctrl = swizz_alloc_ctrl_aligned(table->capacity);
#else
        table->ctrl = SWIZZ_ALLOC_MALLOC(table->capacity);
#endif
        if (!table->entries || !table->ctrl) {
            SWIZZ_FREE_KEY(key_copy);
            return NULL;
        }
        memset(table->ctrl, 0x00, table->capacity);
        memset(table->bloom, 0, sizeof(table->bloom));
        table->generation = 1;
    }
    /* grow if load factor exceeded */
    else if (table->count >= SWIZZ_LOAD_FACTOR(table->capacity)) {
        uint32_t new_cap = table->capacity * 2;
#if SWIZZ_USE_SIMD
        /* Ensure new capacity is at least group width */
        if (new_cap < SWIZZ_GROUP_WIDTH) new_cap = SWIZZ_GROUP_WIDTH;
#endif
        SWIZZ_ENTRY_T *new_entries = SWIZZ_ALLOC_CALLOC(new_cap, sizeof(SWIZZ_ENTRY_T));
#if SWIZZ_USE_SIMD
        uint8_t *new_ctrl = swizz_alloc_ctrl_aligned(new_cap);
#else
        uint8_t *new_ctrl = SWIZZ_ALLOC_MALLOC(new_cap);
#endif
        if (!new_entries || !new_ctrl) {
            SWIZZ_FREE_KEY(key_copy);
            return NULL;
        }
        memset(new_ctrl, 0x00, new_cap);
        memset(table->bloom, 0, sizeof(table->bloom));

        for (uint32_t i = 0; i < table->capacity; i++) {
            if (table->ctrl[i] != 0x00 && table->ctrl[i] != 0x80 && table->entries[i].key) {
                uint64_t eh = table->entries[i].hash;
                uint32_t slot = (uint32_t)(eh & (new_cap - 1));
                while (new_ctrl[slot] != 0x00) slot = (slot + 1) & (new_cap - 1);
                new_entries[slot] = table->entries[i];  /* move */
                new_ctrl[slot] = entry_fingerprint(eh);
                bloom_add(table->bloom, eh);
            }
        }

        SWIZZ_ALLOC_FREE(table->entries);
#if SWIZZ_USE_SIMD
        swizz_free_ctrl_aligned(table->ctrl);
#else
        SWIZZ_ALLOC_FREE(table->ctrl);
#endif
        table->entries = new_entries;
        table->ctrl = new_ctrl;
        table->capacity = new_cap;
        table->generation++;
    }

    /* insert - find empty or deleted slot */
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    
#if SWIZZ_USE_SIMD
    /* SIMD-accelerated search for available slot */
    uint32_t start_slot = slot;
    int first_group = 1;
    while (1) {
        uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
        int mask = swizz_probe_available(&table->ctrl[group_start]);
        
        /* On first iteration, clear bits before the starting slot within the group */
        if (first_group) {
            int offset_in_group = slot - group_start;
            mask &= ~((1 << offset_in_group) - 1);
            first_group = 0;
        }
        
        if (mask != 0) {
            int bit = __builtin_ctz(mask);
            slot = group_start + bit;
            if (slot >= cap) slot -= cap; /* wrap around */
            break;
        }
        
        /* Move to next group */
        slot = (group_start + SWIZZ_GROUP_WIDTH) & (cap - 1);
        if (slot == start_slot) return NULL; /* table full (shouldn't happen) */
    }
#else
    /* Scalar fallback */
    while (table->ctrl[slot] != 0x00 && table->ctrl[slot] != 0x80) {
        slot = (slot + 1) & (cap - 1);
    }
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

    return &table->entries[slot];
}

static inline bool SWIZZ_DELETE(SWIZZ_TABLE_T *table, SWIZZ_KEY_TYPE key)
{
    if (!table->entries || table->count == 0) return false;

    uint64_t h = SWIZZ_HASH(key);
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    uint8_t fp = entry_fingerprint(h);

#if SWIZZ_USE_SIMD
    /* SIMD-accelerated group probing */
    uint32_t start_slot = slot;
    while (1) {
        uint32_t group_start = slot & ~(SWIZZ_GROUP_WIDTH - 1);
        int mask = swizz_probe_group(&table->ctrl[group_start], fp);
        
        /* Check each matching slot in the group */
        while (mask != 0) {
            int bit = __builtin_ctz(mask);
            uint32_t candidate = group_start + bit;
            if (candidate >= cap) candidate -= cap; /* wrap around */
            
            if (table->ctrl[candidate] == fp) {
                SWIZZ_ENTRY_T *e = &table->entries[candidate];
                if (SWIZZ_EQ(e->key, key)) {
                    table->ctrl[candidate] = 0x80;
                    SWIZZ_FREE_KEY(e->key);
                    e->key = NULL;
                    e->hash = 0;
                    table->count--;
                    return true;
                }
            }
            mask &= mask - 1; /* clear lowest bit */
        }
        
        /* Check for empty slot at or after the starting position within the group */
        int empty_mask = swizz_probe_empty(&table->ctrl[group_start]);
        if (slot == start_slot) {
            /* First group: check empty slots at or after start position */
            int offset_in_group = slot - group_start;
            empty_mask &= ~((1 << offset_in_group) - 1);
        }
        if (empty_mask != 0) return false;
        
        /* Move to next group */
        slot = (group_start + SWIZZ_GROUP_WIDTH) & (cap - 1);
        if (slot == start_slot) return false; /* full circle */
    }
#else
    /* Scalar fallback */
    while (table->ctrl[slot] != 0x00) {
        if (table->ctrl[slot] == fp) {
            SWIZZ_ENTRY_T *e = &table->entries[slot];
            if (SWIZZ_EQ(e->key, key)) {
                table->ctrl[slot] = 0x80;
                SWIZZ_FREE_KEY(e->key);
                e->key = NULL;
                e->hash = 0;
                table->count--;
                return true;
            }
        }
        slot = (slot + 1) & (cap - 1);
    }
    return false;
#endif
}

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
#undef SWIZZ_CONCAT
#undef SWIZZ_TABLE_T
#undef SWIZZ_ENTRY_T
#undef SWIZZ_INIT
#undef SWIZZ_FREE_FN
#undef SWIZZ_FIND
#undef SWIZZ_ADD
#undef SWIZZ_DELETE
#undef SWIZZ_REBUILD
