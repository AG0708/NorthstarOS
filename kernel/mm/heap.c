#include <northstar/mm_heap.h>

#define KHEAP_BLOCK_MAGIC UINT64_C(0x4e5348454150424c)
#define KHEAP_DEAD_MAGIC  UINT64_C(0xdead4e53424c4f43)
#define KHEAP_COOKIE_SEED UINT64_C(0x9e3779b97f4a7c15)

struct ns_kheap_block {
    uint64_t magic;
    size_t span;
    size_t requested;
    uintptr_t user_pointer;
    struct ns_kheap_block *previous;
    struct ns_kheap_block *next;
    uint8_t free;
    uint8_t reserved[7];
};

struct allocation_tag {
    uint64_t cookie;
    struct ns_kheap_block *block;
    size_t requested;
};

#define BLOCK_HEADER_SIZE                                                     \
    ((sizeof(struct ns_kheap_block) + NS_KHEAP_MIN_ALIGNMENT - 1u) &          \
     ~(size_t)(NS_KHEAP_MIN_ALIGNMENT - 1u))
#define MIN_BLOCK_SPAN                                                        \
    (BLOCK_HEADER_SIZE + sizeof(struct allocation_tag) +                      \
     NS_KHEAP_MIN_ALIGNMENT)

static struct ns_kheap *global_heap;

static void heap_lock(volatile uint32_t *lock) {
    while (__atomic_exchange_n(lock, 1u, __ATOMIC_ACQUIRE) != 0) {
        while (__atomic_load_n(lock, __ATOMIC_RELAXED) != 0) {
#if defined(__x86_64__) || defined(__i386__)
            __asm__ volatile("pause");
#elif defined(__aarch64__) || defined(__arm__)
            __asm__ volatile("yield");
#endif
        }
    }
}

static void heap_unlock(volatile uint32_t *lock) {
    __atomic_store_n(lock, 0u, __ATOMIC_RELEASE);
}

static void heap_zero(void *destination, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    for (size_t i = 0; i < length; ++i) {
        out[i] = 0;
    }
}

static void heap_copy(void *destination, const void *source, size_t length) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < length; ++i) {
        out[i] = in[i];
    }
}

static bool power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

static bool align_up_uintptr(uintptr_t value, size_t alignment,
                             uintptr_t *out) {
    const uintptr_t mask = alignment - 1u;
    if (value > UINTPTR_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

static bool align_up_size(size_t value, size_t alignment, size_t *out) {
    const size_t mask = alignment - 1u;
    if (value > SIZE_MAX - mask) {
        return false;
    }
    *out = (value + mask) & ~mask;
    return true;
}

static void initialize_free_block(struct ns_kheap_block *block, size_t span,
                                  struct ns_kheap_block *previous,
                                  struct ns_kheap_block *next) {
    block->magic = KHEAP_BLOCK_MAGIC;
    block->span = span;
    block->requested = 0;
    block->user_pointer = 0;
    block->previous = previous;
    block->next = next;
    block->free = 1;
    for (size_t i = 0; i < sizeof(block->reserved); ++i) {
        block->reserved[i] = 0;
    }
}

static uint64_t allocation_cookie(const struct ns_kheap *heap,
                                  const struct ns_kheap_block *block,
                                  uintptr_t pointer) {
    return heap->cookie ^ (uint64_t)(uintptr_t)block ^ (uint64_t)pointer;
}

static bool allocation_layout(const struct ns_kheap_block *block, size_t size,
                              size_t alignment, uintptr_t *out_pointer,
                              size_t *out_consumed) {
    const uintptr_t base = (uintptr_t)block;
    if (base > UINTPTR_MAX - BLOCK_HEADER_SIZE -
                   sizeof(struct allocation_tag)) {
        return false;
    }
    uintptr_t pointer;
    if (!align_up_uintptr(base + BLOCK_HEADER_SIZE +
                             sizeof(struct allocation_tag),
                         alignment, &pointer) ||
        pointer > UINTPTR_MAX - size) {
        return false;
    }
    size_t consumed = (size_t)((pointer + size) - base);
    if (!align_up_size(consumed, NS_KHEAP_MIN_ALIGNMENT, &consumed) ||
        consumed > block->span) {
        return false;
    }
    *out_pointer = pointer;
    *out_consumed = consumed;
    return true;
}

static void split_trailing(struct ns_kheap *heap,
                           struct ns_kheap_block *block, size_t consumed) {
    if (block->span - consumed < MIN_BLOCK_SPAN) {
        return;
    }
    struct ns_kheap_block *tail =
        (struct ns_kheap_block *)((uintptr_t)block + consumed);
    initialize_free_block(tail, block->span - consumed, block, block->next);
    if (tail->next != NULL) {
        tail->next->previous = tail;
    } else {
        heap->last = tail;
    }
    /* Realloc shrink can split immediately before an existing free block. */
    if (tail->next != NULL && tail->next->free) {
        struct ns_kheap_block *redundant = tail->next;
        tail->span += redundant->span;
        tail->next = redundant->next;
        if (tail->next != NULL) {
            tail->next->previous = tail;
        } else {
            heap->last = tail;
        }
        redundant->magic = KHEAP_DEAD_MAGIC;
    }
    block->next = tail;
    block->span = consumed;
}

static bool add_size_overflow(size_t a, size_t b, size_t *out) {
    *out = a + b;
    return *out < a;
}

static int grow_heap(struct ns_kheap *heap, size_t minimum_bytes) {
    if (minimum_bytes > SIZE_MAX - (NS_PAGE_SIZE - 1u)) {
        return NS_KHEAP_ENOMEM;
    }
    size_t growth_pages =
        (minimum_bytes + NS_PAGE_SIZE - 1u) >> NS_PAGE_SHIFT;
    if (growth_pages < heap->minimum_growth_pages) {
        growth_pages = heap->minimum_growth_pages;
    }
    if (growth_pages > SIZE_MAX / NS_PAGE_SIZE) {
        return NS_KHEAP_ENOMEM;
    }
    size_t growth = growth_pages * NS_PAGE_SIZE;
    if (growth > heap->arena_size - heap->committed_size) {
        growth = heap->arena_size - heap->committed_size;
        growth_pages = growth >> NS_PAGE_SHIFT;
    }
    if (growth < minimum_bytes || growth_pages == 0) {
        return NS_KHEAP_ENOMEM;
    }
    uintptr_t address = heap->arena_base + heap->committed_size;
    if (heap->page_ops.map_pages(heap->page_ops.context, address,
                                 growth_pages) != 0) {
        return NS_KHEAP_ENOMEM;
    }
    heap_zero((void *)address, growth);
    if (heap->last != NULL && heap->last->free) {
        heap->last->span += growth;
    } else {
        struct ns_kheap_block *block = (struct ns_kheap_block *)address;
        initialize_free_block(block, growth, heap->last, NULL);
        if (heap->last != NULL) {
            heap->last->next = block;
        } else {
            heap->first = block;
        }
        heap->last = block;
    }
    heap->committed_size += growth;
    return NS_KHEAP_OK;
}

int ns_kheap_init(struct ns_kheap *heap, uintptr_t arena_base,
                  size_t arena_size, size_t initial_pages,
                  size_t minimum_growth_pages,
                  const struct ns_kheap_page_ops *page_ops) {
    if (heap == NULL || page_ops == NULL || page_ops->map_pages == NULL ||
        arena_base == 0 || (arena_base & (NS_PAGE_SIZE - 1u)) != 0 ||
        arena_size < NS_PAGE_SIZE ||
        (arena_size & (NS_PAGE_SIZE - 1u)) != 0 || initial_pages == 0 ||
        initial_pages > arena_size / NS_PAGE_SIZE ||
        minimum_growth_pages == 0 ||
        arena_base > UINTPTR_MAX - arena_size) {
        return NS_KHEAP_EINVAL;
    }
    heap->arena_base = arena_base;
    heap->arena_size = arena_size;
    heap->committed_size = 0;
    heap->minimum_growth_pages = minimum_growth_pages;
    heap->page_ops = *page_ops;
    heap->first = NULL;
    heap->last = NULL;
    heap->cookie = KHEAP_COOKIE_SEED ^ (uint64_t)arena_base ^
                   ((uint64_t)arena_size << 17);
    heap->lock = 0;
    heap->last_error = NS_KHEAP_OK;
    heap->initialized = 1;
    int result = grow_heap(heap, initial_pages * NS_PAGE_SIZE);
    if (result != NS_KHEAP_OK) {
        heap->initialized = 0;
        heap->last_error = result;
        return result;
    }
    return NS_KHEAP_OK;
}

int ns_kheap_destroy(struct ns_kheap *heap) {
    if (heap == NULL || heap->initialized == 0) {
        return NS_KHEAP_EINVAL;
    }
    heap_lock(&heap->lock);
    for (struct ns_kheap_block *block = heap->first; block != NULL;
         block = block->next) {
        if (!block->free) {
            heap->last_error = NS_KHEAP_EBUSY;
            heap_unlock(&heap->lock);
            return NS_KHEAP_EBUSY;
        }
    }
    if (heap->page_ops.unmap_pages != NULL) {
        heap->page_ops.unmap_pages(heap->page_ops.context, heap->arena_base,
                                   heap->committed_size >> NS_PAGE_SHIFT);
    }
    heap->first = NULL;
    heap->last = NULL;
    heap->committed_size = 0;
    heap->initialized = 0;
    heap->last_error = NS_KHEAP_OK;
    heap_unlock(&heap->lock);
    return NS_KHEAP_OK;
}

void *ns_kheap_alloc_aligned(struct ns_kheap *heap, size_t size,
                             size_t alignment) {
    if (heap == NULL || heap->initialized == 0 || size == 0 ||
        !power_of_two(alignment)) {
        if (heap != NULL) {
            heap->last_error = NS_KHEAP_EINVAL;
        }
        return NULL;
    }
    if (alignment < NS_KHEAP_MIN_ALIGNMENT) {
        alignment = NS_KHEAP_MIN_ALIGNMENT;
    }
    heap_lock(&heap->lock);
    for (;;) {
        for (struct ns_kheap_block *block = heap->first; block != NULL;
             block = block->next) {
            uintptr_t pointer;
            size_t consumed;
            if (!block->free ||
                !allocation_layout(block, size, alignment, &pointer,
                                   &consumed)) {
                continue;
            }
            split_trailing(heap, block, consumed);
            block->free = 0;
            block->requested = size;
            block->user_pointer = pointer;
            struct allocation_tag *tag = (struct allocation_tag *)(
                pointer - sizeof(struct allocation_tag));
            tag->block = block;
            tag->requested = size;
            tag->cookie = allocation_cookie(heap, block, pointer);
            heap->last_error = NS_KHEAP_OK;
            heap_unlock(&heap->lock);
            return (void *)pointer;
        }
        size_t minimum;
        if (add_size_overflow(BLOCK_HEADER_SIZE +
                                  sizeof(struct allocation_tag),
                              size, &minimum) ||
            add_size_overflow(minimum, alignment, &minimum)) {
            heap->last_error = NS_KHEAP_ENOMEM;
            heap_unlock(&heap->lock);
            return NULL;
        }
        int result = grow_heap(heap, minimum);
        if (result != NS_KHEAP_OK) {
            heap->last_error = result;
            heap_unlock(&heap->lock);
            return NULL;
        }
    }
}

void *ns_kheap_alloc(struct ns_kheap *heap, size_t size) {
    return ns_kheap_alloc_aligned(heap, size, NS_KHEAP_MIN_ALIGNMENT);
}

void *ns_kheap_calloc(struct ns_kheap *heap, size_t count, size_t size) {
    if (count == 0 || size == 0 || count > SIZE_MAX / size) {
        if (heap != NULL) {
            heap->last_error = NS_KHEAP_EINVAL;
        }
        return NULL;
    }
    size_t total = count * size;
    void *pointer = ns_kheap_alloc(heap, total);
    if (pointer != NULL) {
        heap_zero(pointer, total);
    }
    return pointer;
}

static int validate_pointer(struct ns_kheap *heap, const void *pointer,
                            struct ns_kheap_block **out_block,
                            struct allocation_tag **out_tag) {
    uintptr_t address = (uintptr_t)pointer;
    if (pointer == NULL || address < heap->arena_base + BLOCK_HEADER_SIZE +
                                      sizeof(struct allocation_tag) ||
        address >= heap->arena_base + heap->committed_size) {
        return NS_KHEAP_EINVAL;
    }
    struct allocation_tag *tag = (struct allocation_tag *)(
        address - sizeof(struct allocation_tag));
    struct ns_kheap_block *block = tag->block;
    if ((uintptr_t)block < heap->arena_base ||
        (uintptr_t)block > heap->arena_base + heap->committed_size -
                               BLOCK_HEADER_SIZE ||
        block->magic != KHEAP_BLOCK_MAGIC) {
        return NS_KHEAP_ECORRUPT;
    }
    if (block->free) {
        return NS_KHEAP_EDOUBLEFREE;
    }
    if (block->user_pointer != address || block->requested != tag->requested ||
        tag->cookie != allocation_cookie(heap, block, address)) {
        return NS_KHEAP_ECORRUPT;
    }
    *out_block = block;
    *out_tag = tag;
    return NS_KHEAP_OK;
}

static struct ns_kheap_block *merge_next(struct ns_kheap *heap,
                                         struct ns_kheap_block *block) {
    struct ns_kheap_block *next = block->next;
    if (next == NULL || !next->free) {
        return block;
    }
    block->span += next->span;
    block->next = next->next;
    if (block->next != NULL) {
        block->next->previous = block;
    } else {
        heap->last = block;
    }
    next->magic = KHEAP_DEAD_MAGIC;
    return block;
}

int ns_kheap_free(struct ns_kheap *heap, void *pointer) {
    if (pointer == NULL) {
        return NS_KHEAP_OK;
    }
    if (heap == NULL || heap->initialized == 0) {
        return NS_KHEAP_EINVAL;
    }
    heap_lock(&heap->lock);
    struct ns_kheap_block *block;
    struct allocation_tag *tag;
    int result = validate_pointer(heap, pointer, &block, &tag);
    if (result != NS_KHEAP_OK) {
        heap->last_error = result;
        heap_unlock(&heap->lock);
        return result;
    }
    tag->cookie = 0;
    block->requested = 0;
    block->user_pointer = 0;
    block->free = 1;
    block = merge_next(heap, block);
    if (block->previous != NULL && block->previous->free) {
        block = merge_next(heap, block->previous);
    }
    (void)block;
    heap->last_error = NS_KHEAP_OK;
    heap_unlock(&heap->lock);
    return NS_KHEAP_OK;
}

size_t ns_kheap_usable_size(struct ns_kheap *heap, const void *pointer) {
    if (heap == NULL || pointer == NULL || heap->initialized == 0) {
        return 0;
    }
    heap_lock(&heap->lock);
    struct ns_kheap_block *block;
    struct allocation_tag *tag;
    int result = validate_pointer(heap, pointer, &block, &tag);
    size_t size = result == NS_KHEAP_OK ? block->requested : 0;
    heap->last_error = result;
    heap_unlock(&heap->lock);
    return size;
}

void *ns_kheap_realloc(struct ns_kheap *heap, void *pointer, size_t size) {
    if (pointer == NULL) {
        return ns_kheap_alloc(heap, size);
    }
    if (size == 0) {
        (void)ns_kheap_free(heap, pointer);
        return NULL;
    }
    if (heap == NULL || heap->initialized == 0) {
        return NULL;
    }
    heap_lock(&heap->lock);
    struct ns_kheap_block *block;
    struct allocation_tag *tag;
    int result = validate_pointer(heap, pointer, &block, &tag);
    if (result != NS_KHEAP_OK) {
        heap->last_error = result;
        heap_unlock(&heap->lock);
        return NULL;
    }
    size_t old_size = block->requested;
    size_t available = block->span - ((uintptr_t)pointer - (uintptr_t)block);
    if (available < size && block->next != NULL && block->next->free) {
        merge_next(heap, block);
        available = block->span - ((uintptr_t)pointer - (uintptr_t)block);
    }
    if (available >= size) {
        size_t consumed = ((uintptr_t)pointer + size) - (uintptr_t)block;
        if (align_up_size(consumed, NS_KHEAP_MIN_ALIGNMENT, &consumed)) {
            split_trailing(heap, block, consumed);
        }
        block->requested = size;
        tag->requested = size;
        tag->cookie = allocation_cookie(heap, block, (uintptr_t)pointer);
        heap->last_error = NS_KHEAP_OK;
        heap_unlock(&heap->lock);
        return pointer;
    }
    heap_unlock(&heap->lock);

    void *replacement = ns_kheap_alloc(heap, size);
    if (replacement == NULL) {
        return NULL;
    }
    heap_copy(replacement, pointer, old_size < size ? old_size : size);
    if (ns_kheap_free(heap, pointer) != NS_KHEAP_OK) {
        (void)ns_kheap_free(heap, replacement);
        return NULL;
    }
    return replacement;
}

int ns_kheap_last_error(const struct ns_kheap *heap) {
    return heap == NULL ? NS_KHEAP_EINVAL : heap->last_error;
}

void ns_kheap_get_stats(struct ns_kheap *heap, struct ns_kheap_stats *out) {
    if (heap == NULL || out == NULL || heap->initialized == 0) {
        return;
    }
    heap_lock(&heap->lock);
    heap_zero(out, sizeof(*out));
    out->arena_size = heap->arena_size;
    out->committed_size = heap->committed_size;
    for (struct ns_kheap_block *block = heap->first; block != NULL;
         block = block->next) {
        if (block->free) {
            out->free_span_bytes += block->span;
            ++out->free_blocks;
            if (block->span > out->largest_free_span) {
                out->largest_free_span = block->span;
            }
        } else {
            out->requested_bytes += block->requested;
            ++out->live_allocations;
        }
    }
    heap_unlock(&heap->lock);
}

static int check_invariants_unlocked(struct ns_kheap *heap) {
    if (heap->first == NULL || heap->last == NULL ||
        (uintptr_t)heap->first != heap->arena_base) {
        return NS_KHEAP_ECORRUPT;
    }
    uintptr_t expected = heap->arena_base;
    struct ns_kheap_block *previous = NULL;
    for (struct ns_kheap_block *block = heap->first; block != NULL;
         block = block->next) {
        if ((uintptr_t)block != expected || block->magic != KHEAP_BLOCK_MAGIC ||
            block->span < BLOCK_HEADER_SIZE ||
            (block->span & (NS_KHEAP_MIN_ALIGNMENT - 1u)) != 0 ||
            block->previous != previous ||
            (previous != NULL && previous->free && block->free) ||
            block->span > heap->arena_base + heap->committed_size - expected) {
            return NS_KHEAP_ECORRUPT;
        }
        if (!block->free &&
            (block->user_pointer <= (uintptr_t)block + BLOCK_HEADER_SIZE ||
             block->requested == 0 ||
             block->requested > (uintptr_t)block + block->span -
                                    block->user_pointer)) {
            return NS_KHEAP_ECORRUPT;
        }
        expected += block->span;
        previous = block;
    }
    if (previous != heap->last ||
        expected != heap->arena_base + heap->committed_size) {
        return NS_KHEAP_ECORRUPT;
    }
    return NS_KHEAP_OK;
}

int ns_kheap_check_invariants(struct ns_kheap *heap) {
    if (heap == NULL || heap->initialized == 0) {
        return NS_KHEAP_EINVAL;
    }
    heap_lock(&heap->lock);
    int result = check_invariants_unlocked(heap);
    heap->last_error = result;
    heap_unlock(&heap->lock);
    return result;
}

int ns_kheap_vmm_map_pages(void *context, uintptr_t address,
                           size_t page_count) {
    struct ns_kheap_vmm_provider *provider =
        (struct ns_kheap_vmm_provider *)context;
    if (provider == NULL || provider->vmm == NULL || provider->space == NULL ||
        page_count == 0 || page_count > SIZE_MAX / NS_PAGE_SIZE) {
        return NS_KHEAP_EINVAL;
    }
    return ns_vmm_alloc_map(provider->vmm, provider->space, address,
                            page_count * NS_PAGE_SIZE,
                            (provider->page_flags | NS_VMM_PAGE_WRITE |
                             NS_VMM_PAGE_GLOBAL) &
                                ~(NS_VMM_PAGE_USER | NS_VMM_PAGE_EXEC));
}

void ns_kheap_vmm_unmap_pages(void *context, uintptr_t address,
                              size_t page_count) {
    struct ns_kheap_vmm_provider *provider =
        (struct ns_kheap_vmm_provider *)context;
    if (provider == NULL || provider->vmm == NULL || provider->space == NULL ||
        page_count == 0 || page_count > SIZE_MAX / NS_PAGE_SIZE) {
        return;
    }
    (void)ns_vmm_unmap_range(provider->vmm, provider->space, address,
                             page_count * NS_PAGE_SIZE, true);
}

int ns_kheap_install_global(struct ns_kheap *heap) {
    if (heap == NULL || heap->initialized == 0) {
        return NS_KHEAP_EINVAL;
    }
    struct ns_kheap *expected = NULL;
    if (!__atomic_compare_exchange_n(&global_heap, &expected, heap, false,
                                     __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        return expected == heap ? NS_KHEAP_OK : NS_KHEAP_EBUSY;
    }
    return NS_KHEAP_OK;
}

void *kmalloc(size_t size) {
    struct ns_kheap *heap = __atomic_load_n(&global_heap, __ATOMIC_ACQUIRE);
    return heap == NULL ? NULL : ns_kheap_alloc(heap, size);
}

void *kmalloc_aligned(size_t size, size_t alignment) {
    struct ns_kheap *heap = __atomic_load_n(&global_heap, __ATOMIC_ACQUIRE);
    return heap == NULL ? NULL : ns_kheap_alloc_aligned(heap, size, alignment);
}

void *kcalloc(size_t count, size_t size) {
    struct ns_kheap *heap = __atomic_load_n(&global_heap, __ATOMIC_ACQUIRE);
    return heap == NULL ? NULL : ns_kheap_calloc(heap, count, size);
}

void *krealloc(void *pointer, size_t size) {
    struct ns_kheap *heap = __atomic_load_n(&global_heap, __ATOMIC_ACQUIRE);
    return heap == NULL ? NULL : ns_kheap_realloc(heap, pointer, size);
}

void kfree(void *pointer) {
    struct ns_kheap *heap = __atomic_load_n(&global_heap, __ATOMIC_ACQUIRE);
    if (heap != NULL) {
        (void)ns_kheap_free(heap, pointer);
    }
}
