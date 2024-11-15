/*
 * region.h
 *
 * Copyright (c) 2024, NLnet Labs. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#ifndef REGION_H
#define REGION_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "macros.h"

// databases reside in mmaped regions. the region allocator is designed to
// manage a single mmaped region to support copy-on-write regions. as
// relative addressing is used to access objects, modified pages can be
// copied back to persist changes. the region allocator cannot resize
// the region and does not manage synchronization.

// in context:
//   when a zone transfer comes in, mmap a copy-on-write (MAP_PRIVATE) region.
//   the allocator is automatically cloned because the same physical pages are
//   used. decide if the mapping must be larger initially. start applying
//   changes, if it turns out the region is not sufficiently large enough,
//   discard the changes, map a larger region and try again. as the allocator
//   is embedded in the region, unmapping the region releases all resources.

//
// * Bonwick, Jeff: "The Slab Allocator: An Object-Caching Kernel Memory
//   Allocator", Summer 1994 Usenix Conference, pp. 87-98.
//
// https://www.usenix.org/conference/usenix-summer-1994-technical-conference/slab-allocator-object-caching-kernel
//
//
// * Bonwick, Jeff & Adams, Jonathan: "Magazines and Vmem: Extending
//   the Slab Allocator to Many CPU's and Arbitrary Resources", USENIX
//   2001, pp 15-34.
//
// https://www.usenix.org/conference/2001-usenix-annual-technical-conference/magazines-and-vmem-extending-slab-allocator-many
//

// opaque definition of a region, internals hidden on purpose.
// allocator is embedded in the region, the address of the region is therefore
// the address of the allocator.
typedef struct region region_t;


bool is_object(const region_t *region, intptr_t object);

// determine the fixed address for an object allocated in the region (swizzle)
nonnull((1))
static always_inline void *swizzle(const region_t *region, intptr_t object)
{
//  assert(is_object(region, object));
  return (void*)((uintptr_t)region + (uintptr_t)object);
}

nonnull((1))
static always_inline intptr_t unswizzle(const region_t *region, void *pointer)
{
//  assert((uintptr_t)region < (uintptr_t)pointer);
  const intptr_t object = (intptr_t)((uintptr_t)pointer - (uintptr_t)region);
//  assert(is_object(region, object));
  return object;
}

// contract:
// * require a minimum size of sizeof(region_t)
// * require a page-aligened address
// * require at least ... pages for caches
region_t *region_init(void *address, size_t size);

nonnull((1))
warn_unused_result
intptr_t region_alloc(region_t *region, size_t size);

// kmem_free requires size to be specified, see:
// https://docs.oracle.com/cd/E36784_01/html/E36886/kmem-free-9f.html
// nsd does to, see:
// region-allocator.h
nonnull((1))
void region_free(region_t *region, intptr_t object);

#if 0
// a given cache is valid inside a given region and can only be used in
// conjunction with that region, never without. the maximum number of caches
// is hard coded because space is reserved in the region. the returned value
// is an identifier for the cache.
//
// * constructor/destruction interfaces may prove to be unnecessary
// * move interface in the second paper is likely useful
//
// caches for nsd_region_cache_alloc (5):
// * node4, takes 48 bytes   (16 + 4 + (4*8), keys fit cache)
// * node16, takes 152 bytes (16 + 16 + (16*8), keys fit cache)
// * node32, takes 304 bytes (16 + 32 + (32*8), keys fit cache)
// * node48
// * node256, takes more than 2k.
//
nonnull((1))
intptr_t region_cache_create(
  region_t *region,
  const char *name,
  size_t object_size,
  size_t object_align,
  void(*constructor)(void *, size_t),
  void(*destructor)(void *, size_t));

nonnull((1))
void region_cache_destroy(
  region_t *region, intptr_t cache);

nonnull((1))
intptr_t region_cache_alloc(
  region_t *region, intptr_t cache);

nonnull((1))
void region_cache_free(
  region_t *region, intptr_t cache, intptr_t object);
#endif

#endif // REGION_H
