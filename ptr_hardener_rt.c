/*
 * ptr_hardner - instrumentation functions
 * ---------------------------------------
 *
 * Written by Gwangmu Lee <iss300@proton.me>
 *
 * Functions that will be inserted in place of
 * (de)allocallocators and pointer assignments
 * and dereferences.
 */

/*
 * Glossary:
 *  - objmap := Object Map
 *  - ptrmap := Pointer Map
 *  - mapent := (Generic) Map Entry
 *  - Variables starting with a redundant 'a' := granule-aligned
 *  - Pointers starting with a redundant 'ee' := Assignee (lhs)
 *  - Pointers starting with a redundant 'er' := Assigner (rhs)
 *  - rvalue := non-lvalue (from C standard)
 */

#include <assert.h>
#include <dlfcn.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdarg.h> // NOTE: debugging
#include <unistd.h> // NOTE: debugging

/** Type/constant declarations **/

// Generic map entry: range information and associated pointers.
//  - NULL:         null entry (=no object)
//  - tag == 0:     map entry ('next' = nested map)
//  - otherwise:    normal entry (=range)
//                  (tag == -1: anonymous)
#define COMMON_MAP_ENTRY                                                       \
    struct {                                                                   \
        void *tag;  /* Hash tag: the address of this granule. */               \
        void *base; /* Range: base address. */                                 \
        size_t len; /* Range: length (in bytes). */                            \
        void *next; /* Assoc. ptrs: next entry. */                             \
    }

#define __PH_MAPENT_SET_MAP(entry, map)                                        \
    do {                                                                       \
        assert((entry) != NULL); \
        (entry)->tag = NULL;                                              \
        (entry)->base = map;                                                   \
    } while (0)
#define __PH_MAPENT_IS_MAP(entry) ((entry) != NULL && (entry)->tag == NULL)
#define __PH_MAPENT_GET_MAP(entry)                                             \
    (assert(__PH_MAPENT_IS_MAP(entry)), (entry)->base)

#define __PH_MAPENT_TAG_ANON ((void *)~(uintptr_t)0)

// Object map entry
struct objmap_entry {
    COMMON_MAP_ENTRY;
};

// Pointer map entry
struct ptrmap_entry {
    COMMON_MAP_ENTRY;
    void *prev; /* Assoc. ptrs: previous entry. */
};

typedef uint64_t ptrmap_index_t;
typedef uint64_t objmap_index_t;

// NOTE: reduced for debugging. Original "1 << 16".
#define __PH_OBJMAP_NR_ENTRIES (1 << 3)
#define __PH_PTRMAP_NR_ENTRIES (1 << 3)

// Granule: the minimum unit of byte alignment.
#define __PH_GRANULE_SIZE (1 << 5)
#define __PH_GRANULE_MASK (__PH_GRANULE_SIZE - 1)

#define __PH_GRANULE_CEIL(x)                                                   \
    (((intptr_t)(x) + __PH_GRANULE_SIZE - 1) & ~(intptr_t)__PH_GRANULE_MASK)

// Actual standard allocators. The default behavior is to initialize them when
// they're used the first time, but they _may_ be updated by a global
// constructor if there is an annotated custom allocator. The custom ones should
// have the same type signature to the allocator that it wants to replace.
// Let's say multiple custom allocators (for the same original allocator) causes
// an undefined behavior, though I think it'll function just fine.
void *(*aalloc_impl)(size_t, size_t) __attribute__((weak)); // 'aligned_alloc'
void (*free_impl)(void *) __attribute__((weak));            // 'free'

// Untracked object tolerance
#define __PH_UNTRACKED_OBJ_TOLERANCE (1 << 6)

// Stack handling
// FIXME: reimplement this in a portable way.
#define __PH_STACK_MASK (0x700000000000)
#define __PH_STACK_BASE ((void *)__PH_STACK_MASK)
#define __PH_STACK_LEN (0x0fffffffffff)

#define __PH_IS_STACKADDR(addr)                                                \
    (((intptr_t)(addr) & __PH_STACK_MASK) == __PH_STACK_MASK)

static struct ptrmap_entry __ph_stack_ptrmap_entry = {.tag = __PH_MAPENT_TAG_ANON,
                                      .base = __PH_STACK_BASE,
                                      .len = __PH_STACK_LEN};

// Global pointer map: the entry point of the pointer/object map. On a hash
// collision, the collided entries are moved to a nested map. Thanks to
// __attribute__((weak)), all global variables in different translation units
// would share the same storage space.
struct ptrmap_entry **__ph_ptrmap __attribute__((weak));

// Global object map: roughly the same as the global pointer map.
struct objmap_entry **__ph_objmap __attribute__((weak));

/** Debug utilities **/

static char *__ph_printf_convert(uint64_t num, int base, int *len) {
    // Based on:
    // https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
    static char repr[] = "0123456789abcdef";
    static char buffer[50];
    char *ptr;
    int _len = 0;

    ptr = &buffer[49];
    *ptr = '\0';

    do {
        *--ptr = repr[num % base];
        num /= base;
        _len++;
    } while (num != 0);

    *len = _len;
    return ptr;
}

static void __ph_printf(char *format, ...) {
#ifdef NDEBUG
    return;
#else
    // Based on:
    // https://stackoverflow.com/questions/1735236/how-to-write-my-own-printf-in-c
    char *traverse;
    uint64_t i;
    char *s;
    int len;

    va_list arg;
    va_start(arg, format);

    for (traverse = format; *traverse != '\0'; traverse++) {
        while (*traverse != '%') {
            if (*traverse == '\0')
                return;
            write(STDOUT_FILENO, traverse, 1);
            traverse++;
        }
        traverse++;

        int pad = 0;
        while (*traverse >= '0' && *traverse <= '9') {
            pad = (pad * 10) + (*traverse - '0');
            traverse++;
        }

        len = 0;
        switch (*traverse) {
        case 'c':
            i = va_arg(arg, int);
            write(STDOUT_FILENO, &i, 1);
            len++;
            break;
        case 'd':
            i = va_arg(arg, int);
            if (i < 0) {
                i = -i;
                write(STDOUT_FILENO, "-", 1);
                len++;
            }
            int dsize;
            char *dconv = __ph_printf_convert(i, 10, &dsize);
            write(STDOUT_FILENO, dconv, dsize);
            len += dsize;
            break;
        case 's':
            len = 0;
            s = va_arg(arg, char *);
            for (; *s != '\0'; s++) {
                write(STDOUT_FILENO, s, 1);
                len++;
            }
            break;
        case 'x':
            i = va_arg(arg, unsigned int);
            int xsize;
            char *xconv = __ph_printf_convert(i, 16, &xsize);
            write(STDOUT_FILENO, xconv, xsize);
            len += xsize;
            break;
        case 'p':
            i = va_arg(arg, uintptr_t);
            int psize;
            char *pconv = __ph_printf_convert(i, 16, &psize);
            write(STDOUT_FILENO, "0x", 2);
            write(STDOUT_FILENO, pconv, psize);
            len += psize + 2;
            break;
        }
        while (len < pad) {
            write(STDOUT_FILENO, " ", 1);
            len++;
        }
    }

    va_end(arg);
#endif
}

static void __ph_ptrmap_print_inner(struct ptrmap_entry **ptrmap,
                                    unsigned base_lv, unsigned lv, bool stop,
                                    size_t n_entries) {
    // FIXME: merge this function to objmap's.
    assert(ptrmap);

    char *types[n_entries];
    for (int i = 0; i < n_entries; i++) {
        if (ptrmap[i] == NULL)
            types[i] = "NULL";
        else if (__PH_MAPENT_IS_MAP(ptrmap[i]))
            types[i] = "MAP";
        else 
            types[i] = "NORMAL";
    }

    __ph_printf("Pointer map %d entries (level: %d, addr: %p)\n", n_entries,
                base_lv + lv, ptrmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
        __ph_printf("┐");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("type: %18s", types[i]);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!ptrmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("base: %18p", ptrmap[i]->base);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!ptrmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("len : %18d", ptrmap[i]->len);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!ptrmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("next: %18p", ptrmap[i]->next);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!ptrmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("prev: %18p", ptrmap[i]->prev);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!ptrmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("tag : %18p", ptrmap[i]->tag);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("└");
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
        __ph_printf("┘");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf(" ");
        __ph_printf("%24d", i);
        __ph_printf(" ");
    }
    __ph_printf("\n");
    __ph_printf("\n");

    if (!stop) {
        for (int i = 0; i < n_entries; i++) {
            if (ptrmap[i] && __PH_MAPENT_IS_MAP(ptrmap[i]))
                __ph_ptrmap_print_inner(ptrmap[i]->base, base_lv, lv + 1, stop,
                                        __PH_PTRMAP_NR_ENTRIES);
        }
    }
}

static void __ph_ptrmap_print() {
#ifdef NDEBUG
    return;
#else
    if (!__ph_ptrmap) {
        __ph_printf("(no range map)\n");
        return;
    }

    __ph_ptrmap_print_inner(__ph_ptrmap, 0, 0, false, __PH_PTRMAP_NR_ENTRIES);
#endif
}

static void __ph_ptrmap_entry_print(unsigned base_lv,
                                    struct ptrmap_entry *entry) {
#ifdef NDEBUG
    return;
#else
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_ptrmap_print_inner(&entry, base_lv, 0, true, 1);
#endif
}

static void __ph_objmap_print_inner(struct objmap_entry **objmap,
                                    unsigned base_lv, unsigned lv, bool stop,
                                    size_t n_entries) {
    // FIXME: merge this function to ptrmap's.
    assert(objmap);

    char *types[n_entries];
    for (int i = 0; i < n_entries; i++) {
        if (objmap[i] == NULL)
            types[i] = "NULL";
        else if (__PH_MAPENT_IS_MAP(objmap[i]))
            types[i] = "MAP";
        else 
            types[i] = "NORMAL";
    }

    __ph_printf("Object map %d entries (level: %d, addr: %p)\n", n_entries,
                base_lv + lv, objmap);

    for (int i = 0; i < n_entries; i++) {
        __ph_printf("┌");
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
        __ph_printf("┐");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("│");
        __ph_printf("type: %18s", types[i]);
        __ph_printf("│");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!objmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("base: %18p", objmap[i]->base);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!objmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("len : %18d", objmap[i]->len);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!objmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("next: %18p", objmap[i]->next);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        if (!objmap[i]) {
            __ph_printf("|%24s|", "");
        } else {
            __ph_printf("│");
            __ph_printf("tag : %18p", objmap[i]->tag);
            __ph_printf("│");
        }
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf("└");
        for (int c = 0; c < 24; c++)
            __ph_printf("─");
        __ph_printf("┘");
    }
    __ph_printf("\n");
    for (int i = 0; i < n_entries; i++) {
        __ph_printf(" ");
        __ph_printf("%24d", i);
        __ph_printf(" ");
    }
    __ph_printf("\n");
    __ph_printf("\n");

    if (!stop) {
        for (int i = 0; i < n_entries; i++) {
            if (objmap[i] && __PH_MAPENT_IS_MAP(objmap[i]))
                __ph_objmap_print_inner(objmap[i]->base, base_lv, lv + 1, stop,
                                        __PH_OBJMAP_NR_ENTRIES);
        }
    }
}

static void __ph_objmap_print() {
#ifdef NDEBUG
    return;
#else
    if (!__ph_objmap) {
        __ph_printf("(no range map)\n");
        return;
    }

    __ph_objmap_print_inner(__ph_objmap, 0, 0, true, __PH_OBJMAP_NR_ENTRIES);
#endif
}

static void __ph_objmap_entry_print(unsigned base_lv,
                                    struct objmap_entry *entry) {
#ifdef NDEBUG
    return;
#else
    if (!entry) {
        __ph_printf("(no range map entry)\n");
        return;
    }

    __ph_objmap_print_inner(&entry, base_lv, 0, true, 1);
#endif
}

static void __ph_map_print() {
    __ph_objmap_print();
    __ph_ptrmap_print();
}

/** General utilities **/

static ptrmap_index_t __ph_ptrmap_index(void *addr, unsigned seed) {
    // FIXME: optimize hashing with a number sequence visible to this TU.
    srand(((uintptr_t)addr + seed) % (uintptr_t)UINT_MAX);
    ptrmap_index_t hash = rand();
    hash %= __PH_PTRMAP_NR_ENTRIES;
    return hash;
}

static objmap_index_t __ph_objmap_index(void *addr, unsigned seed) {
    // FIXME: optimize hashing with a number sequence visible to this TU.
    srand(((uintptr_t)addr + seed) % (uintptr_t)UINT_MAX);
    objmap_index_t hash = rand();
    hash %= __PH_OBJMAP_NR_ENTRIES;
    return hash;
}

static bool __ph_init_aalloc() {
    if (!aalloc_impl) {
        __ph_printf("info: initialize aalloc_impl.\n");
        aalloc_impl = dlsym(RTLD_NEXT, "aligned_alloc");
    }
    return !!aalloc_impl;
}

static bool __ph_init_free() {
    if (!free_impl) {
        __ph_printf("info: initialize free_impl.\n");
        free_impl = dlsym(RTLD_NEXT, "free");
    }
    return !!free_impl;
}

/** Pointer map **/

static struct ptrmap_entry **__ph_ptrmap_create(unsigned n_entries) {
    assert(aalloc_impl);
    unsigned size = n_entries * sizeof(struct ptrmap_entry *);
    struct ptrmap_entry **ret = aalloc_impl(1, size);
    if (!ret)
        return NULL;
    memset(ret, 0, size);
    return ret;
}

static struct ptrmap_entry *__ph_ptrmap_entry_create() {
    assert(aalloc_impl);
    void *ret = aalloc_impl(1, sizeof(struct ptrmap_entry));
    if (!ret) return NULL;
    memset(ret, 0, sizeof(struct ptrmap_entry));
    return ret;
}

static struct ptrmap_entry *
__ph_ptrmap_entry_insert_to_list(struct ptrmap_entry *this,
                                 struct ptrmap_entry *prev) {
    if (!this)
        return NULL;
    if (!prev)
        return this;
    this->next = prev->next;
    this->prev = prev;
    prev->next = this;
    return this;
}

static struct ptrmap_entry *
__ph_ptrmap_entry_remove_from_list(struct ptrmap_entry *this) {
    if (!this)
        return NULL;
    if (this->prev)
        ((struct ptrmap_entry *)this->prev)->next = this->next;
    if (this->next)
        ((struct ptrmap_entry *)this->next)->prev = this->prev;
    this->prev = this->next = NULL;
    return this;
}

static struct ptrmap_entry *
__ph_ptrmap_entry_update(struct ptrmap_entry *ret, struct ptrmap_entry *prev) {
    struct ptrmap_entry evalue = {
        .tag = __PH_MAPENT_TAG_ANON, .base = prev->base, .len = prev->len};
    *ret = evalue;
    ret = __ph_ptrmap_entry_insert_to_list(ret, prev);
    assert(ret);
    return ret;
}

static struct ptrmap_entry *
__ph_ptrmap_update_entry(struct ptrmap_entry **ptrmap,
                         struct ptrmap_entry evalue, unsigned lv) {
    assert(evalue.tag != NULL);

    if (lv == UINT_MAX)
        return NULL;

    ptrmap_index_t idx = __ph_ptrmap_index(evalue.tag, lv);
    assert(0 <= idx && idx < __PH_PTRMAP_NR_ENTRIES);
    struct ptrmap_entry *entry = ptrmap[idx];

    if (entry == NULL || entry->tag == evalue.tag) {
        __ph_printf("info: creating a ptrmap entry...\n");
        if (entry == NULL) {
            ptrmap[idx] = entry = __ph_ptrmap_entry_create();
        } else if (entry->tag == evalue.tag) {
            if (!__ph_ptrmap_entry_remove_from_list(entry))
                return NULL;
        }
        entry->base = evalue.base;
        entry->len = evalue.len;
        entry->tag = evalue.tag;

        __ph_ptrmap_entry_print(lv, entry);

        return entry;
    } else if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_ptrmap_update_entry(__PH_MAPENT_GET_MAP(entry), evalue,
                                        lv + 1);
    } else /* normal entry */ {
        __ph_printf("info: creating a nested otrmap...\n");
        assert(entry->tag != evalue.tag);

        struct ptrmap_entry *map_entry = __ph_ptrmap_entry_create();
        if (!map_entry)
            return NULL;

        struct ptrmap_entry **new_ptrmap = __ph_ptrmap_create(__PH_PTRMAP_NR_ENTRIES);
        if (!new_ptrmap)
            return NULL;

        __PH_MAPENT_SET_MAP(map_entry, new_ptrmap);
        ptrmap[idx] = map_entry;
        new_ptrmap[__ph_ptrmap_index(entry->tag, lv + 1)] = entry;

        struct ptrmap_entry *entry2 =
            __ph_ptrmap_update_entry(new_ptrmap, evalue, lv + 1);
        if (!entry2)
            return NULL;

        __ph_printf("info: created a nested ptrmap.\n");
        __ph_ptrmap_entry_print(lv, map_entry);

        return entry2;
    }
}

static struct ptrmap_entry *
__ph_ptrmap_update_entry_by_tag(void *tag, struct ptrmap_entry *prev) {
    if (!__ph_ptrmap)
        __ph_ptrmap = __ph_ptrmap_create(__PH_PTRMAP_NR_ENTRIES);

    struct ptrmap_entry evalue = {
        .tag = tag, .base = prev->base, .len = prev->len};
    struct ptrmap_entry *ret = __ph_ptrmap_update_entry(__ph_ptrmap, evalue, 0);
    ret = __ph_ptrmap_entry_insert_to_list(ret, prev);
    assert(ret);
    return ret;
}

static struct ptrmap_entry *
__ph_ptrmap_get_entry_inner(struct ptrmap_entry **ptrmap, void *tag,
                            unsigned lv) {
    if (!ptrmap)
        return NULL;
    struct ptrmap_entry *entry = ptrmap[__ph_ptrmap_index(tag, lv)];

    if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_ptrmap_get_entry_inner(__PH_MAPENT_GET_MAP(entry), tag,
                                           lv + 1);
    } else if (entry != NULL && entry->tag == tag) {
        return entry;
    } else {
        return NULL;
    }
}

static struct ptrmap_entry *__ph_ptrmap_get_entry(void *tag) {
    if (!__ph_ptrmap)
        return NULL;
    return __ph_ptrmap_get_entry_inner(__ph_ptrmap, tag, 0);
}

/** Object map **/

static struct objmap_entry **__ph_objmap_create(unsigned n_entries) {
    assert(aalloc_impl);
    unsigned size = n_entries * sizeof(struct objmap_entry *);
    struct objmap_entry **ret = aalloc_impl(1, size);
    if (!ret)
        return 0;
    memset(ret, 0, size);
    return ret;
}

static struct objmap_entry *__ph_objmap_entry_create() {
    assert(aalloc_impl);
    void *ret = aalloc_impl(1, sizeof(struct objmap_entry));
    if (!ret) return NULL;
    memset(ret, 0, sizeof(struct objmap_entry));
    return ret;
}

static void __ph_objmap_entry_destroy(struct objmap_entry *entry) {
    assert(free_impl);
    free_impl(entry);
}

static struct objmap_entry *
__ph_objmap_create_entry(struct objmap_entry **objmap,
                         struct objmap_entry evalue, unsigned lv) {
    assert(evalue.tag != NULL);

    if (lv == UINT_MAX)
        return NULL;

    objmap_index_t idx = __ph_objmap_index(evalue.tag, lv);
    assert(0 <= idx && idx < __PH_OBJMAP_NR_ENTRIES);
    struct objmap_entry *entry = objmap[idx];

    if (entry == NULL) {
        __ph_printf("info: creating a objmap entry...\n");
        objmap[idx] = entry = __ph_objmap_entry_create();
        entry->base = evalue.base;
        entry->len = evalue.len;
        entry->tag = evalue.tag;
        __ph_objmap_entry_print(lv, entry);
        return entry;
    } else if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_objmap_create_entry(__PH_MAPENT_GET_MAP(entry), evalue,
                                        lv + 1);
    } else /* normal entry */ {
        __ph_printf("info: creating a nested objmap...\n");
        assert(entry->tag != evalue.tag);

        struct objmap_entry *map_entry = __ph_objmap_entry_create();
        if (!map_entry)
            return NULL;

        struct objmap_entry **new_objmap = __ph_objmap_create(__PH_OBJMAP_NR_ENTRIES);
        if (!new_objmap)
            return NULL;

        __PH_MAPENT_SET_MAP(map_entry, new_objmap);
        objmap[idx] = map_entry;
        new_objmap[__ph_objmap_index(entry->tag, lv + 1)] = entry;

        struct objmap_entry *entry2 =
            __ph_objmap_create_entry(new_objmap, evalue, lv + 1);
        if (!entry2)
            return NULL;

        __ph_printf("info: created a nested objmap.\n");
        __ph_objmap_entry_print(lv, map_entry);

        return entry2;
    }
}

// Return: the first entry.
static struct objmap_entry *__ph_objmap_create_entries(void *base, size_t len) {
    if (!__ph_objmap)
        __ph_objmap = __ph_objmap_create(__PH_OBJMAP_NR_ENTRIES);

    void *abase = (void *)__PH_GRANULE_CEIL(base);
    struct objmap_entry *ret = NULL;

    if (abase > base) {
        // Create entries per byte for the under-occupied granule below.
        for (int off = 0; base + off < abase; off++) {
            void *_tag = base + off;
            struct objmap_entry evalue = {
                .tag = _tag, .base = base, .len = len};
            void *create_res = __ph_objmap_create_entry(__ph_objmap, evalue, 0);
            if (!create_res)
                return NULL;
            if (!ret)
                ret = create_res;
        }
    }

    int goff = 0;
    for (; goff < __PH_GRANULE_CEIL(len) / __PH_GRANULE_SIZE; goff++) {
        void *_tag = abase + (goff * __PH_GRANULE_SIZE);
        struct objmap_entry evalue = {.tag = _tag, .base = base, .len = len};
        void *create_res = __ph_objmap_create_entry(__ph_objmap, evalue, 0);
        if (!create_res)
            return NULL;
        if (!ret)
            ret = create_res;
    }

    if (abase + (goff * __PH_GRANULE_SIZE) < base + len) {
        // Create entries per byte for the under-occupied granule above.
        for (void *_tag = abase + (goff * __PH_GRANULE_SIZE); _tag < base + len;
             _tag++) {
            struct objmap_entry evalue = {
                .tag = _tag, .base = base, .len = len};
            void *create_res = __ph_objmap_create_entry(__ph_objmap, evalue, 0);
            if (!create_res)
                return NULL;
            if (!ret)
                ret = create_res;
        }
    }

    return ret;
}

static struct objmap_entry *
__ph_objmap_get_entry_inner(struct objmap_entry **objmap, void *obj,
                            unsigned lv, bool pop) {
    if (!objmap)
        return NULL;
    objmap_index_t idx = __ph_objmap_index(obj, lv);
    struct objmap_entry *entry = objmap[idx];

    if (__PH_MAPENT_IS_MAP(entry)) {
        return __ph_objmap_get_entry_inner(__PH_MAPENT_GET_MAP(entry), obj,
                                           lv + 1, pop);
    } else if (entry != NULL && entry->tag == obj) {
        if (pop) objmap[idx] = NULL;
        return entry;
    } else {
        return NULL;
    }
}

static struct objmap_entry *__ph_objmap_get_entry(void *obj, bool pop) {
    if (!__ph_objmap)
        return NULL;

    struct objmap_entry *ret = NULL;

    ret = __ph_objmap_get_entry_inner(__ph_objmap, obj, 0, pop);
    return ret;
}

static bool __ph_objmap_destroy_entries(void *tag) {
    // Get the range of the current 'tag'.
    struct objmap_entry *entry = __ph_objmap_get_entry(tag, false);
    if (!entry)
        return true; // Maybe untracked object. Ignore.

    for (int goff = 0; goff < __PH_GRANULE_CEIL(entry->len) / __PH_GRANULE_SIZE;
         goff++) {
        void *aobj = entry->base + (goff * __PH_GRANULE_SIZE);
        struct objmap_entry *oent = __ph_objmap_get_entry(aobj, true);
        if (!oent)
            continue;

        // Invalidate all pointer entries associated with this object.
        struct ptrmap_entry *pent = oent->next;
        for (; pent != NULL; pent = pent->next)
            pent->len = 0;

        // Destroy 'entry'.
        __ph_objmap_entry_destroy(oent);
    }

    return true;
}

/** Internal **/

static void *__ph_objmap_create_entries_or_cleanup(void *aobj, size_t size) {
    assert((intptr_t)aobj % __PH_GRANULE_SIZE == 0);

    bool init_res = __ph_objmap_create_entries(aobj, size);
    if (!init_res) {
        if (!free_impl) {
            free_impl = dlsym(RTLD_NEXT, "free");
            if (!free_impl)
                return NULL;
        }

        free_impl(aobj);
        return NULL;
    }

    return aobj;
}

static void *__ph_malloc(size_t size) {
    if (!__ph_init_aalloc())
        return NULL;

    size_t aligned_size = __PH_GRANULE_CEIL(size);
    if (aligned_size < size)
        return NULL;

    __ph_printf("info: aalloc(%d)\n", aligned_size);
    return aalloc_impl(__PH_GRANULE_SIZE, aligned_size);
}

static void *__ph_aalloc(size_t align, size_t size) {
    if (!__ph_init_aalloc())
        return NULL;

    size_t align_min, align_max;
    if (align < __PH_GRANULE_SIZE) {
        align_min = align;
        align_max = __PH_GRANULE_SIZE;
    } else {
        align_min = __PH_GRANULE_SIZE;
        align_max = align;
    }

    size_t align_lcm = align_min;
    while (align_lcm % align_max)
        align_lcm += align_min;

    size_t aligned_size =
        ((intptr_t)(size) + align_lcm - 1) & ~(intptr_t)align_lcm;
    if (aligned_size < size)
        return NULL;

    __ph_printf("info: aalloc(%d)\n", aligned_size);
    return aalloc_impl(align_lcm, aligned_size);
}

static void __ph_free(void *ptr) {
    if (!__ph_init_free())
        return;

    __ph_printf("info: before free.\n");
    __ph_objmap_print();

    __ph_printf("info: destroy objmap entries.\n");
    bool destroy_res = __ph_objmap_destroy_entries(ptr);
    if (!destroy_res)
        return;

    __ph_printf("info: free.\n");
    free_impl(ptr);
}

/** Interface **/

__attribute__((weak)) void *malloc(size_t size) {
    __ph_printf(">>> malloc(%d)\n", size);

    void *obj = __ph_malloc(size);
    if (size == 1024)
        return obj; // FIXME: God awful stopgap (from libc)
    void *ret = __ph_objmap_create_entries_or_cleanup(obj, size);

    __ph_printf("info: alloc done. (%p)\n", ret);
    __ph_map_print();

    return ret;
}

__attribute__((weak)) void *calloc(size_t num, size_t esize) {
    __ph_printf(">>> calloc(%d, %d)\n", num, esize);

    size_t size = num * esize;
    if (size < esize)
        return NULL;

    void *obj = __ph_malloc(size);
    memset(obj, 0, size);
    void *ret = __ph_objmap_create_entries_or_cleanup(obj, size);

    __ph_printf("info: alloc done. (%p)\n", ret);
    __ph_map_print();

    return ret;
}

__attribute__((weak)) void *aligned_alloc(size_t align, size_t size) {
    __ph_printf(">>> aligned_alloc(%d, %d)\n", align, size);

    void *obj = __ph_aalloc(align, size);
    void *ret = __ph_objmap_create_entries_or_cleanup(obj, size);

    __ph_printf("info: alloc done. (%p)\n", ret);
    __ph_map_print();

    return ret;
}

__attribute__((weak)) void *realloc(void *ptr, size_t size) {
    __ph_printf(">>> realloc(%p, %d)\n", ptr, size);

    struct objmap_entry *oent = __ph_objmap_get_entry(ptr, false);
    if (oent && size <= oent->len)
        return ptr;

    bool destroy_res = __ph_objmap_destroy_entries(ptr);
    if (!destroy_res)
        return NULL;

    void *obj = __ph_malloc(size);
    memmove(obj, ptr, size);
    __ph_free(ptr);
    void *ret = __ph_objmap_create_entries_or_cleanup(obj, size);

    __ph_printf("info: alloc done. (%p)\n", ret);
    __ph_map_print();

    return ret;
}

__attribute__((weak)) void free(void *ptr) {
    __ph_printf(">>> free(%p)\n", ptr);
    __ph_free(ptr);

    __ph_printf("info: free done.\n");
    __ph_map_print();
}

__attribute__((weak)) void free_sized(void *ptr) {
    __ph_printf(">>> free_sized(%p)\n", ptr);
    __ph_free(ptr);

    __ph_printf("info: free done.\n");
    __ph_map_print();
}

__attribute__((weak)) void free_aligned_sized(void *ptr) {
    __ph_printf(">>> free_aligned_sized(%p)\n", ptr);
    __ph_free(ptr);

    __ph_printf("info: free done.\n");
    __ph_map_print();
}

static void __ph_rvalue_ptr_update_from_null(struct ptrmap_entry *ret) {
    __ph_printf(">>> __ph_rvalue_ptr_update_from_null(%p)\n", ret);
    *ret = (struct ptrmap_entry){
        .tag = __PH_MAPENT_TAG_ANON, .base = NULL, .len = 0};

    __ph_ptrmap_entry_print(0, ret);
}

// TODO: merge this with __ph_lvalue_ptr_update_from_obj if viable.
static void __ph_rvalue_ptr_update_from_obj(struct ptrmap_entry *ret, void *obj) {
    __ph_printf(">>> __ph_rvalue_ptr_update_from_obj(%p, %p)\n", ret, obj);

    if (__PH_IS_STACKADDR(obj)) {
        // Case 1: is a stack object.
        // Copy range information from the stack bulk.
        __ph_ptrmap_entry_update(ret, &__ph_stack_ptrmap_entry);
    } else {
        struct objmap_entry *oent = __ph_objmap_get_entry(obj, false);
        if (oent) {
            __ph_printf("info: from this objent entry...\n");
            __ph_objmap_entry_print(0, oent);

            // Case 2: from an objct map entry.
            // Copy range information from the object.
            __ph_ptrmap_entry_update(ret, (struct ptrmap_entry *)oent);
        } else {
            // Case 3: from an untracked object.
            // Make an untracked object at 'obj'.
            oent = __ph_objmap_create_entries(obj, __PH_UNTRACKED_OBJ_TOLERANCE);
            assert(oent);
            __ph_ptrmap_entry_update(ret, (struct ptrmap_entry *)oent);
        }
    }

    __ph_ptrmap_entry_print(0, ret);
}

static struct ptrmap_entry *
__ph_lvalue_ptr_update_from_obj(void *eetag, void *obj) {
    __ph_printf(">>> __ph_lvalue_ptr_update_from_obj(%p, %p)\n", eetag, obj);

    if (__PH_IS_STACKADDR(obj)) {
        // Case 1: is a stack object.
        // Copy range information from the stack bulk.
        return __ph_ptrmap_update_entry_by_tag(eetag, &__ph_stack_ptrmap_entry);
    } else {
        struct objmap_entry *oent = __ph_objmap_get_entry(obj, false);
        if (oent) {
            // Case 2: from an objct map entry.
            // Copy range information from the object.
            return __ph_ptrmap_update_entry_by_tag(eetag,
                                                   (struct ptrmap_entry *)oent);
        } else {
            // Case 3: from an untracked object.
            // Make an untracked object at 'obj'.
            oent = __ph_objmap_create_entries(obj, __PH_UNTRACKED_OBJ_TOLERANCE);
            assert(oent);
            return __ph_ptrmap_update_entry_by_tag(eetag,
                                                   (struct ptrmap_entry *)oent);
        }
    }
}

static struct ptrmap_entry *
__ph_lvalue_ptr_update_from_ptr(void *eetag) {
    __ph_printf(
        ">>> __ph_lvalue_ptr_update_from_ptr(%p)\n", eetag);

    struct ptrmap_entry *ret = __ph_ptrmap_get_entry(eetag);
    if (!ret) 
        return __ph_lvalue_ptr_update_from_obj(eetag, *(void **)eetag);

    __ph_ptrmap_print();

    return ret;
}

static struct ptrmap_entry *
__ph_lvalue_ptr_update_from_ptrent(void *eetag, struct ptrmap_entry *erent) {
    __ph_printf(
        ">>> __ph_lvalue_ptr_update_from_ptrent(%p, {tag: %p, base: %p, len: %d})\n",
        eetag, erent->tag, erent->base, erent->len);

    struct ptrmap_entry *ret = __ph_ptrmap_update_entry_by_tag(eetag, erent);

    __ph_ptrmap_print();

    return ret;
}

static void __ph_ptr_deref(struct ptrmap_entry *entry, void *addr, size_t size) {
    __ph_printf(">>> __ph_ptr_deref(%p, %p, %d)\n", entry, addr, size);

    __ph_printf("info: against this ptrmap entry...\n");
    __ph_ptrmap_entry_print(0, entry);

    if (!(entry->base <= addr && addr + size <= entry->base + entry->len))
        raise(SIGUSR1);
}

/** Pointer map entry stack **/

struct ptrmap_stack_frame {
    struct ptrmap_entry **args;
    size_t len;
    struct ptrmap_entry *ret;
};

// FIXME: constant depth
#define __PH_PTRMAP_STACK_DEPTH (1 << 10)

thread_local struct ptrmap_stack_frame
    __ph_ptrmap_stack[__PH_PTRMAP_STACK_DEPTH] __attribute__((weak));
thread_local unsigned __ph_ptrmap_stack_idx __attribute__((weak));

/* NOTE: the following should be implemented in the IR pass!
 * This is because '__ph_ptrmap_stack_pop' involves conditionally allocating a
 * stack object in the **caller** stack frame.

// args: should be long enough to store ptrmap_entry's in "...".
static void __ph_ptrmap_stack_push(struct ptrmap_entry **args, size_t len,
                                   ...) {
    // Non-allocator callees should update the 'ret' of this frame.
    __ph_ptrmap_stack_idx++;
    __ph_ptrmap_stack[__ph_ptrmap_stack_idx] = 
        (struct ptrmap_stack_frame){ .ret = NULL, .args = args, .len = len };

    va_list arg;
    va_start(arg, len);
    for (int i = 0; i < len; i++)
        args[i] = va_arg(arg, struct ptrmap_entry *);
    va_end(arg);

    return;
}

static unsigned __ph_ptrmap_stack_checkpt() { return __ph_ptrmap_stack_idx; }

static struct ptrmap_entry *__ph_ptrmap_stack_get(size_t pos) {
    assert(pos < __ph_ptrmap_stack[__ph_ptrmap_stack_idx].len);
    return __ph_ptrmap_stack[__ph_ptrmap_stack_idx].args[pos];
}

// ret: NULL if not expecting 'ret', otherwise '*ret' should be writable.
static struct ptrmap_entry *__ph_ptrmap_stack_pop(bool expect_ret, void *obj) {
    struct ptrmap_entry *ret;
    if (expect_ret) {
        ret = __ph_ptrmap_stack[__ph_ptrmap_stack_idx].ret;
        if (ret == NULL) {
            // Load an entry from the objmap instead.
            ret = alloca_to_prev_stack_frame(sizeof(struct ptrmap_entry));
            __ph_rvalue_ptr_update_from_obj(ret, obj);
        }
    }
    __ph_ptrmap_stack_idx--;
    return ret;
}

static void __ph_ptrmap_stack_restore(unsigned idx) {
    __ph_ptrmap_stack_idx = idx;
}

*/

// TODO: every function should be non-static to outsurvive the initial opt.
// TODO: during the path, '__ph_*' functions should all be staticized
// ("internal") to make the compiler inline them in the following re-opt.
