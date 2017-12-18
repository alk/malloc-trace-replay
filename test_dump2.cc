// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// #include <utility>
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

#include <atomic>
#include <vector>
#include <unordered_map>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/pretty-print.h>
#include <kj/io.h>

#include "replay2.capnp.h"

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


static void delete_malloc_state(ThreadCacheState* malloc_state) {
  ThreadCacheState *old{};
  release_malloc_thread_cache(&old);
  set_malloc_thread_cache(malloc_state);
  set_malloc_thread_cache(old);
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

#endif

static void replay_instruction(const replay::Instruction::Reader& instruction) {
  switch (instruction.which()) {
  case replay::Instruction::Which::MALLOC: {
    auto m = instruction.getMalloc();
    auto ptr = malloc(m.getSize());
    if (ptr == nullptr) {
      abort();
    }
    registers[m.getReg()] = ptr;
    memset(ptr, 0, 8);
    break;
  }
  case replay::Instruction::Which::FREE: {
    auto f = instruction.getFree();
    auto reg = f.getReg();
    free(registers[reg]);
    registers[reg] = nullptr;
    break;
  }
  case replay::Instruction::Which::MEMALIGN: {
    auto m = instruction.getMemalign();
    auto ptr = memalign(m.getAlignment(), m.getSize());
    registers[m.getReg()] = ptr;
    memset(ptr, 0, 8);
    break;
  }
  case replay::Instruction::Which::REALLOC: {
    auto r = instruction.getRealloc();
    auto old_reg = r.getOldReg();
    auto new_reg = r.getNewReg();
    auto ptr = realloc(registers[old_reg], r.getSize());
    if (ptr == nullptr) {
      abort();
    }
    registers[old_reg] = nullptr;
    registers[new_reg] = ptr;
    break;
  }
  case replay::Instruction::Which::FREE_SIZED: {
    auto f = instruction.getFreeSized();
    auto reg = f.getReg();
    free(registers[reg]);
    registers[reg] = nullptr;
    break;
  }
  case replay::Instruction::Which::KILL_THREAD:
    break; //TODO
  case replay::Instruction::Which::SWITCH_THREAD:
    // TODO
    break;
  default:
    abort();
  }
}

uint64_t nanos() {
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

static unsigned char buffer_space[128 << 10] __attribute__((aligned(4096)));

extern "C" void dump_batch(::replay::Batch::Reader* reader) {
  printf("%s\n", capnp::prettyPrint(*reader).flatten().cStr());
}

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

  printf("will%s use set/release_malloc_thread_cache\n",
         set_malloc_thread_cache_ptr ? "" : " not");

  ::kj::FdInputStream fd0(fd);
  ::kj::BufferedInputStreamWrapper input(
    fd0,
    kj::arrayPtr(buffer_space, sizeof(buffer_space)));

  uint64_t nanos_start = nanos();
  uint64_t printed_instructions = 0;
  uint64_t total_instructions = 0;

  // std::unordered_map<uint64_t, ThreadCacheState*> fibers_states;

  while (input.tryGetReadBuffer() != nullptr) {
    ::capnp::PackedMessageReader message(input);
    // ::capnp::InputStreamMessageReader message(input);

    auto batch = message.getRoot<replay::Batch>();
    // dump_batch(&batch);
    auto instructions = batch.getInstructions();
    if (instructions.size() == 0) {
      puts("0 size instructions!\n");
    }
    // assert(instructions.size() > 0);

    for (auto instr : instructions) {
      total_instructions++;
      replay_instruction(instr);
    }

    if (total_instructions - printed_instructions > (4 << 20)) {
      uint64_t total_nanos = nanos() - nanos_start;
      printed_instructions = total_instructions;
      printf("total_instructions = %lld\nrate = %f ops/sec\n",
             (long long)total_instructions,
             (double)total_instructions * 1E9 / total_nanos);
      // printf("some ~last reg: %d, live threads: %d\n",
      //        threadsList[0].getInstructions()[0].getReg(),
      //        (int)fibers_states.size());
    }
  }

  printf("processed total %lld malloc ops (aka instructions)\n",
         (long long) total_instructions);

  return 0;
}
