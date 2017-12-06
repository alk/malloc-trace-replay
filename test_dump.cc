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
#include <thread>
#include <future>
#include <unordered_map>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/pretty-print.h>
#include <kj/io.h>

#include "replay.capnp.h"

#include "rr-fiber.h"

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

struct ReplayFiber : public RRFiber {
public:
  ReplayFiber(const std::function<void ()>& body) : RRFiber(WrapBody(body)) {}
  ~ReplayFiber() {
    if (malloc_state != nullptr) {
      delete_malloc_state(malloc_state);
    }
  }

  static ReplayFiber* Current() {
    return static_cast<ReplayFiber*>(RRFiber::Current());
  }

  static void Yield() {
    ThreadCacheState** place = &Current()->malloc_state;
    assert(*place == nullptr);
    release_malloc_thread_cache(place);

    RRFiber::Yield();

    assert(place == &Current()->malloc_state);

    set_malloc_thread_cache(*place);
    *place = nullptr;
  }

  virtual void Join() {
    ThreadCacheState* top_malloc_state = nullptr;
    release_malloc_thread_cache(&top_malloc_state);

    RRFiber::Join();

    set_malloc_thread_cache(top_malloc_state);
  }

  ThreadCacheState* malloc_state{};

private:
  static std::function<void ()> WrapBody(const std::function<void ()>& body) {
    return [body] () {
      ThreadCacheState** place = &Current()->malloc_state;
      if (*place != nullptr) {
        set_malloc_thread_cache(*place);
        *place = nullptr;
      }
      body();
      assert(*place == nullptr);
      release_malloc_thread_cache(place);
    };
  }
};


static constexpr int kMaxRegisters = 1 << 30;
static void** registers;

// static volatile bool verbose_yields;

static void* read_register(int reg) {
  void* rv = registers[reg];

  if (PREDICT_FALSE(rv == nullptr)) {
    do {
      // if (verbose_yields) {
      //   printf("%p: yielding for register %d\n", SimpleFiber::current(), reg);
      // }
      ReplayFiber::Yield();
    } while ((rv = registers[reg]) == 0);
    // if (verbose_yields) {
    //   printf("%p: done waiting %d\n", SimpleFiber::current(), reg);
    // }
  }

  return rv;
}

static void write_register(int reg, void *val) {
  registers[reg] = val;
}

#ifdef NOOP_MALLOC

extern "C" {
  void* some_location[2];
}

#define malloc(a) ((a), (reinterpret_cast<void *>(&some_location[0])))
#define free(a) do {(void)(a);} while (0)
#define realloc(a, b) malloc(b)

#endif

static void replay_instructions(const ::capnp::List<::replay::Instruction>::Reader& instructions) {
  for (auto instr : instructions) {
    // printf("%s\n", capnp::prettyPrint(instr).flatten().cStr());
    auto reg = instr.getReg();
    switch (instr.getType()) {
    case replay::Instruction::Type::MALLOC: {
      assert(registers[reg] == nullptr);
      auto ptr = malloc(instr.getSize());
      if (ptr == nullptr) {
        abort();
      }
      write_register(reg, ptr);
      memset(ptr, 0, 8);
      break;
    }
    case replay::Instruction::Type::FREE:
      free(read_register(reg));
      write_register(reg, nullptr);
      break;
    case replay::Instruction::Type::REALLOC: {
      auto old_reg = instr.getOldReg();
      auto ptr = read_register(old_reg);

      assert(registers[old_reg] != nullptr);
      assert(registers[reg] == nullptr);

      ptr = realloc(ptr, instr.getSize());
      if (ptr == nullptr) {
        abort();
      }
      memset(ptr, 0, 8);
      write_register(reg, ptr);
      write_register(old_reg, nullptr);
      break;
    }
    case replay::Instruction::Type::MEMALLIGN: {
      assert(registers[reg] == nullptr);
      auto ptr = memalign(instr.getAlignment(), instr.getSize());
      if (ptr == nullptr) {
        abort();
      }
      write_register(reg, ptr);
      break;
    }
    default:
      abort();
    }
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

  std::unordered_map<uint64_t, ThreadCacheState*> fibers_states;

  while (input.tryGetReadBuffer() != nullptr) {
    ::capnp::PackedMessageReader message(input);
    // ::capnp::InputStreamMessageReader message(input);

    auto batch = message.getRoot<replay::Batch>();
    // dump_batch(&batch);
    auto threadsList = batch.getThreads();
    assert(threadsList.size() > 0);

    std::vector<std::unique_ptr<ReplayFiber>> fibers;
    fibers.reserve(threadsList.size());

    for (auto threadInfo : threadsList) {
      auto id = threadInfo.getThreadID();
      // printf("thread: %lld\n", (long long)id);
      auto instructions = threadInfo.getInstructions();
      total_instructions += instructions.size();

      auto this_fiber = new ReplayFiber([instructions] () {
          replay_instructions(instructions);
        });

      auto state_iter = fibers_states.find(id);
      ThreadCacheState* malloc_state = nullptr;
      if (state_iter == fibers_states.end()) {
        // printf("new thread %lld\n", (long long)id);
      } else {
        malloc_state = state_iter->second;
      }
      this_fiber->malloc_state = malloc_state;

      fibers.emplace_back(this_fiber);
    }

    for (auto &f : fibers) {
      f->Join();
    }

    for (int i = fibers.size() - 1; i >= 0; i--) {
      auto threadInfo = threadsList[i];
      auto id = threadInfo.getThreadID();
      auto this_fiber = fibers[i].get();

      // assert(threadInfo.getInstructions().size() == 0 || this_fiber->malloc_state != nullptr);
      if (threadInfo.getLive()) {
        fibers_states[id] = this_fiber->malloc_state;
        this_fiber->malloc_state = nullptr;
      } else {
        // printf("thread %lld dying\n", (long long)id);
        fibers_states.erase(id);
      }
    }

    if (total_instructions - printed_instructions > (4 << 20)) {
      uint64_t total_nanos = nanos() - nanos_start;
      printed_instructions = total_instructions;
      printf("total_instructions = %lld\nrate = %f ops/sec\n",
             (long long)total_instructions,
             (double)total_instructions * 1E9 / total_nanos);
      printf("some ~last reg: %d, live threads: %d\n",
             threadsList[0].getInstructions()[0].getReg(),
             (int)fibers_states.size());
    }
  }

  printf("processed total %lld malloc ops (aka instructions)\n",
         (long long) total_instructions);

  return 0;
}
