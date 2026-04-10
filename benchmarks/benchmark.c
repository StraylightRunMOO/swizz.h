#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Helper for string duplication */
static inline const char* str_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

/* Define value type first */
typedef struct { uint32_t id; unsigned flags; } symtab_value_t;

/* Define the generative table configuration */
#define SWISS_NAME symtab
#define SWISS_KEY_TYPE const char*
#define SWISS_VALUE_TYPE symtab_value_t
#define SWISS_HASH(k) hash_string_case_insensitive(k)
#define SWISS_EQ(k1,k2) (!strcasecmp((k1),(k2)))
#define SWISS_DUP_KEY(k) str_dup(k)
#define SWISS_FREE_KEY(k) free((void*)(k))
#include "../swiss.h"

/* Simple benchmark suite for SwissTable generative API */

#define NUM_KEYS 100000
#define WARMUP_ITERATIONS 3
#define BENCHMARK_ITERATIONS 5

static double get_time(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void generate_key(char *buf, int index)
{
    snprintf(buf, 32, "key_%08d", index);
}

/* Benchmark: Insert operations */
static double benchmark_insert(int num_keys)
{
    symtab_table table;
    symtab_init(&table);
    
    char key[32];
    double start = get_time();
    
    for (int i = 0; i < num_keys; i++) {
        generate_key(key, i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
    }
    
    double elapsed = get_time() - start;
    symtab_free(&table);
    return elapsed;
}

/* Benchmark: Lookup operations (existing keys) */
static double benchmark_lookup_hit(int num_keys)
{
    symtab_table table;
    symtab_init(&table);
    
    /* Populate table */
    char key[32];
    for (int i = 0; i < num_keys; i++) {
        generate_key(key, i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
    }
    
    /* Benchmark lookups */
    volatile int found_count = 0;
    double start = get_time();
    
    for (int iter = 0; iter < 10; iter++) {
        for (int i = 0; i < num_keys; i++) {
            generate_key(key, i);
            if (symtab_find(&table, key)) {
                found_count++;
            }
        }
    }
    
    double elapsed = get_time() - start;
    (void)found_count;  /* Suppress unused warning */
    
    symtab_free(&table);
    return elapsed;
}

/* Benchmark: Lookup operations (non-existent keys) */
static double benchmark_lookup_miss(int num_keys)
{
    symtab_table table;
    symtab_init(&table);
    
    /* Populate table */
    char key[32];
    for (int i = 0; i < num_keys; i++) {
        generate_key(key, i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
    }
    
    /* Benchmark lookups of non-existent keys */
    volatile int found_count = 0;
    double start = get_time();
    
    for (int iter = 0; iter < 10; iter++) {
        for (int i = 0; i < num_keys; i++) {
            snprintf(key, 32, "missing_%08d", i);
            if (symtab_find(&table, key)) {
                found_count++;
            }
        }
    }
    
    double elapsed = get_time() - start;
    (void)found_count;
    
    symtab_free(&table);
    return elapsed;
}

/* Benchmark: Delete operations */
static double benchmark_delete(int num_keys)
{
    symtab_table table;
    symtab_init(&table);
    
    /* Populate table */
    char key[32];
    for (int i = 0; i < num_keys; i++) {
        generate_key(key, i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
    }
    
    /* Benchmark deletions */
    double start = get_time();
    
    for (int i = 0; i < num_keys; i++) {
        generate_key(key, i);
        symtab_delete(&table, key);
    }
    
    double elapsed = get_time() - start;
    symtab_free(&table);
    return elapsed;
}

/* Benchmark: Mixed workload */
static double benchmark_mixed(int num_keys)
{
    symtab_table table;
    symtab_init(&table);
    
    char key[32];
    volatile int dummy = 0;
    
    double start = get_time();
    
    /* 50% inserts, 30% lookups, 20% deletes */
    for (int i = 0; i < num_keys; i++) {
        /* Insert */
        generate_key(key, i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
        
        /* Lookup */
        if (i % 3 == 0 && i > 0) {
            generate_key(key, i / 2);
            symtab_entry *found = symtab_find(&table, key);
            if (found) dummy++;
        }
        
        /* Delete */
        if (i % 5 == 0 && i > 0) {
            generate_key(key, i / 5);
            symtab_delete(&table, key);
        }
    }
    
    double elapsed = get_time() - start;
    (void)dummy;
    
    symtab_free(&table);
    return elapsed;
}

int main(void)
{
    printf("SwissTable Benchmarks (Generative API)\n");
    printf("======================================\n\n");
    printf("Running with %d keys\n\n", NUM_KEYS);
    
    /* Warmup */
    printf("Warming up...\n");
    for (int i = 0; i < WARMUP_ITERATIONS; i++) {
        benchmark_insert(1000);
    }
    
    /* Benchmark: Insert */
    printf("\n1. Insert benchmark (%d keys):\n", NUM_KEYS);
    double insert_total = 0;
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        double t = benchmark_insert(NUM_KEYS);
        insert_total += t;
        printf("   Run %d: %.3f ms (%.0f ops/sec)\n", 
               i + 1, t * 1000, NUM_KEYS / t);
    }
    printf("   Average: %.3f ms (%.0f ops/sec)\n", 
           insert_total * 1000 / BENCHMARK_ITERATIONS,
           NUM_KEYS * BENCHMARK_ITERATIONS / insert_total);
    
    /* Benchmark: Lookup (hit) */
    printf("\n2. Lookup benchmark (hits, %d keys x 10 lookups):\n", NUM_KEYS);
    double lookup_hit_total = 0;
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        double t = benchmark_lookup_hit(NUM_KEYS);
        lookup_hit_total += t;
        printf("   Run %d: %.3f ms (%.0f ops/sec)\n", 
               i + 1, t * 1000, (NUM_KEYS * 10.0) / t);
    }
    printf("   Average: %.3f ms (%.0f ops/sec)\n", 
           lookup_hit_total * 1000 / BENCHMARK_ITERATIONS,
           NUM_KEYS * 10.0 * BENCHMARK_ITERATIONS / lookup_hit_total);
    
    /* Benchmark: Lookup (miss) */
    printf("\n3. Lookup benchmark (misses, %d keys x 10 lookups):\n", NUM_KEYS);
    double lookup_miss_total = 0;
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        double t = benchmark_lookup_miss(NUM_KEYS);
        lookup_miss_total += t;
        printf("   Run %d: %.3f ms (%.0f ops/sec)\n", 
               i + 1, t * 1000, (NUM_KEYS * 10.0) / t);
    }
    printf("   Average: %.3f ms (%.0f ops/sec)\n", 
           lookup_miss_total * 1000 / BENCHMARK_ITERATIONS,
           NUM_KEYS * 10.0 * BENCHMARK_ITERATIONS / lookup_miss_total);
    
    /* Benchmark: Delete */
    printf("\n4. Delete benchmark (%d keys):\n", NUM_KEYS);
    double delete_total = 0;
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        double t = benchmark_delete(NUM_KEYS);
        delete_total += t;
        printf("   Run %d: %.3f ms (%.0f ops/sec)\n", 
               i + 1, t * 1000, NUM_KEYS / t);
    }
    printf("   Average: %.3f ms (%.0f ops/sec)\n", 
           delete_total * 1000 / BENCHMARK_ITERATIONS,
           NUM_KEYS * BENCHMARK_ITERATIONS / delete_total);
    
    /* Benchmark: Mixed */
    printf("\n5. Mixed workload benchmark (%d ops):\n", NUM_KEYS);
    double mixed_total = 0;
    for (int i = 0; i < BENCHMARK_ITERATIONS; i++) {
        double t = benchmark_mixed(NUM_KEYS);
        mixed_total += t;
        printf("   Run %d: %.3f ms (%.0f ops/sec)\n", 
               i + 1, t * 1000, NUM_KEYS / t);
    }
    printf("   Average: %.3f ms (%.0f ops/sec)\n", 
           mixed_total * 1000 / BENCHMARK_ITERATIONS,
           NUM_KEYS * BENCHMARK_ITERATIONS / mixed_total);
    
    printf("\nBenchmark complete.\n");
    return 0;
}
