/*
 * alloc.c
 *
 * Copyright (c) 2024, NLnet Labs. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 */
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <inttypes.h>

#include "region.h"

static void error(const char *message)
{
  fprintf(stderr, "%s\n", message);
  exit(1);
}

int main(int argc, char *argv[])
{
  int fd = shm_open("shm", O_CREAT|O_RDWR, S_IRUSR|S_IWUSR);
  if (fd == -1)
    error("cannot open shared memory");

  shm_unlink("test");

  int flags = PROT_READ|PROT_WRITE;
  size_t size = 4096 * 20;

  if (ftruncate(fd, size) == -1)
    error("cannot truncate shared memory");

  void *address = mmap(NULL, size, flags, MAP_SHARED, fd, 0);
  if (address == MAP_FAILED)
    error("cannot map shared memory");

  region_t *region = region_init(address, size);
  if (region == NULL)
    error("cannot initialize region");

  const char foobar[] = "foobar";
  const char foobaz[] = "foobaz";

  intptr_t object = region_alloc(region, sizeof(foobar));
  if (object == 0)
    error("cannot allocate object in region");

  char *foobar_copy = swizzle(region, object);
  memcpy(foobar_copy, foobar, sizeof(foobar));

  printf("foobar object: %" PRId64 ", string: %s\n", object, foobar_copy);

  region_free(region, object);
  if (!(object = region_alloc(region, sizeof(foobaz))))
    error("cannot allocate object in region");

  char *foobaz_copy = swizzle(region, object);
  memcpy(foobaz_copy, foobaz, sizeof(foobaz));

  printf("foobaz object: %" PRId64 ", string: %s\n", object, foobaz_copy);

  return 0;
}
