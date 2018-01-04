// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <assert.h>

#include <unistd.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <sched.h>
#include <dlfcn.h>
#include <signal.h>

#include <atomic>
#include <vector>
#include <unordered_map>

#include "instruction.h"

#include "fd-input-mapper.h"

#ifdef __GNUC__
#define PREDICT_TRUE(x) __builtin_expect(!!(x), 1)
#define PREDICT_FALSE(x) __builtin_expect(!!(x), 0)
#else
#define PREDICT_TRUE(x) (x)
#define PREDICT_FALSE(x) (x)
#endif

struct ThreadCacheState;

extern "C" {
  bool release_malloc_thread_cache(ThreadCacheState** place_state_here) __attribute__((weak));
  bool set_malloc_thread_cache(ThreadCacheState* state) __attribute__((weak));
}

static auto release_malloc_thread_cache_ptr = release_malloc_thread_cache;
static auto set_malloc_thread_cache_ptr = set_malloc_thread_cache;

static void maybe_release_malloc_thread_cache(ThreadCacheState** place_state_here) {
  if (release_malloc_thread_cache_ptr) {
    release_malloc_thread_cache_ptr(place_state_here);
  }
}

static void maybe_set_malloc_thread_cache(ThreadCacheState* state) {
  if (set_malloc_thread_cache_ptr) {
    set_malloc_thread_cache_ptr(state);
  }
}

#define release_malloc_thread_cache(a) maybe_release_malloc_thread_cache(a)
#define set_malloc_thread_cache(a) maybe_set_malloc_thread_cache(a)

extern "C" void MallocExtension_GetStats(char* buffer, int buffer_length) __attribute__((weak));

extern "C" void dump_malloc_stats() {
  static char buffer[128 << 10];
  MallocExtension_GetStats(buffer, sizeof(buffer));
  printf("%s\n", buffer);
}


static constexpr int kMaxRegisters = 1 << 30;
static void** registers;

#ifdef NOOP_MALLOC

extern "C" {
  void* some_location[2];
}

#define malloc(a) ((a), (reinterpret_cast<void *>(&some_location[0])))
#define free(a) do {(void)(a);} while (0)
#define realloc(a, b) malloc(b)
#define memalign(a, b) malloc(b)

#endif

static std::unordered_map<uint64_t, ThreadCacheState*> thread_states;
static constexpr uint64_t kInvalidThreadId = static_cast<uint64_t>(int64_t{-1});
static uint64_t current_thread_id = kInvalidThreadId;

static void delete_malloc_state(ThreadCacheState* malloc_state) {
  ThreadCacheState *old{};
  release_malloc_thread_cache(&old);
  set_malloc_thread_cache(malloc_state);
  set_malloc_thread_cache(old);
}

static void handle_switch_thread(uint64_t thread_id) {
  assert(current_thread_id != thread_id);
  assert(thread_id != kInvalidThreadId);

  ThreadCacheState* ts;
  release_malloc_thread_cache(&ts);
  thread_states[current_thread_id] = ts;

  current_thread_id = thread_id;
  ts = thread_states[current_thread_id];
  if (ts != nullptr) {
    set_malloc_thread_cache(ts);
  }
}

static void handle_kill_thread() {
  assert(current_thread_id != kInvalidThreadId);
  ThreadCacheState* ts = thread_states[current_thread_id];
  thread_states.erase(current_thread_id);
  if (ts != nullptr) {
    delete_malloc_state(ts);
  }
  current_thread_id = kInvalidThreadId;
}

extern "C" void tc_free_sized(void *, size_t);

size_t allocated_count;

static void replay_instruction(const Instruction& i) {
  auto reg = i.reg;
  switch (i.type) {
  case Instruction::Type::MALLOC: {
    auto ptr = malloc(i.malloc.size);
    //printf("%lld = malloc(%lld)\n", (long long)reg, (long long)(i.malloc.size));
    if (ptr == nullptr) {
      abort();
    }
    if (registers[reg] != nullptr) {
      printf("reg = %d\n", (int)reg);
      asm volatile ("int $3");
    }
    assert(registers[reg] == nullptr);
    registers[reg] = ptr;
    if (i.malloc.size != 0) {
      memset(ptr, 0, 8);
    }
    allocated_count++;
    break;
  }
  case Instruction::Type::FREE: {
    if (registers[reg] == nullptr) {
      printf("reg = %d\n", (int)reg);
      asm volatile ("int $3");
    }
    assert(registers[reg] != nullptr);
    free(registers[reg]);
    registers[reg] = nullptr;
    allocated_count--;
    break;
  }
  case Instruction::Type::MEMALIGN: {
    allocated_count++;
    auto ptr = memalign(i.malloc.align, i.malloc.size);
    if (ptr == nullptr) {
      abort();
    }
    registers[reg] = ptr;
    memset(ptr, 0, 8);
    break;
  }
  case Instruction::Type::REALLOC: {
    auto old_reg = reg;
    auto new_reg = i.realloc.new_reg;
    assert(registers[old_reg] != nullptr);
    auto ptr = realloc(registers[old_reg], i.realloc.new_size);
    if (ptr == nullptr) {
      abort();
    }
    registers[old_reg] = nullptr;
    registers[new_reg] = ptr;
    break;
  }
  case Instruction::Type::FREE_SIZED: {
    assert(registers[reg] != nullptr);
    allocated_count--;
#if 0
    tc_free_sized(registers[reg], i.malloc.size);
#else
    free(registers[reg]);
#endif
    registers[reg] = nullptr;
    break;
  }
  case Instruction::Type::KILL_THREAD:
    handle_kill_thread();
    break;
  case Instruction::Type::SWITCH_THREAD:
    handle_switch_thread(i.switch_thread.thread_id);
    break;
  case Instruction::Type::SET_TS_CPU:
    break; // do nothing
  default:
    abort();
  }
}

static uint64_t nanos() {
  struct timeval tv;
  int rv = gettimeofday(&tv, nullptr);
  if (rv != 0) {
    perror("gettimeofday");
    abort();
  }
  return (tv.tv_usec + uint64_t{1000000} * tv.tv_sec) * uint64_t{1000};
}

static void mmap_registers() {
  size_t pagesize = getpagesize();
  size_t size_needed = (sizeof(void *) * kMaxRegisters + pagesize - 1) & ~(pagesize - 1);
  auto mmap_result = mmap(0, size_needed + pagesize,
                          PROT_READ|PROT_WRITE,
                          MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE,
                          0, 0);
  if (mmap_result == MAP_FAILED) {
    perror("mmap");
    abort();
  }
  auto registers_ptr = static_cast<char *>(mmap_result);
  int rv = mprotect(registers_ptr + size_needed, pagesize, PROT_NONE);
  if (rv != 0) {
    perror("mprotect");
    abort();
  }
  registers = static_cast<void **>(mmap_result);
}

static void setup_malloc_state_fns() {
  if (getenv("NO_THREAD_CACHE_SWITCH")) {
    release_malloc_thread_cache_ptr = nullptr;
    set_malloc_thread_cache_ptr = nullptr;
    return;
  }
  void *handle = dlopen(NULL, RTLD_LAZY);
  if (!release_malloc_thread_cache_ptr) {
    auto v = dlsym(handle, "release_malloc_thread_cache");
    release_malloc_thread_cache_ptr = reinterpret_cast<bool (*)(ThreadCacheState**)>(v);
  }
  if (!set_malloc_thread_cache) {
    auto v = dlsym(handle, "set_malloc_thread_cache");
    set_malloc_thread_cache_ptr = reinterpret_cast<bool (*)(ThreadCacheState*)>(v);
  }
}

char buffer[16 << 20];
char *buf_ptr = buffer;
char *buf_end = buffer;

const int kMinReadAmount = 1024;
const int kMinBufSizeToRebase = kMinReadAmount;

static void refill_buffer(int fd) {
  if (buffer + sizeof(buffer) - buf_end < kMinBufSizeToRebase) {
    int amount = buf_end - buf_ptr;
    memcpy(buffer, buf_ptr, amount);
    buf_ptr = buffer;
    buf_end = buf_ptr + amount;
  }
  int rv;
  int total_read = 0;
  do {
    rv = read(fd, buf_end, buffer + sizeof(buffer) - buf_end);
    if (rv < 0) {
      if (errno == EINTR) {
        continue;
      }
      perror("read");
      abort();
    }
    if (rv == 0) {
      break;
    }
    total_read += rv;
    buf_end += rv;
  } while (total_read < kMinReadAmount);
}

static inline __attribute__((always_inline)) bool buffer_read(int fd, void* ptr, size_t amount) {
  if (buf_ptr + amount > buf_end) {
    refill_buffer(fd);
    if (buf_ptr + amount > buf_end) {
      return false;
    }
  }
  memcpy(ptr, buf_ptr, amount);
  buf_ptr += amount;
  return true;
}

static void WriteStringToFile(const std::string& s, const std::string& filename) {
  FILE* fp = fopen(filename.c_str(), "w");
  fwrite(s.data(), 1, s.length(), fp);
  fclose(fp);
}

extern "C" __attribute__((weak)) char *MallocExtension_GetHeapSample_malloced();

int main(int argc, char **argv) {
  mmap_registers();
  setup_malloc_state_fns();

  int fd = 0;

  if (argc > 1) {
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
      perror("open");
      abort();
    }
  }

  // if our input is pipe from some decompressor, larger pipe buffer
  // is good idea
#ifdef F_SETPIPE_SZ
  fcntl(fd, F_SETPIPE_SZ, 1 << 20);

  fcntl(fd, F_SETPIPE_SZ, 4 << 20);

  fcntl(fd, F_SETPIPE_SZ, 32 << 20);
#endif

  signal(SIGINT, [](int dummy) {
      exit(0);
    });

  printf("will%s use set/release_malloc_thread_cache\n",
         set_malloc_thread_cache_ptr ? "" : " not");

  uint64_t nanos_start = nanos();
  uint64_t printed_instructions = 0;
  uint64_t total_instructions = 0;

  Instruction next_instr(0);
  bool ok = buffer_read(fd, &next_instr, sizeof(Instruction));
  if (!ok) abort();
  Instruction instr(0);
  while (true) {
    memcpy(&instr, &next_instr, sizeof(instr));
    bool ok = buffer_read(fd, &next_instr, sizeof(Instruction));
    if (!ok) {
      break;
    }
    if (next_instr.type == Instruction::Type::MALLOC
        || next_instr.type == Instruction::Type::FREE
        || next_instr.type == Instruction::Type::FREE_SIZED
        || next_instr.type == Instruction::Type::MEMALIGN
        || next_instr.type == Instruction::Type::REALLOC) {
      __builtin_prefetch(registers + next_instr.reg, 0, 3);
    }

    replay_instruction(instr);

    total_instructions += 1;

    if (total_instructions - printed_instructions > (4 << 20)) {
      uint64_t total_nanos = nanos() - nanos_start;
      printed_instructions = total_instructions;
      printf("\rtotal_instructions = %lld; rate = %f ops/sec; live threads: %d; ~last reg: %d         \b\b\b\b\b\b\b\b\b",
             (long long)total_instructions,
             (double)total_instructions * 1E9 / total_nanos,
             (int)thread_states.size(),
             (int)next_instr.reg);
      fflush(stdout);
    }
  }

  printf("\nprocessed total %lld malloc ops (aka instructions)\n",
         (long long) total_instructions);

  printf("allocated still: %llu\n", (unsigned long long)allocated_count);

  char* heap_sample = MallocExtension_GetHeapSample_malloced();
  WriteStringToFile(heap_sample, "dump.heap");
  return 0;
}
