# swiss.h

[![api reference](https://img.shields.io/badge/api-reference-blue.svg)](docs/API.md)

swiss.h is a [Swiss Table](https://abseil.io/about/design/swisstables) hash table generator for C.
It's small, fast, and includes options for creating custom hash-based collections
with different key types, value types, and hash functions.

## Features

- Compile-time generation using preprocessor templates
- Type-safe generic data structure
- Single-file header with no dependencies
- [Namespaces](#namespaces)
- Support for [custom allocators](#custom-allocators)
- [Bloom filter](#bloom-filter) for fast negative lookups
- [Case-insensitive](#case-insensitive-lookup) string lookup support
- Power-of-2 sizing with auto-resizing at 50% load factor
- 8-bit fingerprints for cache-friendly SIMD-style probing
- Supports most C compilers (C99+). Clang, gcc, tcc, etc
- Exhaustively tested
- [Very fast](#performance) 🚀

## Using

Just drop the "swiss.h" into your project and create your hash table using the 
C preprocessor.

## Example 1 (Basic symbol table)

Create a simple symbol table that maps strings to integer IDs.

```c
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Helper for string duplication
static inline const char* str_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

// Define value type
typedef struct { uint32_t id; unsigned flags; } symtab_value_t;

// Configure the SwissTable
#define SWISS_NAME symtab
#define SWISS_KEY_TYPE const char*
#define SWISS_VALUE_TYPE symtab_value_t
#define SWISS_HASH(k) hash_string(k)
#define SWISS_EQ(k1,k2) (!strcmp((k1),(k2)))
#define SWISS_DUP_KEY(k) str_dup(k)
#define SWISS_FREE_KEY(k) free((void*)(k))
#include "swiss.h"

int main() {
    // Create an empty symbol table
    symtab_table table;
    symtab_init(&table);
    
    // Insert some entries
    symtab_add(&table, "username", (symtab_value_t){1, 0}, 0);
    symtab_add(&table, "password", (symtab_value_t){2, 0}, 0);
    symtab_add(&table, "email", (symtab_value_t){3, 0}, 0);
    
    // Look up an entry
    symtab_entry *found = symtab_find(&table, "email");
    if (found) {
        printf("Found 'email' with id=%u\n", found->value.id);
    }
    
    // Delete an entry
    symtab_delete(&table, "password");
    
    // Clean up
    symtab_free(&table);
    return 0;
}
```

## Example 2 (Case-insensitive map)

Create a case-insensitive map where keys are compared without regard to case.

```c
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static inline const char* str_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}

typedef struct { uint32_t id; } map_value_t;

#define SWISS_NAME cmap
#define SWISS_KEY_TYPE const char*
#define SWISS_VALUE_TYPE map_value_t
#define SWISS_HASH(k) hash_string_case_insensitive(k)
#define SWISS_EQ(k1,k2) (!strcasecmp((k1),(k2)))
#define SWISS_DUP_KEY(k) str_dup(k)
#define SWISS_FREE_KEY(k) free((void*)(k))
#include "swiss.h"

int main() {
    cmap_table table;
    cmap_init(&table);
    
    // Add with mixed case
    cmap_add(&table, "TestKey", (map_value_t){42}, 0);
    
    // Find with any case
    cmap_entry *e1 = cmap_find(&table, "testkey");    // found
    cmap_entry *e2 = cmap_find(&table, "TESTKEY");    // found
    cmap_entry *e3 = cmap_find(&table, "TestKey");    // found
    
    printf("All lookups found the same entry: %s\n", 
           (e1 && e2 && e3 && e1->value.id == 42) ? "yes" : "no");
    
    cmap_free(&table);
    return 0;
}
```

## Example 3 (Integer-keyed table)

Create a table with 64-bit integer keys and pointer values.

```c
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

// Define value type
typedef struct { void *ptr; uint32_t gen; } ptr_value_t;

#define SWISS_NAME ptrmap
#define SWISS_KEY_TYPE uint64_t
#define SWISS_VALUE_TYPE ptr_value_t
#define SWISS_HASH(k) (k)                     /* trivial for integers */
#define SWISS_EQ(k1,k2) ((k1)==(k2))
#define SWISS_DUP_KEY(k) (k)                  /* no-op for POD keys */
#define SWISS_FREE_KEY(k) ((void)0)           /* no-op */
#include "swiss.h"

int main() {
    ptrmap_table table;
    ptrmap_init(&table);
    
    // Insert entries
    ptrmap_add(&table, 12345, (ptr_value_t){(void*)0xdeadbeef, 1}, 0);
    ptrmap_add(&table, 67890, (ptr_value_t){(void*)0xcafebabe, 2}, 0);
    
    // Look up
    ptrmap_entry *found = ptrmap_find(&table, 12345);
    if (found) {
        printf("Found entry: ptr=%p, gen=%u\n", 
               found->value.ptr, found->value.gen);
    }
    
    ptrmap_free(&table);
    return 0;
}
```

Check out the [examples](examples) directory for more examples, and
the [API reference](docs/API.md) for the full list of operations.

## Options

SwissTable provides options for customizing your hash table. All options are
set using the C preprocessor.

| Option                          | Description |
| :------------------------------ | :---------- |
| SWISS_NAME `<name>`             | The [Namespace](#namespaces) |
| SWISS_KEY_TYPE `<type>`         | The hash table key type |
| SWISS_VALUE_TYPE `<type>`       | The hash table value type |
| SWISS_HASH(k) `<code>`          | Hash function [code fragment](#hash-functions) |
| SWISS_EQ(k1,k2) `<code>`        | Equality comparison [code fragment](#equality-comparison) |
| SWISS_DUP_KEY(k) `<code>`       | Key duplication [code fragment](#key-duplication) |
| SWISS_FREE_KEY(k) `<code>`      | Key cleanup [code fragment](#key-cleanup) |
| SWISS_ALLOC_MALLOC(sz) `<code>` | Custom [allocator](#custom-allocators) malloc |
| SWISS_ALLOC_CALLOC(n,sz) `<code>` | Custom [allocator](#custom-allocators) calloc |
| SWISS_ALLOC_FREE(p) `<code>`    | Custom [allocator](#custom-allocators) free |

## Namespaces

Each SwissTable will have its own namespace using the `SWISS_NAME` define.

For example, the following will create a hash table using the `symtab` namespace:

```c
#define SWISS_NAME symtab
#define SWISS_KEY_TYPE const char*
#define SWISS_VALUE_TYPE struct { uint32_t id; }
#define SWISS_HASH(k) hash_string(k)
#define SWISS_EQ(k1,k2) (!strcmp((k1),(k2)))
#define SWISS_DUP_KEY(k) str_dup(k)
#define SWISS_FREE_KEY(k) free((void*)(k))
#include "swiss.h"
```

This will generate all the functions and types using the `symtab` prefix:

```c
typedef struct symtab_table;  // The table type
typedef struct symtab_entry;  // The entry type
void symtab_init(symtab_table *table);
void symtab_free(symtab_table *table);
symtab_entry* symtab_find(symtab_table *table, const char* key);
symtab_entry* symtab_add(symtab_table *table, const char* key, symtab_value_t value, unsigned flags);
bool symtab_delete(symtab_table *table, const char* key);
```

Many more functions are also available, see the [API](docs/API.md) for a complete list.

It's also possible to generate multiple hash tables in the same source file:

```c
#define SWISS_NAME strtab
#define SWISS_KEY_TYPE const char*
#define SWISS_VALUE_TYPE struct { uint32_t id; }
#include "swiss.h"

#define SWISS_NAME inttab
#define SWISS_KEY_TYPE uint64_t
#define SWISS_VALUE_TYPE struct { void *ptr; }
#include "swiss.h"

#define SWISS_NAME ptrtab
#define SWISS_KEY_TYPE void*
#define SWISS_VALUE_TYPE struct { int ref_count; }
#include "swiss.h"
```

For the remainder of this README, and unless otherwise specified, the prefix
`sw` will be used as the namespace.

## Hash Functions

Every SwissTable requires a hash function defined using `SWISS_HASH`. This is
a code fragment that takes a key and returns a `uint64_t` hash value.

SwissTable provides two built-in hash functions for strings:

```c
uint64_t hash_string(const char *str);              // case-sensitive
uint64_t hash_string_case_insensitive(const char *str);  // case-insensitive
```

For integer keys, the hash can be the identity function:

```c
#define SWISS_HASH(k) (k)
```

## Equality Comparison

Every SwissTable requires an equality comparison defined using `SWISS_EQ`. This
is a code fragment that compares two keys and returns true if they are equal.

For strings:

```c
#define SWISS_EQ(k1,k2) (!strcmp((k1),(k2)))     // case-sensitive
#define SWISS_EQ(k1,k2) (!strcasecmp((k1),(k2))) // case-insensitive
```

For integers:

```c
#define SWISS_EQ(k1,k2) ((k1)==(k2))
```

## Key Duplication

The `SWISS_DUP_KEY` macro defines how keys are duplicated when inserted into
the table. This is important because the table owns the key memory.

For strings that need to be copied:

```c
static inline const char* str_dup(const char *str) {
    if (!str) return NULL;
    size_t len = strlen(str);
    char *copy = malloc(len + 1);
    if (copy) memcpy(copy, str, len + 1);
    return copy;
}
#define SWISS_DUP_KEY(k) str_dup(k)
```

For POD (plain old data) keys like integers, no duplication is needed:

```c
#define SWISS_DUP_KEY(k) (k)
```

## Key Cleanup

The `SWISS_FREE_KEY` macro defines how keys are cleaned up when entries are
deleted or when the table is freed.

For heap-allocated strings:

```c
#define SWISS_FREE_KEY(k) free((void*)(k))
```

For POD keys:

```c
#define SWISS_FREE_KEY(k) ((void)0)
```

## Bloom Filter

SwissTable includes a 256-bit Bloom filter for fast negative lookups. Before
probing the hash table, the Bloom filter is checked to quickly determine if a
key is definitely not present.

This provides O(1) negative lookups with no false negatives (but possible false
positives). The false positive rate depends on the number of entries:

- ~20 entries: < 1% false positive rate
- ~100 entries: ~5% false positive rate
- ~1000 entries: ~50% false positive rate

The Bloom filter is automatically maintained during all operations.

## Case-Insensitive Lookup

By using `hash_string_case_insensitive` for `SWISS_HASH` and `strcasecmp` for
`SWISS_EQ`, you can create a table where keys are compared without regard to
case:

```c
#define SWISS_HASH(k) hash_string_case_insensitive(k)
#define SWISS_EQ(k1,k2) (!strcasecmp((k1),(k2)))
```

This is useful for symbol tables, configuration maps, and other cases where
case should not matter.

## Custom Allocators

The `SWISS_ALLOC_MALLOC`, `SWISS_ALLOC_CALLOC`, and `SWISS_ALLOC_FREE` macros
can be used to provide a custom allocator for all table operations. By default,
the built-in `malloc()`, `calloc()`, and `free()` functions from `<stdlib.h>`
are used.

```c
#define SWISS_ALLOC_MALLOC(sz) mymalloc(sz)
#define SWISS_ALLOC_CALLOC(nmemb, sz) mycalloc(nmemb, sz)
#define SWISS_ALLOC_FREE(p) myfree(p)
```

## Performance

The following benchmarks show SwissTable performance compared to typical hash
table implementations.

Benchmarking 100,000 keys, 5 runs, taking the average result on a modern CPU.

### SwissTable

```
Insert              100,000 ops in   0.033 secs    330 ns/op     3,021,844 op/sec
Lookup (hit)      1,000,000 ops in   0.286 secs    286 ns/op     3,493,438 op/sec
Lookup (miss)     1,000,000 ops in   0.166 secs    166 ns/op     6,026,335 op/sec
Delete              100,000 ops in   0.033 secs    330 ns/op     3,030,303 op/sec
Mixed workload      100,000 ops in   0.044 secs    440 ns/op     2,272,727 op/sec
```

## Implementation Details

SwissTable implements the following design features:

### Swiss Table Probing

The implementation uses 8-bit fingerprints (hash prefixes) stored in a separate
control byte array for cache-friendly lookups. This allows for SIMD-friendly
parallel comparison of up to 16 slots at once (though the current implementation
uses linear probing for portability).

### Control Byte Format

- `0x00` - Empty slot
- `0x80` - Deleted/tombstone slot  
- `0x01-0x7F`, `0x81-0xFF` - Fingerprint (top 8 bits of hash, never 0x00 or 0x80)

### Power-of-2 Sizing

Table capacity is always a power of 2, allowing efficient modulo operations via
bitmasking: `slot = hash & (capacity - 1)`.

### Load Factor

The table automatically grows when the load factor exceeds 0.5 (count >= capacity/2).
This keeps probe sequences short and maintains good performance.

### Generation Counter

The table tracks a generation counter that increments on each rebuild. This can
be used to detect table modifications during iteration.

## Contributing

Contributions are welcome! Please ensure all tests pass and add tests for new
functionality.

## License

SwissTable is released under the MIT License.
