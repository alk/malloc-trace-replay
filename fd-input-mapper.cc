// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "fd-input-mapper.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/mman.h>
#include <signal.h>
#include <errno.h>

Mapper::~Mapper() {}
ConstMapper::~ConstMapper() {}

FDInputMapper::FDInputMapper(int fd) : fd_(fd) {
  auto mmap_result = mmap(nullptr, 1ULL << 40,
                          PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
  if (mmap_result == MAP_FAILED) {
    perror("mmap");
    abort();
  }

  mmap_area_ = reinterpret_cast<const char*>(mmap_result);
  last_end_ = last_start_ = mmap_area_;
}

size_t FDInputMapper::Realize(const char* start, size_t len) {
  if (start > last_end_) {
    abort();
  }

  if (reinterpret_cast<uintptr_t>(start) + len < reinterpret_cast<uintptr_t>(start)) {
    abort();
  }

  intptr_t to_drop = start - last_start_;
  if (to_drop > 0) {
    const char* start2 = start - (reinterpret_cast<uintptr_t>(start) & 4095);
    mmap(const_cast<char *>(last_start_), start2 - last_start_,
         PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, 0, 0);
    last_start_ = start2;
  }

  const char* new_last_end = start + len;
  new_last_end += (~reinterpret_cast<uintptr_t>(new_last_end) + 1) & 4095;

  const char* last_end_page = last_end_ + ((~reinterpret_cast<uintptr_t>(last_end_) + 1) & 4095);

  if (last_end_page != new_last_end) {
    auto mmap_result = mmap(const_cast<char *>(last_end_page), new_last_end - last_end_page,
                            PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED|MAP_POPULATE,
                            0, 0);
    if (mmap_result == MAP_FAILED) {
      perror("mmap");
      abort();
    }
  }

  char* read_ptr = const_cast<char*>(last_end_);

  if (read_ptr < new_last_end) {
    int rv = read(fd_, read_ptr, new_last_end - read_ptr);
    if (rv < 0) {
      if (errno == EINTR) {
        rv = 0;
      } else {
        perror("read");
        abort();
      }
    } else if (rv != 0) {
      read_ptr += rv;
    }
  }

  last_end_ = read_ptr;
  return last_end_ - start;
}
