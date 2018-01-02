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
#include <signal.h>

#include <atomic>
#include <vector>
#include <unordered_map>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/pretty-print.h>
#include <kj/io.h>

#include "instruction.h"

// #include "replay2.capnp.h"

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

static void replay_instruction(const Instruction& i) {
  // printf("%s\n", capnp::prettyPrint(instruction).flatten().cStr());
  auto reg = i.reg;
  switch (i.type) {
  case Instruction::Type::MALLOC: {
    auto ptr = malloc(i.malloc.size);
    if (ptr == nullptr) {
      abort();
    }
    registers[reg] = ptr;
    memset(ptr, 0, 8);
    break;
  }
  case Instruction::Type::FREE: {
    free(registers[reg]);
    registers[reg] = nullptr;
    break;
  }
  case Instruction::Type::MEMALIGN: {
    auto ptr = memalign(i.malloc.align, i.malloc.size);
    registers[reg] = ptr;
    memset(ptr, 0, 8);
    break;
  }
  case Instruction::Type::REALLOC: {
    auto old_reg = reg;
    auto new_reg = i.realloc.new_reg;
    auto ptr = realloc(registers[old_reg], i.realloc.new_size);
    if (ptr == nullptr) {
      abort();
    }
    registers[old_reg] = nullptr;
    registers[new_reg] = ptr;
    break;
  }
  case Instruction::Type::FREE_SIZED: {
    free(registers[reg]);
    registers[reg] = nullptr;
    break;
  }
  case Instruction::Type::KILL_THREAD:
    handle_kill_thread();
    break;
  case Instruction::Type::SWITCH_THREAD: {
    handle_switch_thread(i.switch_thread.thread_id);
    break;
  }
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

// static unsigned char buffer_space[128 << 10] __attribute__((aligned(4096)));

// extern "C" void dump_batch(::replay::Batch::Reader* reader) {
//   printf("%s\n", capnp::prettyPrint(*reader).flatten().cStr());
// }

// static int roughly_last_reg(const capnp::List<replay::Instruction>::Reader& instructions) {
//   size_t i = instructions.size() - 1;
//   for (; i >= 0; i--) {
//     auto instr = instructions[i];
//     switch (instr.which()) {
//     case replay::Instruction::Which::MALLOC:
//       return instr.getMalloc().getReg();
//     case replay::Instruction::Which::MEMALIGN:
//       return instr.getMemalign().getReg();
//     case replay::Instruction::Which::FREE:
//       return instr.getFree().getReg();
//     }
//   }
//   return -1;
// }

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
#endif

  signal(SIGINT, [](int dummy) {
      exit(0);
    });

  printf("will%s use set/release_malloc_thread_cache\n",
         set_malloc_thread_cache_ptr ? "" : " not");

  // ::kj::FdInputStream fd0(fd);
  // ::kj::BufferedInputStreamWrapper input(
  //   fd0,
  //   kj::arrayPtr(buffer_space, sizeof(buffer_space)));
  FILE* input = fdopen(fd, "r");
  setvbuf(input, 0, _IOFBF, 256 << 10);

  uint64_t nanos_start = nanos();
  uint64_t printed_instructions = 0;
  uint64_t total_instructions = 0;

  // std::unordered_map<uint64_t, ThreadCacheState*> fibers_states;

  // capnp::ReaderOptions options;
  // options.traversalLimitInWords = 256 << 20;

  Instruction instr(0);
  while (fread_unlocked(&instr, 1, sizeof(instr), input)) {
  // while (input.tryGetReadBuffer() != nullptr) {
  //   ::capnp::PackedMessageReader message(input, options);
  //   // ::capnp::InputStreamMessageReader message(input);

  //   auto batch = message.getRoot<replay::Batch>();
  //   // dump_batch(&batch);
  //   auto instructions = batch.getInstructions();

  //   for (auto instr : instructions) {
  //     total_instructions++;
  //     replay_instruction(instr);
  //   }

    replay_instruction(instr);

    total_instructions += 1;

    if (total_instructions - printed_instructions > (4 << 20)) {
      uint64_t total_nanos = nanos() - nanos_start;
      printed_instructions = total_instructions;
      printf("\rtotal_instructions = %lld; rate = %f ops/sec; live threads: %d         \b\b\b\b\b\b\b\b\b",
             (long long)total_instructions,
             (double)total_instructions * 1E9 / total_nanos,
             (int)thread_states.size());
      fflush(stdout);
    }
  }

  printf("\nprocessed total %lld malloc ops (aka instructions)\n",
         (long long) total_instructions);

  return 0;
}
