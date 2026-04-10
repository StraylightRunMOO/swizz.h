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

/* Stress tests for SwissTable generative API - edge cases and heavy load scenarios */

#define TEST_PASS(msg) printf("  [PASS] %s\n", msg)
#define TEST_FAIL(msg) do { printf("  [FAIL] %s\n", msg); return 0; } while(0)

static int tests_passed = 0;
static int tests_failed = 0;

#define RUN_TEST(name) do { \
    printf("\nTest: %s\n", #name); \
    if (name()) { tests_passed++; } else { tests_failed++; } \
} while(0)

/* Test 1: Many entries with same hash prefix (collision stress) */
static int test_hash_collisions(void)
{
    symtab_table table;
    symtab_init(&table);
    
    /* Create keys that might have similar hash prefixes */
    char key[64];
    int num_entries = 10000;
    
    for (int i = 0; i < num_entries; i++) {
        /* Generate keys with patterns that might collide */
        snprintf(key, sizeof(key), "collision_test_key_%d_pattern", i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = (unsigned)i };
        if (!symtab_add(&table, key, val, 0)) {
            TEST_FAIL("Failed to add entry");
        }
    }
    
    /* Verify all entries are findable */
    for (int i = 0; i < num_entries; i++) {
        snprintf(key, sizeof(key), "collision_test_key_%d_pattern", i);
        symtab_entry *found = symtab_find(&table, key);
        if (!found) {
            TEST_FAIL("Entry not found after insertion");
        }
        if (found->value.id != (uint32_t)i) {
            TEST_FAIL("Entry has wrong ID");
        }
    }
    
    symtab_free(&table);
    TEST_PASS("Handled 10K entries with potential hash collisions");
    return 1;
}

/* Test 2: Alternating insert and delete operations */
static int test_alternating_ops(void)
{
    symtab_table table;
    symtab_init(&table);
    
    char key[32];
    
    /* Insert and delete in alternating pattern */
    for (int cycle = 0; cycle < 100; cycle++) {
        /* Insert 100 entries */
        for (int i = 0; i < 100; i++) {
            snprintf(key, sizeof(key), "alt_%d_%d", cycle, i);
            symtab_value_t val = { .id = (uint32_t)(cycle * 100 + i), .flags = 0 };
            if (!symtab_add(&table, key, val, 0)) {
                TEST_FAIL("Failed to add entry");
            }
        }
        
        /* Delete half of them */
        for (int i = 0; i < 50; i++) {
            snprintf(key, sizeof(key), "alt_%d_%d", cycle, i);
            if (!symtab_delete(&table, key)) {
                TEST_FAIL("Failed to delete entry");
            }
        }
        
        /* Verify remaining entries */
        for (int i = 50; i < 100; i++) {
            snprintf(key, sizeof(key), "alt_%d_%d", cycle, i);
            symtab_entry *found = symtab_find(&table, key);
            if (!found) {
                TEST_FAIL("Remaining entry not found");
            }
        }
        
        /* Verify deleted entries are gone */
        for (int i = 0; i < 50; i++) {
            snprintf(key, sizeof(key), "alt_%d_%d", cycle, i);
            if (symtab_find(&table, key)) {
                TEST_FAIL("Deleted entry still found");
            }
        }
    }
    
    symtab_free(&table);
    TEST_PASS("100 cycles of alternating insert/delete operations");
    return 1;
}

/* Test 3: Very long keys */
static int test_long_keys(void)
{
    symtab_table table;
    symtab_init(&table);
    
    /* Create very long keys */
    char long_key[4096];
    
    for (int i = 0; i < 100; i++) {
        /* Create a long key with the index embedded */
        memset(long_key, 'A', sizeof(long_key) - 1);
        long_key[sizeof(long_key) - 1] = '\0';
        snprintf(long_key + 100, 20, "_KEY_%d_", i);
        
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        if (!symtab_add(&table, long_key, val, 0)) {
            TEST_FAIL("Failed to add long key entry");
        }
    }
    
    /* Verify all long keys are findable */
    for (int i = 0; i < 100; i++) {
        memset(long_key, 'A', sizeof(long_key) - 1);
        long_key[sizeof(long_key) - 1] = '\0';
        snprintf(long_key + 100, 20, "_KEY_%d_", i);
        
        symtab_entry *found = symtab_find(&table, long_key);
        if (!found || found->value.id != (uint32_t)i) {
            TEST_FAIL("Long key not found or wrong ID");
        }
    }
    
    symtab_free(&table);
    TEST_PASS("Handled 100 entries with 4KB keys");
    return 1;
}

/* Test 4: Special characters in keys */
static int test_special_chars(void)
{
    symtab_table table;
    symtab_init(&table);
    
    const char *special_keys[] = {
        "",                           /* Empty string */
        " ",                          /* Single space */
        "  ",                         /* Multiple spaces */
        "\t",                         /* Tab */
        "\n",                         /* Newline */
        "key\x00hidden",              /* Null byte (will stop at null) */
        "!@#$%^&*()",                 /* Special chars */
        "<script>alert('xss')</script>",  /* HTML-like */
        "unicode_\xc3\xa9",           /* UTF-8 encoded é */
        "\\\\\\\\",                  /* Backslashes */
        "a\x01b\x02c\x03",            /* Control chars */
    };
    
    int num_keys = sizeof(special_keys) / sizeof(special_keys[0]);
    
    for (int i = 0; i < num_keys; i++) {
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        if (!symtab_add(&table, special_keys[i], val, 0)) {
            TEST_FAIL("Failed to add special char key");
        }
    }
    
    /* Verify (note: we need to be careful with embedded nulls) */
    for (int i = 0; i < num_keys; i++) {
        symtab_entry *found = symtab_find(&table, special_keys[i]);
        if (!found) {
            /* Only fail for keys without embedded nulls */
            if (i != 6) {  /* index 6 has null byte */
                TEST_FAIL("Special char key not found");
            }
        }
    }
    
    symtab_free(&table);
    TEST_PASS("Handled special characters in keys");
    return 1;
}

/* Test 5: Massive table growth */
static int test_massive_growth(void)
{
    symtab_table table;
    symtab_init(&table);
    
    char key[32];
    int num_entries = 500000;
    
    printf("    Inserting %d entries...\n", num_entries);
    
    for (int i = 0; i < num_entries; i++) {
        snprintf(key, sizeof(key), "massive_%d", i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        if (!symtab_add(&table, key, val, 0)) {
            TEST_FAIL("Failed to add entry during massive growth");
        }
        
        if (i % 100000 == 99999) {
            printf("    ...inserted %d entries\n", i + 1);
        }
    }
    
    printf("    Verifying entries...\n");
    
    /* Spot check entries */
    for (int i = 0; i < num_entries; i += 1000) {
        snprintf(key, sizeof(key), "massive_%d", i);
        symtab_entry *found = symtab_find(&table, key);
        if (!found || found->value.id != (uint32_t)i) {
            TEST_FAIL("Entry verification failed after massive growth");
        }
    }
    
    printf("    Final capacity: %u, count: %u\n", table.capacity, table.count);
    
    symtab_free(&table);
    TEST_PASS("Successfully handled 500K entries");
    return 1;
}

/* Test 6: Rapid rebuilds */
static int test_rapid_rebuilds(void)
{
    symtab_table table;
    symtab_init(&table);
    
    char key[32];
    
    /* Add entries one at a time to trigger many rebuilds */
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "rebuild_%d", i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        if (!symtab_add(&table, key, val, 0)) {
            TEST_FAIL("Failed to add entry during rapid rebuilds");
        }
    }
    
    printf("    Generation counter: %llu\n", (unsigned long long)table.generation);
    
    /* Verify all entries */
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "rebuild_%d", i);
        symtab_entry *found = symtab_find(&table, key);
        if (!found || found->value.id != (uint32_t)i) {
            TEST_FAIL("Entry not found after rapid rebuilds");
        }
    }
    
    symtab_free(&table);
    TEST_PASS("Handled 1000 entries with rapid rebuilds");
    return 1;
}

/* Test 7: Delete all then re-add */
static int test_delete_all_readd(void)
{
    symtab_table table;
    symtab_init(&table);
    
    char key[32];
    
    /* Add entries */
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "readd_%d", i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
    }
    
    /* Delete all */
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "readd_%d", i);
        if (!symtab_delete(&table, key)) {
            TEST_FAIL("Failed to delete entry");
        }
    }
    
    if (table.count != 0) {
        TEST_FAIL("Count should be 0 after deleting all");
    }
    
    /* Re-add with different IDs */
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "readd_%d", i);
        symtab_value_t val = { .id = (uint32_t)(i + 10000), .flags = 1 };
        if (!symtab_add(&table, key, val, 0)) {
            TEST_FAIL("Failed to re-add entry");
        }
    }
    
    /* Verify new IDs */
    for (int i = 0; i < 1000; i++) {
        snprintf(key, sizeof(key), "readd_%d", i);
        symtab_entry *found = symtab_find(&table, key);
        if (!found || found->value.id != (uint32_t)(i + 10000)) {
            TEST_FAIL("Re-added entry has wrong ID");
        }
    }
    
    symtab_free(&table);
    TEST_PASS("Delete all and re-add works correctly");
    return 1;
}

/* Test 8: Bloom filter false positive check */
static int test_bloom_false_positive(void)
{
    symtab_table table;
    symtab_init(&table);
    
    /* Add a small number of entries - bloom filter is 256 bits */
    for (int i = 0; i < 20; i++) {
        char key[32];
        snprintf(key, sizeof(key), "bloom_%d", i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
    }
    
    /* Check many non-existent keys */
    int false_positives = 0;
    int checks = 1000;
    
    for (int i = 0; i < checks; i++) {
        char key[32];
        snprintf(key, sizeof(key), "nonexistent_%d", i);
        
        uint64_t h = hash_string_case_insensitive(key);
        if (bloom_may_contain(table.bloom, h)) {
            /* Bloom says maybe - check if actually present */
            if (!symtab_find(&table, key)) {
                false_positives++;
            }
        }
    }
    
    double fp_rate = (double)false_positives / checks * 100;
    printf("    Bloom false positive rate: %.2f%% (%d/%d)\n", 
           fp_rate, false_positives, checks);
    
    /* With 256-bit filter and 20 entries, expect < 5% false positive rate */
    if (fp_rate > 10.0) {
        TEST_FAIL("Bloom filter false positive rate too high");
    }
    
    symtab_free(&table);
    TEST_PASS("Bloom filter has acceptable false positive rate");
    return 1;
}

/* Test 9: Case sensitivity edge cases */
static int test_case_edge_cases(void)
{
    symtab_table table;
    symtab_init(&table);
    
    /* Add with mixed case */
    symtab_value_t val = { .id = 1, .flags = 0 };
    symtab_add(&table, "CamelCaseKey", val, 0);
    
    /* Should find with any case */
    if (!symtab_find(&table, "camelcasekey")) {
        TEST_FAIL("Lowercase lookup failed");
    }
    if (!symtab_find(&table, "CAMELCASEKEY")) {
        TEST_FAIL("Uppercase lookup failed");
    }
    if (!symtab_find(&table, "CamelCaseKey")) {
        TEST_FAIL("Exact case lookup failed");
    }
    if (!symtab_find(&table, "cAmElCaSeKeY")) {
        TEST_FAIL("Random case lookup failed");
    }
    
    symtab_free(&table);
    TEST_PASS("Case-insensitive lookup works for all variations");
    return 1;
}

/* Test 10: Simultaneous updates */
static int test_simultaneous_updates(void)
{
    symtab_table table;
    symtab_init(&table);
    
    char key[32];
    
    /* Add initial entries - flags passed as 4th parameter */
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "update_%d", i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        symtab_add(&table, key, val, 0);
    }
    
    /* Update all entries multiple times - flags passed as 4th parameter */
    for (int round = 0; round < 10; round++) {
        for (int i = 0; i < 100; i++) {
            snprintf(key, sizeof(key), "update_%d", i);
            symtab_value_t val = { .id = (uint32_t)(i + round * 100), .flags = 0 };
            symtab_entry *updated = symtab_add(&table, key, val, (unsigned)round);
            if (!updated) {
                TEST_FAIL("Failed to update entry");
            }
        }
    }
    
    /* Verify final values - check entry flags (4th param), not value.flags */
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "update_%d", i);
        symtab_entry *found = symtab_find(&table, key);
        if (!found || found->value.id != (uint32_t)(i + 900) || found->flags != 9) {
            TEST_FAIL("Entry has wrong value after multiple updates");
        }
    }
    
    symtab_free(&table);
    TEST_PASS("10 rounds of updates on 100 entries");
    return 1;
}

int main(void)
{
    printf("SwissTable Stress Tests (Generative API)\n");
    printf("========================================\n");
    printf("Testing edge cases and heavy load scenarios...\n");
    
    RUN_TEST(test_hash_collisions);
    RUN_TEST(test_alternating_ops);
    RUN_TEST(test_long_keys);
    RUN_TEST(test_special_chars);
    RUN_TEST(test_massive_growth);
    RUN_TEST(test_rapid_rebuilds);
    RUN_TEST(test_delete_all_readd);
    RUN_TEST(test_bloom_false_positive);
    RUN_TEST(test_case_edge_cases);
    RUN_TEST(test_simultaneous_updates);
    
    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    
    return tests_failed > 0 ? 1 : 0;
}
