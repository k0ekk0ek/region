#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

static void error(const char *message)
{
  fprintf(stderr, "%s\n", message);
  exit(1);
}

// Memory mappings cannot be resized, but we can create a new mapping.
// That's exactly what is used for the copy-on-write map. So, use a sensible
// default per RR (we know the incoming amount of octets). Create a
// copy-on-write mapping using a sensible number, if the mapping fails,
// simply retry. Once we the data is in the private map and the server is
// using that temporarily, create a new mapping (if resize is required) and
// copy the modified pages over.
//
// As relative addresses are used, the data in the mapping remains valid.


int main(int argc, char *argv[])
{
  if (argc != 2)
    error("no filename specified");
  const char *name = argv[1];

  // Linux, FreeBSD and NetBSD offer memfd_create
  // FreeBSD additionally offers SHM_ANON (shm_open since FreeBSD 4.3)
  // OpenBSD offers shm_mkstemp (shm_open since OpenBSD 5.4, Nov 1, 2013)
  // Solaris 9, 10 support shm_open
  int fd = shm_open(name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
  if (fd == -1)
    error("cannot open region");

  shm_unlink(name);

  static const char hello1[] = "Hello, World!";
  static const char hello2[] = "Hello, alternate World!";
  static const size_t size = 4096;
  int flags = PROT_READ | PROT_WRITE;

  if (ftruncate(fd, size) == -1)
    error("cannot ftruncate");

  void *map1 = mmap(NULL, size, flags, MAP_SHARED, fd, 0);
  if (map1 == MAP_FAILED)
    error("cannot mmap file");

  memcpy(map1, hello1, sizeof(hello1));
  printf("map1 (%p) contains: %s\n", map1, map1);

  void *map2 = mmap(NULL, 2*size, flags, MAP_PRIVATE, fd, 0);
  if (map2 == MAP_FAILED)
    error("cannot mmap (2) file");

  printf("map2 (%p) contains: %s\n", map2, map2);

  printf("copy '%s' into map2\n", hello2);

  ftruncate(fd, 2*size);
  memset(map2, 0, 2*size);
  memcpy(map2, hello2, sizeof(hello2));
  printf("now map1 (%p) contains: %s\n", map1, map1);
  printf("now map2 (%p) contains: %s\n", map2, map2);

  printf("copy map2 to map1\n");

  memcpy(map1, map2, sizeof(hello2));
  printf("now map1 (%p) contains: %s\n", map1, map1);
  printf("now map2 (%p) contains: %s\n", map2, map2);

  return 0;
}
