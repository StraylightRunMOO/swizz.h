# swizz.h

[![api reference](https://img.shields.io/badge/api-reference-blue.svg)](docs/API.md)

swizz.h is a [Swiss Table](https://abseil.io/about/design/swisstables) hash table generator for C.
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

Just drop the "swizz.h" into your project and create your hash table using the 
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

// Configure the SwizzTable
#define SWIZZ_NAME symtab
#define SWIZZ_KEY_TYPE const char*
#define SWIZZ_VALUE_TYPE symtab_value_t
#define SWIZZ_HASH(k) hash_string(k)
#define SWIZZ_EQ(k1,k2) (!strcmp((k1),(k2)))
#define SWIZZ_DUP_KEY(k) str_dup(k)
#define SWIZZ_FREE_KEY(k) free((void*)(k))
#include "swizz.h"

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

#define SWIZZ_NAME cmap
#define SWIZZ_KEY_TYPE const char*
#define SWIZZ_VALUE_TYPE map_value_t
#define SWIZZ_HASH(k) hash_string_case_insensitive(k)
#define SWIZZ_EQ(k1,k2) (!strcasecmp((k1),(k2)))
#define SWIZZ_DUP_KEY(k) str_dup(k)
#define SWIZZ_FREE_KEY(k) free((void*)(k))
#include "swizz.h"

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

#define SWIZZ_NAME ptrmap
#define SWIZZ_KEY_TYPE uint64_t
#define SWIZZ_VALUE_TYPE ptr_value_t
#define SWIZZ_HASH(k) (k)                     /* trivial for integers */
#define SWIZZ_EQ(k1,k2) ((k1)==(k2))
#define SWIZZ_DUP_KEY(k) (k)                  /* no-op for POD keys */
#define SWIZZ_FREE_KEY(k) ((void)0)           /* no-op */
#include "swizz.h"

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

SwizzTable provides options for customizing your hash table. All options are
set using the C preprocessor.

| Option                          | Description |
| :------------------------------ | :---------- |
| SWIZZ_NAME `<name>`             | The [Namespace](#namespaces) |
| SWIZZ_KEY_TYPE `<type>`         | The hash table key type |
| SWIZZ_VALUE_TYPE `<type>`       | The hash table value type |
| SWIZZ_HASH(k) `<code>`          | Hash function [code fragment](#hash-functions) |
| SWIZZ_EQ(k1,k2) `<code>`        | Equality comparison [code fragment](#equality-comparison) |
| SWIZZ_DUP_KEY(k) `<code>`       | Key duplication [code fragment](#key-duplication) |
| SWIZZ_FREE_KEY(k) `<code>`      | Key cleanup [code fragment](#key-cleanup) |
| SWIZZ_ALLOC_MALLOC(sz) `<code>` | Custom [allocator](#custom-allocators) malloc |
| SWIZZ_ALLOC_CALLOC(n,sz) `<code>` | Custom [allocator](#custom-allocators) calloc |
| SWIZZ_ALLOC_FREE(p) `<code>`    | Custom [allocator](#custom-allocators) free |

## Namespaces

Each SwizzTable will have its own namespace using the `SWIZZ_NAME` define.

For example, the following will create a hash table using the `symtab` namespace:

```c
#define SWIZZ_NAME symtab
#define SWIZZ_KEY_TYPE const char*
#define SWIZZ_VALUE_TYPE struct { uint32_t id; }
#define SWIZZ_HASH(k) hash_string(k)
#define SWIZZ_EQ(k1,k2) (!strcmp((k1),(k2)))
#define SWIZZ_DUP_KEY(k) str_dup(k)
#define SWIZZ_FREE_KEY(k) free((void*)(k))
#include "swizz.h"
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
#define SWIZZ_NAME strtab
#define SWIZZ_KEY_TYPE const char*
#define SWIZZ_VALUE_TYPE struct { uint32_t id; }
#include "swizz.h"

#define SWIZZ_NAME inttab
#define SWIZZ_KEY_TYPE uint64_t
#define SWIZZ_VALUE_TYPE struct { void *ptr; }
#include "swizz.h"

#define SWIZZ_NAME ptrtab
#define SWIZZ_KEY_TYPE void*
#define SWIZZ_VALUE_TYPE struct { int ref_count; }
#include "swizz.h"
```

For the remainder of this README, and unless otherwise specified, the prefix
`sw` will be used as the namespace.

## Hash Functions

Every SwizzTable requires a hash function defined using `SWIZZ_HASH`. This is
a code fragment that takes a key and returns a `uint64_t` hash value.

SwizzTable provides two built-in hash functions for strings:

```c
uint64_t hash_string(const char *str);              // case-sensitive
uint64_t hash_string_case_insensitive(const char *str);  // case-insensitive
```

For integer keys, the hash can be the identity function:

```c
#define SWIZZ_HASH(k) (k)
```

## Equality Comparison

Every SwizzTable requires an equality comparison defined using `SWIZZ_EQ`. This
is a code fragment that compares two keys and returns true if they are equal.

For strings:

```c
#define SWIZZ_EQ(k1,k2) (!strcmp((k1),(k2)))     // case-sensitive
#define SWIZZ_EQ(k1,k2) (!strcasecmp((k1),(k2))) // case-insensitive
```

For integers:

```c
#define SWIZZ_EQ(k1,k2) ((k1)==(k2))
```

## Key Duplication

The `SWIZZ_DUP_KEY` macro defines how keys are duplicated when inserted into
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
#define SWIZZ_DUP_KEY(k) str_dup(k)
```

For POD (plain old data) keys like integers, no duplication is needed:

```c
#define SWIZZ_DUP_KEY(k) (k)
```

## Key Cleanup

The `SWIZZ_FREE_KEY` macro defines how keys are cleaned up when entries are
deleted or when the table is freed.

For heap-allocated strings:

```c
#define SWIZZ_FREE_KEY(k) free((void*)(k))
```

For POD keys:

```c
#define SWIZZ_FREE_KEY(k) ((void)0)
```

## Bloom Filter

SwizzTable includes a 256-bit Bloom filter for fast negative lookups. Before
probing the hash table, the Bloom filter is checked to quickly determine if a
key is definitely not present.

This provides O(1) negative lookups with no false negatives (but possible false
positives). The false positive rate depends on the number of entries:

- ~20 entries: < 1% false positive rate
- ~100 entries: ~5% false positive rate
- ~1000 entries: ~50% false positive rate

The Bloom filter is automatically maintained during all operations.

## Case-Insensitive Lookup

By using `hash_string_case_insensitive` for `SWIZZ_HASH` and `strcasecmp` for
`SWIZZ_EQ`, you can create a table where keys are compared without regard to
case:

```c
#define SWIZZ_HASH(k) hash_string_case_insensitive(k)
#define SWIZZ_EQ(k1,k2) (!strcasecmp((k1),(k2)))
```

This is useful for symbol tables, configuration maps, and other cases where
case should not matter.

## Custom Allocators

The `SWIZZ_ALLOC_MALLOC`, `SWIZZ_ALLOC_CALLOC`, and `SWIZZ_ALLOC_FREE` macros
can be used to provide a custom allocator for all table operations. By default,
the built-in `malloc()`, `calloc()`, and `free()` functions from `<stdlib.h>`
are used.

```c
#define SWIZZ_ALLOC_MALLOC(sz) mymalloc(sz)
#define SWIZZ_ALLOC_CALLOC(nmemb, sz) mycalloc(nmemb, sz)
#define SWIZZ_ALLOC_FREE(p) myfree(p)
```

## Performance

The following benchmarks show SwizzTable performance compared to typical hash
table implementations.

Benchmarking 100,000 keys, 5 runs, taking the average result on a modern CPU.

### SwizzTable

```
Insert              100,000 ops in   0.033 secs    330 ns/op     3,021,844 op/sec
Lookup (hit)      1,000,000 ops in   0.286 secs    286 ns/op     3,493,438 op/sec
Lookup (miss)     1,000,000 ops in   0.166 secs    166 ns/op     6,026,335 op/sec
Delete              100,000 ops in   0.033 secs    330 ns/op     3,030,303 op/sec
Mixed workload      100,000 ops in   0.044 secs    440 ns/op     2,272,727 op/sec
```

## Implementation Details

SwizzTable implements the following design features:

### Swizz Table Probing

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

SwizzTable is released under the MIT License.
