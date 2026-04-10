#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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

/* Define the generative table configuration for a symbol table */
#define SWISS_NAME symtab
#define SWISS_KEY_TYPE const char*
#define SWISS_VALUE_TYPE symtab_value_t
#define SWISS_HASH(k) hash_string_case_insensitive(k)
#define SWISS_EQ(k1,k2) (!strcasecmp((k1),(k2)))
#define SWISS_DUP_KEY(k) str_dup(k)
#define SWISS_FREE_KEY(k) free((void*)(k))
#include "../swiss.h"

/* Example demonstrating SwissTable generative API usage */

int main(void)
{
    printf("SwissTable Example (Generative API)\n");
    printf("===================================\n\n");
    
    /* Initialize table */
    symtab_table table;
    symtab_init(&table);
    printf("Created empty symbol table\n");
    printf("  Initial capacity: %u\n", table.capacity);
    printf("  Initial count: %u\n\n", table.count);
    
    /* Add some entries */
    printf("Adding entries...\n");
    
    symtab_value_t entries[] = {
        { .id = 1, .flags = 0 },
        { .id = 2, .flags = 0 },
        { .id = 3, .flags = 0 },
        { .id = 4, .flags = 0 },
        { .id = 5, .flags = 0 },
    };
    const char *names[] = {"username", "password", "email", "age", "country"};
    
    for (int i = 0; i < 5; i++) {
        symtab_entry *added = symtab_add(&table, names[i], entries[i], 0);
        if (added) {
            printf("  Added '%s' with id=%u, hash=%016llx\n", 
                   added->key, added->value.id, (unsigned long long)added->hash);
        }
    }
    
    printf("\nTable status:\n");
    printf("  Capacity: %u\n", table.capacity);
    printf("  Count: %u\n", table.count);
    printf("  Generation: %llu\n\n", (unsigned long long)table.generation);
    
    /* Look up entries */
    printf("Looking up entries...\n");
    
    const char *lookups[] = {"email", "username", "unknown"};
    for (int i = 0; i < 3; i++) {
        symtab_entry *found = symtab_find(&table, lookups[i]);
        if (found) {
            printf("  Found '%s': id=%u, flags=%u\n", 
                   lookups[i], found->value.id, found->flags);
        } else {
            printf("  '%s' not found\n", lookups[i]);
        }
    }
    
    /* Demonstrate case-insensitive lookup */
    printf("\nCase-insensitive lookup:\n");
    symtab_entry *found1 = symtab_find(&table, "EMAIL");
    symtab_entry *found2 = symtab_find(&table, "Email");
    symtab_entry *found3 = symtab_find(&table, "email");
    printf("  find(\"EMAIL\"): %s\n", found1 ? "found" : "not found");
    printf("  find(\"Email\"): %s\n", found2 ? "found" : "not found");
    printf("  find(\"email\"): %s\n", found3 ? "found" : "not found");
    
    /* Update an entry */
    printf("\nUpdating 'age' entry...\n");
    symtab_value_t updated = { .id = 42, .flags = 1 };
    symtab_entry *result = symtab_add(&table, "age", updated, 0);
    if (result) {
        printf("  Updated 'age': id=%u, flags=%u\n", result->value.id, result->flags);
    }
    
    /* Delete an entry */
    printf("\nDeleting 'password' entry...\n");
    if (symtab_delete(&table, "password")) {
        printf("  Successfully deleted 'password'\n");
    }
    
    /* Verify deletion */
    symtab_entry *deleted = symtab_find(&table, "password");
    printf("  find(\"password\") after delete: %s\n", 
           deleted ? "found (error!)" : "not found (correct)");
    
    printf("\nFinal table status:\n");
    printf("  Capacity: %u\n", table.capacity);
    printf("  Count: %u\n", table.count);
    printf("  Generation: %llu\n", (unsigned long long)table.generation);
    
    /* Show bloom filter state */
    printf("\nBloom filter state:\n");
    printf("  bloom[0] = 0x%016llx\n", (unsigned long long)table.bloom[0]);
    printf("  bloom[1] = 0x%016llx\n", (unsigned long long)table.bloom[1]);
    printf("  bloom[2] = 0x%016llx\n", (unsigned long long)table.bloom[2]);
    printf("  bloom[3] = 0x%016llx\n", (unsigned long long)table.bloom[3]);
    
    /* Cleanup */
    symtab_free(&table);
    printf("\nTable freed.\n");
    
    return 0;
}
