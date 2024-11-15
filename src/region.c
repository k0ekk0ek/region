/*
 * region.c - region based memory allocator
 *
 * Copyright (c) 2024, NLnet Labs. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#include <assert.h>
#include <string.h>

#include "macros.h"
#include "region.h"

// hardware page size is typically 4096 bytes, though increasing the virtual
// size makes it more efficient for large objects. e.g., for 512 byte objects,
// a single page slab is on the small side. depending on requirements,
// consider increasing virtual page size to 16384?
#define PAGE_SIZE (4096llu)
#define PAGE_MASK (~(PAGE_SIZE - 1))

#if 0
struct object {
  /** Offset of next dirty / << only valid in the dirty/free lists */
  uintptr_t next;
};
#endif

struct object_list {
  uintptr_t list;
  size_t count; // << this can be a 16-bit number
};

#if 0
// large objects require bookkeeping too. return the offset (address) from
// where the user can start writing data. prepend bookkeeping information to
// the object and perhaps a signature to detect memory errors

struct large_object {
  // * signature? (error detection)
  // * size
  // * previous, next
  // * dirty
};
#endif

struct page {
  /** Offset of next page. */
  uintptr_t next;
  // * signature? (error detection)
  // * statistics
};

struct slab {
  /** Page header. */
  struct page page;
  /** Offset of cache to which slab belongs. */
  uintptr_t cache;
  /** Offset of list to which slab belongs (full_slabs, free_slabs, etc). */
  uintptr_t list;
  /** Offset of next slab that belongs to the same cache+list. */
  uintptr_t next;
  /** Offset where objects start from. */
  uintptr_t objects;
  struct object_list free_objects;
};

struct slab_list {
  uintptr_t list;
  size_t count;
};

struct cache {
  char name[16];
  struct slab_list full_slabs;
  struct slab_list partial_slabs;
  struct slab_list free_slabs;
  /** Object size for cache. */
  uint16_t object_size;
  /** Boundary to align cache objects on (always a multiple of 8). */
  uint16_t alignment;
  /** Aligned object size for cache. */
  size_t aligned_size;
  /** Number of objects that fit in a slab. */
  size_t object_count;
  // * constructor / destructor interfaces are not required (yet)
  // * statistics
};

// cache and heap pages are located apart to allow for large objects. slab
// pages are allocated from the head, heap pages are allocated from the tail.
// to determine if an object is allocated from a slab or the heap, checking
// the range it falls into works if the region is fixed. unfortunately,
// regions may need to be resized (and remapped), meaning the contiguous
// memory space becomes segmented. to conveniently determine if a page is
// managed as a slab or as heap memory, the allocator maintains two bitsets
// where each bit represents a page. if the corresponding bit for a page is
// not set, the page is free.
//
// the bitsets are located in the first page while the region is small enough.
// if a region is resized and the number of bits required to cover the entire
// region exceeds the (otherwise unused) space available in the first page,
// pages are reserved from the tail. the bitsets are never exposed to the user
// and are therefore safe to move.
//
// using bitsets allows for flexibile use of pages and does not force
// allocating segments, or lineair allocation of pages.
struct bitset {
  uintptr_t bits;
  size_t size;
};

// region will reside at the start of the memory map so that:
// * everything is automatically cleaned up when mmap is discarded
// * copy-on-write copies (or any copies) are automatically initialized
// * multiple copies are easy to maintain
// * addresses that overlap with the administration are always invalid
// * address of mapped region automatically points to the administration

struct region {
  size_t size;
#if 0
  /** List of pages that have been updated. */
  uintptr_t dirty_pages;
  /** List of large objects that have been updated. */
  uintptr_t dirty_objects;
#endif
  uintptr_t pages;
  // pointer to first free page (avoid unnecessary scanning)
  // FIXME: transform into a circular buffer or similar to improve
  //        performance in scenarios where a low order page is released
  //        after allocating a high order page
  uintptr_t free_page;

  // (binary?) heap is maintained with the region, but we have to account for
  // multiple non-contiguous segments
  struct {
    struct bitset bitset;
    // implement
  } heap;

  // region reserves space for a predefined set of caches
  struct {
    struct bitset bitset;
    size_t count;
    struct cache cache[20];
  } caches;
};


// map small object sizes to caches. use next power of two for now.
static const uint8_t alloc_size_index[] = {
   0, /*   8 */     1, /*  16 */     2, /*  24 */     2, /*  32 */
   3, /*  40 */     3, /*  48 */     3, /*  56 */     3, /*  64 */
   4, /*  72 */     4, /*  80 */     4, /*  88 */     4, /*  96 */
   4, /* 104 */     4, /* 112 */     4, /* 120 */     4, /* 128 */
   5, /* 136 */     5, /* 144 */     5, /* 152 */     5, /* 160 */
   5, /* 168 */     5, /* 176 */     5, /* 184 */     5, /* 192 */
   5, /* 200 */     5, /* 208 */     5, /* 216 */     5, /* 224 */
   5, /* 232 */     5, /* 240 */     5, /* 248 */     5  /* 256 */
};

struct alloc_cache {
  const char *name;
  size_t size;
  size_t align;
};

static const struct alloc_cache alloc_caches[] = {
  { "region_alloc-8",     8, 8 },
  { "region_alloc-16",   16, 8 },
  { "region_alloc-32",   32, 8 },
  { "region_alloc-64",   64, 8 },
  { "region_alloc-128", 128, 8 },
  { "region_alloc-256", 256, 8 }
};

nonnull((1))
static always_inline void set_bit(
  region_t *region, uintptr_t bits, size_t size, size_t bit)
{
  uint8_t *ptr = swizzle(region, bits);
  ptr[ (bit >> 3) ] |= (1u << ((bit ^ 7) & 0xfu));
}

nonnull((1))
static always_inline uint8_t get_bit(
  const region_t *region, uintptr_t bits, size_t size, size_t bit)
{
  const uint8_t *ptr = swizzle(region, bits);
  return ptr[ (bit >> 3) ] & (1u << ((bit ^ 7) & 0xfu));
}


static size_t aligned_size(size_t size, size_t align)
{
  if (align == 0)
    align = 8;

  if (align > size)
    return align;

  return align * ((size + (align - 1)) / align);
}

static intptr_t cache_init(
  struct region *region, const char *name, size_t size, size_t align)
{
  // FIXME: must not add duplicates, check (same name)

  const size_t id = region->caches.count++;
  struct cache *cache = &region->caches.cache[id];

  const size_t slab_space = PAGE_SIZE - sizeof(struct slab);
  size_t name_length = strlen(name);
  if (name_length >= sizeof(cache->name))
    name_length = sizeof(cache->name) - 1;

  memset(cache, 0, sizeof(*cache));
  memcpy(cache->name, name, name_length);
  cache->object_size = size;
  cache->alignment = align;
  cache->aligned_size = aligned_size(size, align);
  cache->object_count = slab_space / cache->aligned_size;

  return id;
}

struct region *region_init(void *address, size_t size)
{
  // region must be page aligned
  if (((uintptr_t)address & PAGE_MASK) != (uintptr_t)address)
    return NULL;

  size_t pages = ((sizeof(struct region) + PAGE_SIZE) / PAGE_SIZE) * PAGE_SIZE;
  size_t caches = sizeof(alloc_caches) / sizeof(alloc_caches[0]);
  size_t size_pages = size / PAGE_SIZE;

  // size must be a multiple of page size and sufficiently large enough
  if ((size & PAGE_MASK) != size || size < pages || size_pages <= caches)
    return NULL;

  struct region *region = address;

  // bitmap size required to track heap and slab pages (aligned to 8 bytes)
  size_t bitmap_size = ((size / PAGE_SIZE) + 7) & (size_t)-8;
  // space available for bitmaps is page size minus space required for
  // region administration divided by two (cache and heap bitmaps)
  size_t unused_space = (PAGE_SIZE - sizeof(struct region)) >> 1;
  if (bitmap_size < unused_space) {
    region->heap.bitset.bits = PAGE_SIZE - (bitmap_size << 1);
    region->heap.bitset.size = bitmap_size;
    region->caches.bitset.bits = PAGE_SIZE - bitmap_size;
    region->caches.bitset.size = bitmap_size;
  } else {
    // a sensible number of pages must be available for data
    size_t bitmap_pages = ((bitmap_size << 1) + (PAGE_SIZE - 1)) / PAGE_SIZE;
    if (bitmap_pages + caches >= size_pages)
      return NULL;
    region->heap.bitset.bits = size - (PAGE_SIZE * bitmap_pages);
    region->heap.bitset.size = bitmap_size;
    region->caches.bitset.bits = size - ((PAGE_SIZE * bitmap_pages) >> 2);
    region->caches.bitset.size = bitmap_size;
  }

  region->size = size;
  region->caches.count = 0;
  region->pages = region->free_page = pages;

  // initialize small object caches
  for (size_t index=0; index < caches; index++) {
    const struct alloc_cache *cache = &alloc_caches[index];
    (void)cache_init(region, cache->name, cache->size, cache->align);
  }

  return region;
}

nonnull_all
static uintptr_t allocate_page(struct region *region)
{
  // check if a free (lowest to highest) page is available
  const uintptr_t page = region->free_page;

  if (!page)
    return 0;

  assert((page & PAGE_MASK) == page);
  assert(region->heap.bitset.size == region->caches.bitset.size);

  // determine offset relative to heap and cache bases and scan forward in
  // 64 bit blocks (64 pages)
  // FIXME: improve using vectorization or roaring bitmaps?

  size_t bit = page / PAGE_SIZE;
  size_t block = (bit + 1) & (size_t)-64;
  const size_t last_block = region->heap.bitset.size & (size_t)-64;
  uint64_t bits = 0;
  // ignore current page
  if (bit > block)
    bits = 1llu << ((bit ^ 63) & 63);

  const uint64_t *heap_bits = swizzle(region, region->heap.bitset.bits);
  const uint64_t *cache_bits = swizzle(region, region->caches.bitset.bits);

  // pages are allocated to heap or cache, never both
  assert(!(heap_bits[block] & cache_bits[block]));
  bits |= heap_bits[block] | cache_bits[block];
  block++;

  for (; bits == (uint64_t)-1 && block <= last_block; block++) {
    assert(!(heap_bits[block] & cache_bits[block]));
    bits = heap_bits[block] | cache_bits[block];
  }

  // ignore unavailable pages
  if (block == last_block + 1)
    bits |= 1llu << ((region->heap.bitset.size ^ 63) & 63);

  if (bits != (uint64_t)-1)
    region->free_page = (block + __builtin_ctzll(~bits)) * PAGE_SIZE;
  else
    region->free_page = 0;

  return page;
}

nonnull((1,2))
static uintptr_t allocate_slab(region_t *region, struct cache *cache)
{
  uintptr_t slab_offset;
  if (!(slab_offset = allocate_page(region)))
    return 0;

  uint8_t *bits = swizzle(region, region->caches.bitset.bits);
  size_t bit = slab_offset / PAGE_SIZE;

  assert(bit < region->caches.bitset.size);
  bits[ bit / 8 ] |= (1 << (7 - bit % 8));


  struct slab *slab = swizzle(region, slab_offset);
  memset((uint8_t *)slab + sizeof(uintptr_t), 0, PAGE_SIZE - sizeof(uintptr_t));

  // slab
  slab->cache = unswizzle(region, cache);
  slab->list = unswizzle(region, &cache->free_slabs);
  slab->next = cache->free_slabs.list;
  slab->objects = slab_offset + (PAGE_SIZE - ((cache->object_count - 1) * cache->aligned_size));
  slab->free_objects.list = slab->objects;
  slab->free_objects.count = cache->object_count;

  // objects
  uintptr_t object = slab->objects + ((cache->object_count - 1) * cache->aligned_size);
  uintptr_t next_object = 0u;
  for (; object > slab->objects; next_object = object, object -= cache->aligned_size)
    memcpy(swizzle(region, object), &next_object, sizeof(object));

  assert(object == slab->objects);

  // cache
  cache->free_slabs.list = slab_offset;
  cache->free_slabs.count++;

  return slab_offset;
}

nonnull((1))
static intptr_t cache_alloc(region_t *region, size_t index, size_t size)
{
  assert(region);
  assert(index < sizeof(region->caches.count));

  struct slab *slab;
  struct cache *cache = &region->caches.cache[index];
  uintptr_t slab_offset, object_offset;

  if (cache->partial_slabs.list) {
    slab_offset = cache->partial_slabs.list;
    slab = swizzle(region, slab_offset);
    // move to full slabs if depleted
    if (slab->free_objects.count == 1) {
      cache->partial_slabs.count--;
      cache->partial_slabs.list = slab->next;
      slab->list = slab->next = cache->full_slabs.list;
      cache->full_slabs.count++;
      cache->full_slabs.list = slab_offset;
    }
  } else {
    if (!cache->free_slabs.list && !allocate_slab(region, cache))
      return 0;
    assert(cache->free_slabs.count && cache->free_slabs.list);
    slab_offset = cache->free_slabs.list;
    slab = swizzle(region, slab_offset);
    // remove from free slab list
    cache->free_slabs.count--;
    cache->free_slabs.list = slab->next;
    if (slab->free_objects.count == 1) {
      slab->list = unswizzle(region, &cache->full_slabs);
      slab->next = cache->full_slabs.list;
      cache->full_slabs.count++;
      cache->full_slabs.list = slab_offset;
    } else {
      slab->list = unswizzle(region, &cache->partial_slabs);
      slab->next = cache->partial_slabs.list;
      cache->partial_slabs.count++;
      cache->partial_slabs.list = slab_offset;
    }
  }

  slab->free_objects.count--;
  object_offset = slab->free_objects.list;
  memcpy(&slab->free_objects.list, swizzle(region, object_offset), sizeof(uintptr_t));

//  mark_page(region, slab_offset);

  return (intptr_t)object_offset;
}

nonnull((1))
static always_inline void
cache_free(region_t *region, size_t index, intptr_t object)
{
  assert(region);
  assert(index < sizeof(region->caches.count));

  const uintptr_t slab_offset = object & PAGE_MASK;
  const uintptr_t next_page = slab_offset + PAGE_SIZE;
  struct slab *slab = swizzle(region, slab_offset);
  struct cache *cache = &region->caches.cache[index];
  assert((uintptr_t)swizzle(region, slab->cache) == (uintptr_t)cache);

  const size_t uintptr_size = sizeof(object);

  // detect double free
  for (uintptr_t free_object = slab->free_objects.list; free_object; ) {
    assert(free_object != object);
    assert(free_object < next_page);
    memcmp(&free_object, swizzle(region, free_object), uintptr_size);
  }

  memcpy(swizzle(region, object), &slab->free_objects.list, uintptr_size);
  slab->free_objects.list = object;
  slab->free_objects.count++;

  if (slab->free_objects.count != cache->object_count)
    return;

  if (cache->partial_slabs.list == slab_offset) {
    cache->partial_slabs.count--;
    cache->partial_slabs.list = slab->next;
    assert(!cache->partial_slabs.count == !cache->partial_slabs.list);
  }

  slab->next = cache->free_slabs.list;
  cache->free_slabs.count++;
  memcpy(&cache->free_slabs.list, &slab_offset, uintptr_size);
}

nonnull((1))
static always_inline bool is_heap_object(
  const region_t *region, intptr_t object)
{
  const size_t bit = object & PAGE_MASK;
  return 0 != get_bit(
    region, region->caches.bitset.bits, region->caches.bitset.size, bit);
}

nonnull((1))
static always_inline bool is_cache_object(
  const region_t *region, intptr_t object)
{
  const size_t bit = object & PAGE_MASK;
  return 0 != get_bit(
    region, region->heap.bitset.bits, region->heap.bitset.size, bit);
}

nonnull((1))
bool is_object(const region_t *region, intptr_t object)
{
  assert(region);
  if (object <= region->pages || object >= region->size)
    return false;
  // objects are aligned to 8 bytes
  if (object & 0x7u)
    return false;
  // object page must be allocated to cache or heap
  if (!is_cache_object(region, object) && !is_heap_object(region, object))
    return false;
  return true;
}

nonnull((1))
static always_inline intptr_t object_cache(
  const region_t *region, intptr_t object)
{
  const struct slab *slab = swizzle(region, object & PAGE_MASK);
  const size_t cache_offset = slab->cache;
  const struct cache *cache = swizzle(region, cache_offset);
  uintptr_t x = (uintptr_t)cache - (uintptr_t)&region->caches.cache[0];
  x = x / sizeof(region->caches.cache[0]);
  return x;//slab->cache;
}

#if 0
// the heap will grow from the tail in our case. large objects are uncommon
// in dns, but in theory, an RR may contain as much as 65535 bytes of data.
nonnull((1))
static intptr_t heap_alloc(region_t *region, size_t size)
{
  // * verify size indeed exceeds small object size
  // * at least align on 8 bytes
  // * reserve entire pages
}

nonnull((1))
static void heap_free(region_t *region, intptr_t object)
{
  // implement
}
#endif

// non-caching allocation routines use object caches internally for object
// sizes ranging from 8 bytes to 256 bytes in roughly 10-20% increments. a
// best-fit heap allocator is used for large objects.

static always_inline bool is_small_object_size(size_t size)
{
  return size <= 256;
}

// objects have a minimum size of sizeof(void*) bytes. an object is opaque
// when allocated, but contains a pointer to the next free object otherwise.
// the last available object in a slab is zeroed to indicate the slab is
// depleted

static always_inline size_t small_object_cache(size_t size)
{
  assert(size <= 256);
  return alloc_size_index[ (size - 1) >> 3 ];
}

intptr_t region_alloc(region_t *region, size_t size)
{
  assert(region);

  if (is_small_object_size(size)) {
    if (size == 0)
      return 0;
    const size_t index = small_object_cache(size);
    return cache_alloc(region, index, size);
#if 0
  } else {
    return heap_alloc(region, size);
#endif
  }
}

void region_free(region_t *region, intptr_t object)
{
  assert(region);

  if (object <= region->pages || object >= region->size)
    return;
  if (object & 0x7u)
    return;

  if (is_cache_object(region, object))
    cache_free(region, object_cache(region, object), object);
#if 0
  else if (is_heap_object(region, object))
    heap_free(region, object);
#endif
}

#if 0
region_cache_init()
{
  // * align must be a multiple of 8
}

intptr_t region_cache_alloc(region_t *region, intptr_t cache)
{
  assert(region);

  //if (cache >= sizeof(region->caches.cache / region->caches.cache[0]))
  //  return -1; // invalid cache
  //if (cache >= region->caches.count)
  //  return -1; // invalid cache

  return cache_alloc(region, cache, 0);
}

void region_cache_free(region_t *region, intptr_t cache, intptr_t object)
{
  assert(region);
  // * check cache is valid
  // * check object resides in region
}
#endif
