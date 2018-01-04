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
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "varint_codec.h"
#include "malloc_trace_encoder.h"
#include "fd-input-mapper.h"

typedef tcmalloc::VarintCodec VarintCodec;
typedef tcmalloc::EventsEncoder EventsEncoder;


static void panic(const char *reason) {
  fprintf(stderr, "%s\n", reason);
  abort();
}

static void pabort(const char* place) {
  perror(place);
  abort();
}

class MemStream {
public:
  explicit MemStream(Mapper* mapper);
  virtual ~MemStream() {}

  inline uint64_t must_read_varint();
  inline const char* advance_by(uint64_t amount);

  bool has_more() {
    if (ptr_ < end_) {
      return true;
    }
    realize_more(0);
    return ptr_ < end_;
  }

  bool try_advance_by(uint64_t amount, const char** place) {
    permissive_advance_ = true;
    const char* old_ptr = advance_by(amount);
    permissive_advance_ = false;
    if (ptr_ > end_) {
      ptr_ = end_;
      return false;
    }
    *place = old_ptr;
    return true;
  }

  inline bool try_read_varint(uint64_t* place);

  const char* get_ptr() {
    return ptr_;
  }

  void unrealize_up_to(const char* ptr);

private:
  const uint64_t kRealizeSize = 16 << 20;
  const size_t kMaxVarintSize = 10;

  bool has_at_least_varint() {
    return ptr_ + kMaxVarintSize <= end_;
  }

  bool realize_more(uint64_t min_amount);
  uint64_t read_varint_slow();
  const char* advance_by_slow(uint64_t amount);

  const char *ptr_;
  const char *end_;
  const char *realized_start_;
  const char *const begin_;
  Mapper* const mapper_;
  bool permissive_advance_{};
};

MemStream::MemStream(Mapper* mapper) : begin_(mapper->GetBegin()), mapper_(mapper) {
  realized_start_ = end_ = ptr_ = begin_;
}

bool MemStream::realize_more(uint64_t min_amount) {
  size_t realize_amount = ptr_ - realized_start_ + std::max(min_amount, kRealizeSize);
  size_t got_amount = mapper_->Realize(realized_start_, realize_amount);

  end_ = realized_start_ + got_amount;
  return (uint64_t(end_ - ptr_) >= min_amount);
}

inline const char* MemStream::advance_by(uint64_t amount) {
  const char* retval = ptr_;

  if (uint64_t(end_ - ptr_) >= amount) {
    ptr_ += amount;
    return retval;
  }
  return advance_by_slow(amount);
}

inline const char* MemStream::advance_by_slow(uint64_t amount) {
  const char* retval = ptr_;
  bool ok = realize_more(amount);

  if (!ok) {
    if (!permissive_advance_) {
      panic("trying to realize with min_amount beyond end of data");
    }
    ptr_ = end_ + 1;
    return retval;
  }

  ptr_ += amount;
  return retval;
}

inline uint64_t MemStream::must_read_varint() {
  if (!has_at_least_varint()) {
    return read_varint_slow();
  }
  VarintCodec::DecodeResult<uint64_t> res = VarintCodec::decode_unsigned(ptr_);
  advance_by(res.advance);
  return res.value;
}

inline bool MemStream::try_read_varint(uint64_t* place) {
  if (has_at_least_varint()) {
    VarintCodec::DecodeResult<uint64_t> res = VarintCodec::decode_unsigned(ptr_);
    advance_by(res.advance);
    *place = res.value;
    return true;
  }

  permissive_advance_ = true;
  uint64_t result = read_varint_slow();
  permissive_advance_ = false;

  if (ptr_ > end_) {
    ptr_ = end_;
    return false;
  }

  *place = result;
  return true;
}

uint64_t MemStream::read_varint_slow() {
  realize_more(0);
  if (has_at_least_varint()) {
    return must_read_varint();
  }
  // auto page_end_gap = (~reinterpret_cast<uintptr_t>(ptr_) + 1) & 4095;
  // if (page_end_gap <= kMaxVarintSize) {
  //   return must_read_varint();
  // }
  char tmp[kMaxVarintSize];
  memcpy(tmp, ptr_, end_ - ptr_);
  VarintCodec::DecodeResult<uint64_t> res = VarintCodec::decode_unsigned(tmp);
  advance_by(res.advance); // will abort if we've read past eof
  return res.value;
}

static const int kUnrealizeStep = 128 << 20;

void MemStream::unrealize_up_to(const char* ptr) {
  assert(ptr >= realized_start_);
  if (ptr - realized_start_ < kUnrealizeStep) {
    return;
  }

  ptr -= reinterpret_cast<uintptr_t>(ptr) & 4096;
  size_t got_amount = mapper_->Realize(ptr, end_ - ptr);
  if (ptr + got_amount < end_) {
    abort();
  }
  ptr = realized_start_;
  end_ = realized_start_ + got_amount;
}

class ConstMapperHolder {
public:
  ConstMapperHolder(const char* begin, const char* end)
    : const_mapper_(begin, end - begin) {
  }
  virtual ~ConstMapperHolder() {};
protected:
  ConstMapper const_mapper_;
};

class ConstMemStream : public ConstMapperHolder, public MemStream {
public:
  ConstMemStream(const char* begin, const char* end)
    : ConstMapperHolder(begin, end), MemStream(&const_mapper_) {
  }
};

struct OuterEvent {
  uint8_t type;
  uint32_t cpu;
  uint64_t ts;
  uint64_t thread_id;
  const char *buf_start;
  const char *buf_end;
};

struct BufEvent {
  uint64_t ts;
  uint64_t cpu;
  uint64_t thread_id;
  uint64_t size;
};

class EvCounter : public MemStream {
public:
  EvCounter(Mapper* mapper) : MemStream(mapper) {}
  ~EvCounter() {}

  size_t mallocs{};
  size_t frees{};
  size_t sized_frees{};
  size_t other{};

  bool process_one();
};

bool EvCounter::process_one() {
  if (!has_more()) {
    return false;
  }

  uint64_t first_word;
  uint64_t second_word;
  uint64_t third_word;
  bool ok = try_read_varint(&first_word);
  if (!ok) {
    return false;
  }
  unsigned evtype = EventsEncoder::decode_type(first_word);

  int extra_words = 0;

  switch (evtype) {
  case EventsEncoder::kEventDeath:
    extra_words = 1;
    break;
  case EventsEncoder::kEventBuf:
    extra_words = 2;
    break;

  case EventsEncoder::kEventEnd:
    fprintf(stderr, "found end\n");
    return false;
    break;
  case EventsEncoder::kEventSyncBarrier:
    extra_words = 1;
    break;
  case EventsEncoder::kEventMalloc:
    mallocs++;
    break;
  case EventsEncoder::kEventFree:
    frees++;
    break;
  case EventsEncoder::kEventFreeSized:
    extra_words = 1;
    sized_frees++;
    break;
  case EventsEncoder::kEventTok:
    extra_words = 1;
    break;
  case EventsEncoder::kEventRealloc:
    extra_words = 1;
    other++;
    break;
  case EventsEncoder::kEventMemalign:
    extra_words = 1;
    other++;
    break;
  default:
    printf("unknown type: %u\n", evtype);
    abort();
  }

  if (extra_words > 0) {
    ok = try_read_varint(&second_word);
    if (!ok) {
      return false;
    }
    if (extra_words > 1) {
      assert(extra_words == 2);
      ok = try_read_varint(&third_word);
      if (!ok) {
        return false;
      }
    }
  }

  unrealize_up_to(get_ptr());

  return true;
}


ConstMapper mmap_mapper(int fd) {
  struct stat st;
  int rv = fstat(fd, &st);
  if (rv < 0) {
    pabort("fstat");
  }

  auto pagesize = getpagesize();
  auto mmap_size = ((st.st_size + pagesize - 1) & ~(pagesize - 1)) + pagesize;
  auto mmap_result = mmap(nullptr, mmap_size,
                          PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
  if (mmap_result == MAP_FAILED) {
    pabort("mmap");
  }
  const char* mmap_area = static_cast<const char *>(mmap_result);
  mmap_result = mmap(mmap_result, st.st_size,
                     PROT_READ, MAP_SHARED|MAP_FIXED, fd, 0);
  if (mmap_result == MAP_FAILED) {
    pabort("mmap");
  }

  rv = madvise(mmap_result, st.st_size, MADV_SEQUENTIAL);
  if (rv < 0) {
    pabort("madvise");
  }
  return ConstMapper(mmap_area, st.st_size);
}

int main(int argc, char **argv) {
  int fd = 0;

  ConstMapper m{mmap_mapper(fd)};
  // FDInputMapper m(fd);
  EvCounter counter(&m);
  while (counter.process_one()) {
    // nothing
  }
  printf("mallocs = %zu\n", counter.mallocs);
  printf("frees = %zu\n", counter.frees);
  printf("sized_frees = %zu\n", counter.sized_frees);
  printf("other = %zu\n", counter.other);
}
