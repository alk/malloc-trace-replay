// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <time.h>
#include <sys/mman.h>
#include <signal.h>

#include "events-serializer.h"

static void pabort(const char *t) {
  perror(t);
  abort();
}

int main(int argc, char **argv) {
  int fd = 0;
  if (argc > 1) {
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
      pabort("open");
    }
  }

  struct stat st;
  int rv = fstat(fd, &st);
  if (rv < 0) {
    pabort("fstat");
  }

  auto mmap_result = mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
  if (mmap_result == MAP_FAILED) {
    pabort("mmap");
  }


}
