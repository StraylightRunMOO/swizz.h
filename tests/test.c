#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* Helper for string duplication */
static inline const char* str_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

/* Define value type first (required for generative API) */
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

/* Test counter */
static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(test) do { \
    tests_run++; \
    printf("  Running %s... ", #test); \
    if (test()) { \
        tests_passed++; \
        printf("PASSED\n"); \
    } else { \
        printf("FAILED\n"); \
    } \
} while(0)

/* Test 1: Basic initialization */
static int test_init(void)
{
    symtab_table table;
    symtab_init(&table);
    
    int pass = (table.entries == NULL && 
                table.capacity == 0 && 
                table.count == 0);
    
    symtab_free(&table);
    return pass;
}

/* Test 2: Add and find single entry */
static int test_add_find_single(void)
{
    symtab_table table;
    symtab_init(&table);
    
    symtab_value_t val = { .id = 1, .flags = 0 };
    symtab_entry *added = symtab_add(&table, "test", val, 0);
    
    int pass = (added != NULL);
    if (pass) {
        symtab_entry *found = symtab_find(&table, "test");
        pass = (found != NULL && found->value.id == 1);
    }
    
    symtab_free(&table);
    return pass;
}

/* Test 3: Add multiple entries */
static int test_add_multiple(void)
{
    symtab_table table;
    symtab_init(&table);
    
    const char *names[] = {"foo", "bar", "baz", "qux"};
    int pass = 1;
    
    for (int i = 0; i < 4; i++) {
        symtab_value_t val = { .id = (uint32_t)(i + 1), .flags = 0 };
        if (!symtab_add(&table, names[i], val, 0)) {
            pass = 0;
            break;
        }
    }
    
    if (pass) {
        for (int i = 0; i < 4; i++) {
            symtab_entry *found = symtab_find(&table, names[i]);
            if (!found || found->value.id != (uint32_t)(i + 1)) {
                pass = 0;
                break;
            }
        }
    }
    
    symtab_free(&table);
    return pass;
}

/* Test 4: Update existing entry */
static int test_update_entry(void)
{
    symtab_table table;
    symtab_init(&table);
    
    symtab_value_t val1 = { .id = 1, .flags = 0 };
    symtab_add(&table, "key", val1, 0);
    
    symtab_value_t val2 = { .id = 42, .flags = 1 };
    symtab_entry *updated = symtab_add(&table, "key", val2, 0);
    
    int pass = (updated != NULL && updated->value.id == 42);
    if (pass) {
        /* Should only have one entry */
        pass = (table.count == 1);
    }
    
    symtab_free(&table);
    return pass;
}

/* Test 5: Delete entry */
static int test_delete_entry(void)
{
    symtab_table table;
    symtab_init(&table);
    
    symtab_value_t val = { .id = 1, .flags = 0 };
    symtab_add(&table, "todelete", val, 0);
    
    int pass = symtab_delete(&table, "todelete");
    if (pass) {
        symtab_entry *found = symtab_find(&table, "todelete");
        pass = (found == NULL);
    }
    
    symtab_free(&table);
    return pass;
}

/* Test 6: Find non-existent entry */
static int test_find_nonexistent(void)
{
    symtab_table table;
    symtab_init(&table);
    
    symtab_value_t val = { .id = 1, .flags = 0 };
    symtab_add(&table, "exists", val, 0);
    
    symtab_entry *found = symtab_find(&table, "doesnotexist");
    
    symtab_free(&table);
    return (found == NULL);
}

/* Test 7: Delete non-existent entry */
static int test_delete_nonexistent(void)
{
    symtab_table table;
    symtab_init(&table);
    
    symtab_value_t val = { .id = 1, .flags = 0 };
    symtab_add(&table, "exists", val, 0);
    
    int result = symtab_delete(&table, "doesnotexist");
    
    symtab_free(&table);
    return (!result);  /* Should return false */
}

/* Test 8: Case-insensitive lookup */
static int test_case_insensitive(void)
{
    symtab_table table;
    symtab_init(&table);
    
    symtab_value_t val = { .id = 1, .flags = 0 };
    symtab_add(&table, "TestKey", val, 0);
    
    symtab_entry *found1 = symtab_find(&table, "testkey");
    symtab_entry *found2 = symtab_find(&table, "TESTKEY");
    
    int pass = (found1 != NULL && found2 != NULL && 
                found1->value.id == 1 && found2->value.id == 1);
    
    symtab_free(&table);
    return pass;
}

/* Test 9: Hash function consistency */
static int test_hash_consistency(void)
{
    uint64_t h1 = hash_string("test");
    uint64_t h2 = hash_string("test");
    uint64_t h3 = hash_string("TEST");
    
    return (h1 == h2 && h1 != h3);
}

/* Test 10: Fingerprint generation */
static int test_fingerprint(void)
{
    uint8_t fp1 = entry_fingerprint(0x0011223344556677ULL);
    uint8_t fp2 = entry_fingerprint(0xFF11223344556677ULL);
    uint8_t fp3 = entry_fingerprint(0x0011223344556677ULL);  /* Same as fp1 */
    
    /* Fingerprint should never be 0x00 or 0x80 */
    int pass = (fp1 != 0x00 && fp1 != 0x80);
    pass = pass && (fp2 != 0x00 && fp2 != 0x80);
    pass = pass && (fp1 == fp3);  /* Same hash = same fingerprint */
    
    return pass;
}

/* Test 11: Bloom filter operations */
static int test_bloom_filter(void)
{
    symtab_table table;
    symtab_init(&table);
    
    uint64_t h = hash_string("test");
    
    /* Before adding, bloom should not contain */
    int pass = !bloom_may_contain(table.bloom, h);
    
    bloom_add(table.bloom, h);
    
    /* After adding, bloom should contain */
    pass = pass && bloom_may_contain(table.bloom, h);
    
    symtab_free(&table);
    return pass;
}

/* Test 12: Empty table operations */
static int test_empty_table(void)
{
    symtab_table table;
    symtab_init(&table);
    
    int pass = (symtab_find(&table, "anything") == NULL);
    pass = pass && !symtab_delete(&table, "anything");
    pass = pass && (table.count == 0);
    
    symtab_free(&table);
    return pass;
}

/* Test 13: Table growth */
static int test_table_growth(void)
{
    symtab_table table;
    symtab_init(&table);
    
    int pass = 1;
    
    /* Add enough entries to trigger multiple rebuilds */
    for (int i = 0; i < 100; i++) {
        char name[32];
        snprintf(name, sizeof(name), "key%d", i);
        symtab_value_t val = { .id = (uint32_t)i, .flags = 0 };
        if (!symtab_add(&table, name, val, 0)) {
            pass = 0;
            break;
        }
    }
    
    /* Verify all entries are still findable */
    if (pass) {
        for (int i = 0; i < 100; i++) {
            char name[32];
            snprintf(name, sizeof(name), "key%d", i);
            symtab_entry *found = symtab_find(&table, name);
            if (!found || found->value.id != (uint32_t)i) {
                pass = 0;
                break;
            }
        }
    }
    
    symtab_free(&table);
    return pass;
}

/* Test 14: Generation counter */
static int test_generation_counter(void)
{
    symtab_table table;
    symtab_init(&table);
    
    uint64_t gen0 = table.generation;
    
    symtab_value_t val1 = { .id = 1, .flags = 0 };
    symtab_add(&table, "a", val1, 0);
    
    uint64_t gen1 = table.generation;
    
    symtab_value_t val2 = { .id = 2, .flags = 0 };
    symtab_add(&table, "b", val2, 0);
    
    uint64_t gen2 = table.generation;
    
    symtab_free(&table);
    
    return (gen0 == 0 && gen1 > gen0 && gen2 >= gen1);
}

int main(void)
{
    printf("SwissTable Test Suite (Generative API)\n");
    printf("======================================\n\n");
    
    RUN_TEST(test_init);
    RUN_TEST(test_add_find_single);
    RUN_TEST(test_add_multiple);
    RUN_TEST(test_update_entry);
    RUN_TEST(test_delete_entry);
    RUN_TEST(test_find_nonexistent);
    RUN_TEST(test_delete_nonexistent);
    RUN_TEST(test_case_insensitive);
    RUN_TEST(test_hash_consistency);
    RUN_TEST(test_fingerprint);
    RUN_TEST(test_bloom_filter);
    RUN_TEST(test_empty_table);
    RUN_TEST(test_table_growth);
    RUN_TEST(test_generation_counter);
    
    printf("\n---------------------\n");
    printf("Results: %d/%d tests passed\n", tests_passed, tests_run);
    
    return (tests_passed == tests_run) ? 0 : 1;
}
