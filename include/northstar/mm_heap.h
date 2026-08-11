#ifndef NORTHSTAR_MM_HEAP_H
#define NORTHSTAR_MM_HEAP_H

#include <northstar/mm_vmm.h>

#define NS_KHEAP_MIN_ALIGNMENT 16u

enum ns_kheap_error {
    NS_KHEAP_OK = 0,
    NS_KHEAP_EINVAL = -1,
    NS_KHEAP_ENOMEM = -2,
    NS_KHEAP_ECORRUPT = -3,
    NS_KHEAP_EDOUBLEFREE = -4,
    NS_KHEAP_EBUSY = -5
};

typedef int (*ns_kheap_map_pages_fn)(void *context, uintptr_t address,
                                     size_t page_count);
typedef void (*ns_kheap_unmap_pages_fn)(void *context, uintptr_t address,
                                        size_t page_count);

struct ns_kheap_page_ops {
    void *context;
    ns_kheap_map_pages_fn map_pages;
    ns_kheap_unmap_pages_fn unmap_pages;
};

struct ns_kheap_vmm_provider {
    struct ns_vmm *vmm;
    struct ns_vmm_space *space;
    uint64_t page_flags;
};

struct ns_kheap_block;

struct ns_kheap {
    uintptr_t arena_base;
    size_t arena_size;
    size_t committed_size;
    size_t minimum_growth_pages;
    struct ns_kheap_page_ops page_ops;
    struct ns_kheap_block *first;
    struct ns_kheap_block *last;
    uint64_t cookie;
    volatile uint32_t lock;
    int last_error;
    uint32_t initialized;
};

struct ns_kheap_stats {
    size_t arena_size;
    size_t committed_size;
    size_t requested_bytes;
    size_t free_span_bytes;
    size_t largest_free_span;
    size_t live_allocations;
    size_t free_blocks;
};

/* page callbacks execute while the heap lock is held and must not recurse. */
int ns_kheap_init(struct ns_kheap *heap, uintptr_t arena_base,
                  size_t arena_size, size_t initial_pages,
                  size_t minimum_growth_pages,
                  const struct ns_kheap_page_ops *page_ops);
int ns_kheap_destroy(struct ns_kheap *heap);

void *ns_kheap_alloc(struct ns_kheap *heap, size_t size);
void *ns_kheap_alloc_aligned(struct ns_kheap *heap, size_t size,
                             size_t alignment);
void *ns_kheap_calloc(struct ns_kheap *heap, size_t count, size_t size);
void *ns_kheap_realloc(struct ns_kheap *heap, void *pointer, size_t size);
int ns_kheap_free(struct ns_kheap *heap, void *pointer);
size_t ns_kheap_usable_size(struct ns_kheap *heap, const void *pointer);
int ns_kheap_last_error(const struct ns_kheap *heap);
void ns_kheap_get_stats(struct ns_kheap *heap, struct ns_kheap_stats *out);
int ns_kheap_check_invariants(struct ns_kheap *heap);

int ns_kheap_vmm_map_pages(void *context, uintptr_t address,
                           size_t page_count);
void ns_kheap_vmm_unmap_pages(void *context, uintptr_t address,
                              size_t page_count);

/* Conventional kernel-global facade, installed exactly once during boot. */
int ns_kheap_install_global(struct ns_kheap *heap);
void *kmalloc(size_t size);
void *kmalloc_aligned(size_t size, size_t alignment);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *pointer, size_t size);
void kfree(void *pointer);

#endif
