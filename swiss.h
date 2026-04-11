/* swiss.h — SwissTable bgen-style generator
 * C99 header-only, zero dependencies, -pedantic -Werror clean.
 * Drop this in your project and #include it after defining the macros above.
 * Multiple independent tables by re-#defining and re-including.
 */

#ifndef SWISS_H_INTERNAL_GUARD
#define SWISS_H_INTERNAL_GUARD

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
#  define SWISS_GROUP_WIDTH 32
#  define SWISS_USE_SIMD 1
   typedef __m256i swiss_group_t;
#  define swiss_group_load(p)     _mm256_loadu_si256((const __m256i*)(p))
#  define swiss_group_bcast(fp)   _mm256_set1_epi8((char)(fp))
#  define swiss_group_cmpeq(g,f)  _mm256_cmpeq_epi8((g),(f))
#  define swiss_group_mask(m)     _mm256_movemask_epi8(m)
#  define swiss_group_or(a,b)     _mm256_or_si256((a),(b))
#elif defined(__SSE2__)
#  include <emmintrin.h>
#  define SWISS_GROUP_WIDTH 16
#  define SWISS_USE_SIMD 1
   typedef __m128i swiss_group_t;
#  define swiss_group_load(p)     _mm_loadu_si128((const __m128i*)(p))
#  define swiss_group_bcast(fp)   _mm_set1_epi8((char)(fp))
#  define swiss_group_cmpeq(g,f)  _mm_cmpeq_epi8((g),(f))
#  define swiss_group_mask(m)     _mm_movemask_epi8(m)
#  define swiss_group_or(a,b)     _mm_or_si128((a),(b))
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#  include <arm_neon.h>
#  define SWISS_GROUP_WIDTH 16
#  define SWISS_USE_SIMD 1
   typedef uint8x16_t swiss_group_t;
#  define swiss_group_load(p)     vld1q_u8((const uint8_t*)(p))
#  define swiss_group_bcast(fp)   vdupq_n_u8(fp)
#  define swiss_group_cmpeq(g,f)  vceqq_u8((g),(f))
#  define swiss_group_or(a,b)     vorrq_u8((a),(b))
   /* NEON movemask: extract comparison results to 16-bit mask */
   static inline int swiss_neon_movemask(uint8x16_t v) {
       /* v has 0xFF for equal bytes - extract bit 7 from each byte */
       uint8_t bytes[16];
       vst1q_u8(bytes, v);
       int mask = 0;
       for (int i = 0; i < 16; i++) {
           mask |= ((bytes[i] >> 7) & 1) << i;
       }
       return mask;
   }
#  define swiss_group_mask(m)     swiss_neon_movemask(m)
#else
#  define SWISS_GROUP_WIDTH 1
#  define SWISS_USE_SIMD 0
   typedef uint8_t swiss_group_t;
#endif

/* Load factor: 7/8 for SIMD (better cache utilization), 1/2 for scalar */
#if SWISS_USE_SIMD
#  define SWISS_LOAD_FACTOR(cap)  (((cap) * 7) >> 3)
#else
#  define SWISS_LOAD_FACTOR(cap)  ((cap) >> 1)
#endif

/* ------------------------------------------------------------------
 * SIMD-accelerated group probing helpers
 * ------------------------------------------------------------------ */

/* Probe for matching fingerprint - returns bitmask of matches */
static inline int swiss_probe_group(const uint8_t *ctrl, uint8_t fp)
{
#if SWISS_USE_SIMD
    swiss_group_t group = swiss_group_load((const swiss_group_t*)ctrl);
    swiss_group_t fingerprint = swiss_group_bcast(fp);
    swiss_group_t cmp = swiss_group_cmpeq(group, fingerprint);
    return swiss_group_mask(cmp);
#else
    return (*ctrl == fp) ? 1 : 0;
#endif
}

/* Probe for empty slots - returns bitmask of empty (0x00) slots */
static inline int swiss_probe_empty(const uint8_t *ctrl)
{
#if SWISS_USE_SIMD
    swiss_group_t group = swiss_group_load((const swiss_group_t*)ctrl);
    swiss_group_t empty = swiss_group_bcast(0x00);
    swiss_group_t cmp = swiss_group_cmpeq(group, empty);
    return swiss_group_mask(cmp);
#else
    return (*ctrl == 0x00) ? 1 : 0;
#endif
}

/* Probe for empty or deleted slots - returns bitmask of available slots */
static inline int swiss_probe_available(const uint8_t *ctrl)
{
#if SWISS_USE_SIMD
    swiss_group_t group = swiss_group_load((const swiss_group_t*)ctrl);
    swiss_group_t empty = swiss_group_bcast(0x00);
    swiss_group_t deleted = swiss_group_bcast(0x80);
    swiss_group_t cmp_empty = swiss_group_cmpeq(group, empty);
    swiss_group_t cmp_deleted = swiss_group_cmpeq(group, deleted);
    swiss_group_t cmp = swiss_group_or(cmp_empty, cmp_deleted);
    return swiss_group_mask(cmp);
#else
    return (*ctrl == 0x00 || *ctrl == 0x80) ? 1 : 0;
#endif
}

#endif /* SWISS_H_INTERNAL_GUARD */

/* ------------------------------------------------------------------
 * Required user macros (must be defined before #include)
 * ------------------------------------------------------------------ */
#if !defined(SWISS_NAME) || !defined(SWISS_KEY_TYPE) || !defined(SWISS_VALUE_TYPE) || \
    !defined(SWISS_HASH) || !defined(SWISS_EQ) || !defined(SWISS_DUP_KEY) || !defined(SWISS_FREE_KEY)
#error "You must #define SWISS_NAME, SWISS_KEY_TYPE, SWISS_VALUE_TYPE, SWISS_HASH(k), SWISS_EQ(k1,k2), SWISS_DUP_KEY(k), SWISS_FREE_KEY(k) before including swiss.h"
#endif

/* Optional allocator overrides (defaults to stdlib) - use unique names */
#ifndef SWISS_ALLOC_MALLOC
#define SWISS_ALLOC_MALLOC(sz) malloc(sz)
#endif
#ifndef SWISS_ALLOC_CALLOC
#define SWISS_ALLOC_CALLOC(nmemb, size) calloc(nmemb, size)
#endif
#ifndef SWISS_ALLOC_FREE
#define SWISS_ALLOC_FREE(p) free(p)
#endif

/* ------------------------------------------------------------------
 * Aligned allocation for SIMD control bytes
 * ------------------------------------------------------------------ */
#if SWISS_USE_SIMD
/* Aligned allocation: stores original pointer at start for later free */
static inline uint8_t* swiss_alloc_ctrl_aligned(size_t cap)
{
    size_t align = SWISS_GROUP_WIDTH;
    /* Allocate extra space for alignment padding and original pointer storage */
    void *raw = SWISS_ALLOC_MALLOC(cap + align + sizeof(void*));
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
static inline void swiss_free_ctrl_aligned(uint8_t *ctrl)
{
    if (!ctrl) return;
    void **storage = (void**)((uintptr_t)ctrl - sizeof(void*));
    SWISS_ALLOC_FREE(*storage);
}
#endif

/* ------------------------------------------------------------------
 * Token pasting helpers (two-stage to force macro expansion)
 * ------------------------------------------------------------------ */
#define SWISS_CONCAT_(x, y) x##y
#define SWISS_CONCAT(x, y) SWISS_CONCAT_(x, y)
#define SWISS_TABLE_T  SWISS_CONCAT(SWISS_NAME, _table)
#define SWISS_ENTRY_T  SWISS_CONCAT(SWISS_NAME, _entry)
#define SWISS_INIT     SWISS_CONCAT(SWISS_NAME, _init)
#define SWISS_FREE_FN  SWISS_CONCAT(SWISS_NAME, _free)
#define SWISS_FIND     SWISS_CONCAT(SWISS_NAME, _find)
#define SWISS_ADD      SWISS_CONCAT(SWISS_NAME, _add)
#define SWISS_DELETE   SWISS_CONCAT(SWISS_NAME, _delete)
#define SWISS_REBUILD  SWISS_CONCAT(SWISS_NAME, _rebuild_cache)

/* ------------------------------------------------------------------
 * Internal entry (key is owned by table)
 * ------------------------------------------------------------------ */
typedef struct SWISS_ENTRY_T {
    SWISS_KEY_TYPE   key;      /* owned */
    uint64_t         hash;
    SWISS_VALUE_TYPE value;
    unsigned         flags;
} SWISS_ENTRY_T;

/* ------------------------------------------------------------------
 * The table (public API)
 * ------------------------------------------------------------------ */
typedef struct SWISS_TABLE_T {
    SWISS_ENTRY_T *entries;
    uint8_t       *ctrl;
    uint64_t       bloom[4];
    uint32_t       capacity;
    uint32_t       count;
    uint64_t       generation;
} SWISS_TABLE_T;

/* ------------------------------------------------------------------
 * Lifecycle
 * ------------------------------------------------------------------ */
static inline void SWISS_INIT(SWISS_TABLE_T *table)
{
    memset(table, 0, sizeof(*table));
}

static inline void SWISS_FREE_FN(SWISS_TABLE_T *table)
{
    if (table->entries) {
        for (uint32_t i = 0; i < table->capacity; i++) {
            if (table->ctrl[i] != 0x00 && table->ctrl[i] != 0x80 && table->entries[i].key) {
                SWISS_FREE_KEY(table->entries[i].key);
            }
        }
        SWISS_ALLOC_FREE(table->entries);
#if SWISS_USE_SIMD
        swiss_free_ctrl_aligned(table->ctrl);
#else
        SWISS_ALLOC_FREE(table->ctrl);
#endif
    }
    memset(table, 0, sizeof(*table));
}

/* ------------------------------------------------------------------
 * Internal rebuild (compaction after heavy deletes)
 * ------------------------------------------------------------------ */
static inline void SWISS_REBUILD(SWISS_TABLE_T *table)
{
    if (!table->entries || table->count == 0) return;

    uint32_t valid_count = 0;
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->ctrl[i] != 0x00 && table->ctrl[i] != 0x80 && table->entries[i].key)
            valid_count++;
    }
    if (valid_count == 0) {
        SWISS_FREE_FN(table);
        return;
    }

    uint32_t cap = 4;
    while (cap < valid_count * 2) cap <<= 1;
#if SWISS_USE_SIMD
    if (cap < SWISS_GROUP_WIDTH) cap = SWISS_GROUP_WIDTH;
#endif

    SWISS_ENTRY_T *new_entries = SWISS_ALLOC_CALLOC(cap, sizeof(SWISS_ENTRY_T));
#if SWISS_USE_SIMD
    uint8_t *new_ctrl = swiss_alloc_ctrl_aligned(cap);
#else
    uint8_t *new_ctrl = SWISS_ALLOC_MALLOC(cap);
#endif
    if (!new_entries || !new_ctrl) return;  /* OOM — leave old table intact */

    memset(new_ctrl, 0x00, cap);
    memset(table->bloom, 0, sizeof(table->bloom));

    uint32_t new_count = 0;
    for (uint32_t i = 0; i < table->capacity; i++) {
        if (table->ctrl[i] == 0x00 || table->ctrl[i] == 0x80 || !table->entries[i].key) continue;

        SWISS_ENTRY_T *p = &table->entries[i];
        uint64_t h = p->hash;
        uint32_t slot = (uint32_t)(h & (cap - 1));

        while (new_ctrl[slot] != 0x00) {
            if (new_ctrl[slot] != 0x80 && SWISS_EQ(new_entries[slot].key, p->key)) {
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

    SWISS_ALLOC_FREE(table->entries);
#if SWISS_USE_SIMD
    swiss_free_ctrl_aligned(table->ctrl);
#else
    SWISS_ALLOC_FREE(table->ctrl);
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
static inline SWISS_ENTRY_T* SWISS_FIND(SWISS_TABLE_T *table, SWISS_KEY_TYPE key)
{
    if (!table->entries || table->count == 0) return NULL;

    uint64_t h = SWISS_HASH(key);
    if (!bloom_may_contain(table->bloom, h)) return NULL;

    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    uint8_t fp = entry_fingerprint(h);

#if SWISS_USE_SIMD
    /* SIMD-accelerated group probing */
    uint32_t start_slot = slot;
    while (1) {
        uint32_t group_start = slot & ~(SWISS_GROUP_WIDTH - 1);
        int mask = swiss_probe_group(&table->ctrl[group_start], fp);
        
        /* Check each matching slot in the group */
        while (mask != 0) {
            int bit = __builtin_ctz(mask);
            uint32_t candidate = group_start + bit;
            if (candidate >= cap) candidate -= cap; /* wrap around */
            
            if (table->ctrl[candidate] == fp) {
                SWISS_ENTRY_T *e = &table->entries[candidate];
                if (SWISS_EQ(e->key, key)) return e;
            }
            mask &= mask - 1; /* clear lowest bit */
        }
        
        /* Check for empty slot at or after the starting position within the group.
         * We only stop if we find an empty slot in the probe sequence.
         * For the first group, check from starting offset. For subsequent groups, check all. */
        int empty_mask = swiss_probe_empty(&table->ctrl[group_start]);
        if (slot == start_slot) {
            /* First group: check empty slots at or after start position */
            int offset_in_group = slot - group_start;
            empty_mask &= ~((1 << offset_in_group) - 1);
        }
        if (empty_mask != 0) return NULL;
        
        /* Move to next group */
        slot = (group_start + SWISS_GROUP_WIDTH) & (cap - 1);
        if (slot == start_slot) return NULL; /* full circle */
    }
#else
    /* Scalar fallback */
    while (table->ctrl[slot] != 0x00) {
        if (table->ctrl[slot] == fp) {
            SWISS_ENTRY_T *e = &table->entries[slot];
            if (SWISS_EQ(e->key, key)) return e;
        }
        slot = (slot + 1) & (cap - 1);
    }
    return NULL;
#endif
}

static inline SWISS_ENTRY_T* SWISS_ADD(SWISS_TABLE_T *table, SWISS_KEY_TYPE key, SWISS_VALUE_TYPE value, unsigned flags)
{
    SWISS_KEY_TYPE key_copy = SWISS_DUP_KEY(key);
    if (!key_copy && key) return NULL;

    uint64_t h = SWISS_HASH(key);  /* hash before possible dup failure */

    /* check for existing */
    SWISS_ENTRY_T *existing = SWISS_FIND(table, key);
    if (existing) {
        SWISS_FREE_KEY(key_copy);
        existing->value = value;
        existing->flags = flags;
        return existing;
    }

    /* first insert — allocate initial table */
    if (!table->entries) {
#if SWISS_USE_SIMD
        /* Minimum capacity must be at least group width for SIMD loads */
        table->capacity = (4 > SWISS_GROUP_WIDTH) ? 4 : SWISS_GROUP_WIDTH;
#else
        table->capacity = 4;
#endif
        table->entries = SWISS_ALLOC_CALLOC(table->capacity, sizeof(SWISS_ENTRY_T));
#if SWISS_USE_SIMD
        table->ctrl = swiss_alloc_ctrl_aligned(table->capacity);
#else
        table->ctrl = SWISS_ALLOC_MALLOC(table->capacity);
#endif
        if (!table->entries || !table->ctrl) {
            SWISS_FREE_KEY(key_copy);
            return NULL;
        }
        memset(table->ctrl, 0x00, table->capacity);
        memset(table->bloom, 0, sizeof(table->bloom));
        table->generation = 1;
    }
    /* grow if load factor exceeded */
    else if (table->count >= SWISS_LOAD_FACTOR(table->capacity)) {
        uint32_t new_cap = table->capacity * 2;
#if SWISS_USE_SIMD
        /* Ensure new capacity is at least group width */
        if (new_cap < SWISS_GROUP_WIDTH) new_cap = SWISS_GROUP_WIDTH;
#endif
        SWISS_ENTRY_T *new_entries = SWISS_ALLOC_CALLOC(new_cap, sizeof(SWISS_ENTRY_T));
#if SWISS_USE_SIMD
        uint8_t *new_ctrl = swiss_alloc_ctrl_aligned(new_cap);
#else
        uint8_t *new_ctrl = SWISS_ALLOC_MALLOC(new_cap);
#endif
        if (!new_entries || !new_ctrl) {
            SWISS_FREE_KEY(key_copy);
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

        SWISS_ALLOC_FREE(table->entries);
#if SWISS_USE_SIMD
        swiss_free_ctrl_aligned(table->ctrl);
#else
        SWISS_ALLOC_FREE(table->ctrl);
#endif
        table->entries = new_entries;
        table->ctrl = new_ctrl;
        table->capacity = new_cap;
        table->generation++;
    }

    /* insert - find empty or deleted slot */
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    
#if SWISS_USE_SIMD
    /* SIMD-accelerated search for available slot */
    uint32_t start_slot = slot;
    int first_group = 1;
    while (1) {
        uint32_t group_start = slot & ~(SWISS_GROUP_WIDTH - 1);
        int mask = swiss_probe_available(&table->ctrl[group_start]);
        
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
        slot = (group_start + SWISS_GROUP_WIDTH) & (cap - 1);
        if (slot == start_slot) return NULL; /* table full (shouldn't happen) */
    }
#else
    /* Scalar fallback */
    while (table->ctrl[slot] != 0x00 && table->ctrl[slot] != 0x80) {
        slot = (slot + 1) & (cap - 1);
    }
#endif

    SWISS_ENTRY_T e = {0};
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

static inline bool SWISS_DELETE(SWISS_TABLE_T *table, SWISS_KEY_TYPE key)
{
    if (!table->entries || table->count == 0) return false;

    uint64_t h = SWISS_HASH(key);
    uint32_t cap = table->capacity;
    uint32_t slot = (uint32_t)(h & (cap - 1));
    uint8_t fp = entry_fingerprint(h);

#if SWISS_USE_SIMD
    /* SIMD-accelerated group probing */
    uint32_t start_slot = slot;
    while (1) {
        uint32_t group_start = slot & ~(SWISS_GROUP_WIDTH - 1);
        int mask = swiss_probe_group(&table->ctrl[group_start], fp);
        
        /* Check each matching slot in the group */
        while (mask != 0) {
            int bit = __builtin_ctz(mask);
            uint32_t candidate = group_start + bit;
            if (candidate >= cap) candidate -= cap; /* wrap around */
            
            if (table->ctrl[candidate] == fp) {
                SWISS_ENTRY_T *e = &table->entries[candidate];
                if (SWISS_EQ(e->key, key)) {
                    table->ctrl[candidate] = 0x80;
                    SWISS_FREE_KEY(e->key);
                    e->key = NULL;
                    e->hash = 0;
                    table->count--;
                    return true;
                }
            }
            mask &= mask - 1; /* clear lowest bit */
        }
        
        /* Check for empty slot at or after the starting position within the group */
        int empty_mask = swiss_probe_empty(&table->ctrl[group_start]);
        if (slot == start_slot) {
            /* First group: check empty slots at or after start position */
            int offset_in_group = slot - group_start;
            empty_mask &= ~((1 << offset_in_group) - 1);
        }
        if (empty_mask != 0) return false;
        
        /* Move to next group */
        slot = (group_start + SWISS_GROUP_WIDTH) & (cap - 1);
        if (slot == start_slot) return false; /* full circle */
    }
#else
    /* Scalar fallback */
    while (table->ctrl[slot] != 0x00) {
        if (table->ctrl[slot] == fp) {
            SWISS_ENTRY_T *e = &table->entries[slot];
            if (SWISS_EQ(e->key, key)) {
                table->ctrl[slot] = 0x80;
                SWISS_FREE_KEY(e->key);
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
#undef SWISS_NAME
#undef SWISS_KEY_TYPE
#undef SWISS_VALUE_TYPE
#undef SWISS_HASH
#undef SWISS_EQ
#undef SWISS_DUP_KEY
#undef SWISS_FREE_KEY
#undef SWISS_ALLOC_MALLOC
#undef SWISS_ALLOC_CALLOC
#undef SWISS_ALLOC_FREE
#undef SWISS_CONCAT
#undef SWISS_TABLE_T
#undef SWISS_ENTRY_T
#undef SWISS_INIT
#undef SWISS_FREE_FN
#undef SWISS_FIND
#undef SWISS_ADD
#undef SWISS_DELETE
#undef SWISS_REBUILD
