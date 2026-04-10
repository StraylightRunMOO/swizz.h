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
        SWISS_ALLOC_FREE(table->ctrl);
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

    SWISS_ENTRY_T *new_entries = SWISS_ALLOC_CALLOC(cap, sizeof(SWISS_ENTRY_T));
    uint8_t *new_ctrl = SWISS_ALLOC_MALLOC(cap);
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
    SWISS_ALLOC_FREE(table->ctrl);
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

    while (table->ctrl[slot] != 0x00) {
        if (table->ctrl[slot] == fp) {
            SWISS_ENTRY_T *e = &table->entries[slot];
            if (SWISS_EQ(e->key, key)) return e;
        }
        slot = (slot + 1) & (cap - 1);
    }
    return NULL;
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
        table->capacity = 4;
        table->entries = SWISS_ALLOC_CALLOC(table->capacity, sizeof(SWISS_ENTRY_T));
        table->ctrl = SWISS_ALLOC_MALLOC(table->capacity);
        if (!table->entries || !table->ctrl) {
            SWISS_FREE_KEY(key_copy);
            return NULL;
        }
        memset(table->ctrl, 0x00, table->capacity);
        memset(table->bloom, 0, sizeof(table->bloom));
        table->generation = 1;
    }
    /* grow if load > 0.5 */
    else if (table->count >= table->capacity / 2) {
        uint32_t new_cap = table->capacity * 2;
        SWISS_ENTRY_T *new_entries = SWISS_ALLOC_CALLOC(new_cap, sizeof(SWISS_ENTRY_T));
        uint8_t *new_ctrl = SWISS_ALLOC_MALLOC(new_cap);
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
        SWISS_ALLOC_FREE(table->ctrl);
        table->entries = new_entries;
        table->ctrl = new_ctrl;
        table->capacity = new_cap;
        table->generation++;
    }

    /* insert */
    uint32_t slot = (uint32_t)(h & (table->capacity - 1));
    while (table->ctrl[slot] != 0x00 && table->ctrl[slot] != 0x80) {
        slot = (slot + 1) & (table->capacity - 1);
    }

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
