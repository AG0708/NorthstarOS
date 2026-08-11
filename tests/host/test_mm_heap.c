#include <northstar/mm_heap.h>

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#define ARENA_SIZE (2u * 1024u * 1024u)
#define WORKER_COUNT 4u
#define WORKER_SLOTS 32u
#define WORKER_ROUNDS 6000u

static _Alignas(4096) uint8_t arena[ARENA_SIZE];

struct page_backend {
    size_t mapped_pages;
    size_t map_calls;
    size_t unmap_calls;
};

static int map_pages(void *context, uintptr_t address, size_t pages) {
    struct page_backend *backend = context;
    assert(address == (uintptr_t)arena + backend->mapped_pages * NS_PAGE_SIZE);
    assert(pages <= ARENA_SIZE / NS_PAGE_SIZE - backend->mapped_pages);
    backend->mapped_pages += pages;
    ++backend->map_calls;
    return 0;
}

static void unmap_pages(void *context, uintptr_t address, size_t pages) {
    struct page_backend *backend = context;
    assert(address == (uintptr_t)arena);
    assert(pages == backend->mapped_pages);
    ++backend->unmap_calls;
    backend->mapped_pages = 0;
}

static void test_allocation_semantics(struct ns_kheap *heap) {
    assert(ns_kheap_alloc(heap, 0) == NULL);
    assert(ns_kheap_calloc(heap, SIZE_MAX, 2) == NULL);
    assert(ns_kheap_alloc_aligned(heap, 16, 24) == NULL);

    void *small = ns_kheap_alloc(heap, 31);
    void *cacheline = ns_kheap_alloc_aligned(heap, 257, 64);
    void *page = ns_kheap_alloc_aligned(heap, 8193, 4096);
    assert(small != NULL && cacheline != NULL && page != NULL);
    assert(((uintptr_t)small & 15u) == 0);
    assert(((uintptr_t)cacheline & 63u) == 0);
    assert(((uintptr_t)page & 4095u) == 0);
    assert(ns_kheap_usable_size(heap, small) == 31);
    assert(ns_kheap_usable_size(heap, cacheline) == 257);
    assert(ns_kheap_usable_size(heap, page) == 8193);
    memset(small, 0xa5, 31);
    memset(cacheline, 0x5a, 257);
    memset(page, 0x3c, 8193);
    assert(ns_kheap_check_invariants(heap) == NS_KHEAP_OK);

    assert(ns_kheap_destroy(heap) == NS_KHEAP_EBUSY);
    assert(ns_kheap_free(heap, cacheline) == NS_KHEAP_OK);
    assert(ns_kheap_free(heap, small) == NS_KHEAP_OK);
    assert(ns_kheap_free(heap, page) == NS_KHEAP_OK);
    assert(ns_kheap_check_invariants(heap) == NS_KHEAP_OK);

    void *zeroed = ns_kheap_calloc(heap, 100, 13);
    assert(zeroed != NULL);
    for (size_t i = 0; i < 1300; ++i) {
        assert(((uint8_t *)zeroed)[i] == 0);
    }
    assert(ns_kheap_free(heap, zeroed) == NS_KHEAP_OK);

    /* Immediate double-free is diagnosed without mutating allocator state. */
    void *double_free = ns_kheap_alloc(heap, 48);
    assert(double_free != NULL);
    assert(ns_kheap_free(heap, double_free) == NS_KHEAP_OK);
    assert(ns_kheap_free(heap, double_free) == NS_KHEAP_EDOUBLEFREE);
    assert(ns_kheap_check_invariants(heap) == NS_KHEAP_OK);
}

static void test_reallocation_and_growth(struct ns_kheap *heap,
                                         struct page_backend *backend) {
    uint8_t *pointer = ns_kheap_alloc(heap, 128);
    assert(pointer != NULL);
    for (size_t i = 0; i < 128; ++i) {
        pointer[i] = (uint8_t)(i ^ 0x6d);
    }
    pointer = ns_kheap_realloc(heap, pointer, 100000);
    assert(pointer != NULL);
    for (size_t i = 0; i < 128; ++i) {
        assert(pointer[i] == (uint8_t)(i ^ 0x6d));
    }
    assert(backend->map_calls > 1);
    pointer = ns_kheap_realloc(heap, pointer, 64);
    assert(pointer != NULL);
    for (size_t i = 0; i < 64; ++i) {
        assert(pointer[i] == (uint8_t)(i ^ 0x6d));
    }
    assert(ns_kheap_free(heap, pointer) == NS_KHEAP_OK);
    assert(ns_kheap_alloc(heap, ARENA_SIZE * 2u) == NULL);
    assert(ns_kheap_last_error(heap) == NS_KHEAP_ENOMEM);
    assert(ns_kheap_check_invariants(heap) == NS_KHEAP_OK);
}

struct worker_context {
    struct ns_kheap *heap;
    uint32_t seed;
};

static uint32_t next_random(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void *allocator_worker(void *opaque) {
    struct worker_context *context = opaque;
    void *slots[WORKER_SLOTS] = {0};
    uint32_t state = context->seed;
    for (size_t round = 0; round < WORKER_ROUNDS; ++round) {
        uint32_t random = next_random(&state);
        size_t slot = random % WORKER_SLOTS;
        if (slots[slot] != NULL) {
            assert(ns_kheap_free(context->heap, slots[slot]) == NS_KHEAP_OK);
            slots[slot] = NULL;
        } else {
            size_t size = ((random >> 8) % 768u) + 1u;
            size_t alignment = (size_t)16u << ((random >> 20) & 3u);
            slots[slot] = ns_kheap_alloc_aligned(context->heap, size,
                                                  alignment);
            assert(slots[slot] != NULL);
            memset(slots[slot], (int)(random & 0xffu), size);
        }
    }
    for (size_t slot = 0; slot < WORKER_SLOTS; ++slot) {
        if (slots[slot] != NULL) {
            assert(ns_kheap_free(context->heap, slots[slot]) == NS_KHEAP_OK);
        }
    }
    return NULL;
}

static void test_concurrent_stress(struct ns_kheap *heap) {
    pthread_t threads[WORKER_COUNT];
    struct worker_context contexts[WORKER_COUNT];
    for (size_t i = 0; i < WORKER_COUNT; ++i) {
        contexts[i].heap = heap;
        contexts[i].seed = (uint32_t)(0x1234567u + i * 0x10203u);
        assert(pthread_create(&threads[i], NULL, allocator_worker,
                              &contexts[i]) == 0);
    }
    for (size_t i = 0; i < WORKER_COUNT; ++i) {
        assert(pthread_join(threads[i], NULL) == 0);
    }
    assert(ns_kheap_check_invariants(heap) == NS_KHEAP_OK);
    struct ns_kheap_stats stats;
    ns_kheap_get_stats(heap, &stats);
    assert(stats.live_allocations == 0);
    assert(stats.free_blocks == 1);
    assert(stats.largest_free_span == stats.committed_size);
}

int main(void) {
    struct page_backend backend = {0};
    struct ns_kheap heap;
    struct ns_kheap_page_ops ops = {
        .context = &backend,
        .map_pages = map_pages,
        .unmap_pages = unmap_pages,
    };
    assert(ns_kheap_init(&heap, (uintptr_t)arena, sizeof(arena), 1, 2,
                         &ops) == NS_KHEAP_OK);
    test_allocation_semantics(&heap);
    test_reallocation_and_growth(&heap, &backend);
    test_concurrent_stress(&heap);

    assert(ns_kheap_install_global(&heap) == NS_KHEAP_OK);
    uint8_t *global = kmalloc_aligned(777, 128);
    assert(global != NULL && ((uintptr_t)global & 127u) == 0);
    memset(global, 0x19, 777);
    global = krealloc(global, 2048);
    assert(global != NULL);
    for (size_t i = 0; i < 777; ++i) {
        assert(global[i] == 0x19);
    }
    kfree(global);
    uint8_t *global_zero = kcalloc(32, 8);
    assert(global_zero != NULL);
    for (size_t i = 0; i < 256; ++i) {
        assert(global_zero[i] == 0);
    }
    kfree(global_zero);

    assert(ns_kheap_destroy(&heap) == NS_KHEAP_OK);
    assert(backend.unmap_calls == 1);
    assert(backend.mapped_pages == 0);
    puts("test_mm_heap: ok");
    return 0;
}
