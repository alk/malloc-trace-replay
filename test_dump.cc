// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
// #include <utility>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <assert.h>

#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sched.h>

#include <atomic>
#include <vector>
#include <thread>
#include <future>

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include <capnp/pretty-print.h>
#include <kj/io.h>

#include "replay.capnp.h"

#include "rr-fiber.h"

#ifdef HAVE_BUILTIN_EXPECT
#define PREDICT_TRUE(x) __builtin_expect(!!(x), 1)
#define PREDICT_FALSE(x) __builtin_expect(!!(x), 0)
#else
#define PREDICT_TRUE(x) (x)
#define PREDICT_FALSE(x) (x)
#endif


static unsigned char buffer_space[128 << 10] __attribute__((aligned(4096)));

static uintptr_t registers[1024 << 10];

static constexpr int kSpins = 1024;

static volatile bool verbose_yields;

static void *read_register(int reg) {
  uintptr_t rv = registers[reg];

  if (PREDICT_FALSE(rv == 0)) {
    do {
      if (verbose_yields) {
        printf("%p: yielding for register %d\n", SimpleFiber::current(), reg);
      }
      RRFiber::Yield();
    } while ((rv = registers[reg]) == 0);
    if (verbose_yields) {
      printf("%p: done waiting %d\n", SimpleFiber::current(), reg);
    }
  }

  return reinterpret_cast<void *>(rv);
}

static void write_register(int reg, void *val) {
  registers[reg] = reinterpret_cast<uintptr_t>(val);
}

static void replay_instructions(const ::capnp::List<::replay::Instruction>::Reader& instructions) {
  for (auto instr : instructions) {
    // printf("%s\n", capnp::prettyPrint(instr).flatten().cStr());
    auto reg = instr.getReg();
    switch (instr.getType()) {
    case replay::Instruction::Type::MALLOC: {
      assert(registers[reg] == 0);
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
      auto ptr = read_register(reg);
      ptr = realloc(ptr, instr.getSize());
      if (ptr == nullptr) {
        abort();
      }
      write_register(reg, ptr);
      break;
    }
    case replay::Instruction::Type::MEMALLIGN: {
      assert(registers[reg] == 0);
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

int main(int argc, char **argv) {
  int fd = 0;

  if (argc > 1) {
    fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
      perror("open");
      abort();
    }
  }

  ::kj::FdInputStream fd0(fd);
  ::kj::BufferedInputStreamWrapper input(
    fd0,
    kj::arrayPtr(buffer_space, sizeof(buffer_space)));

  uint64_t nanos_start = nanos();
  uint64_t printed_instructions = 0;
  uint64_t total_instructions = 0;

  while (input.tryGetReadBuffer() != nullptr) {
    ::capnp::PackedMessageReader message(input);
    // ::capnp::InputStreamMessageReader message(input);

    auto batch = message.getRoot<replay::Batch>();

    auto threadsList = batch.getThreads();

    assert(threadsList.size() > 0);

    std::vector<std::unique_ptr<RRFiber>> fibers;

    fibers.reserve(threadsList.size());

    for (auto threadInfo : threadsList) {
      // printf("thread: %lld\n", (long long)threadInfo.getThreadID());
      auto instructions = threadInfo.getInstructions();
      total_instructions += instructions.size();
      fibers.emplace_back(new RRFiber([instructions] () {
            replay_instructions(instructions);
          }));
    }

    for (auto &f : fibers) {
      f->Join();
    }

    if (total_instructions - printed_instructions > (4 << 20)) {
      uint64_t total_nanos = nanos() - nanos_start;
      printed_instructions = total_instructions;
      printf("total_instructions = %lld\nrate = %f instr/sec\n",
             (long long)total_instructions,
             (double)total_instructions * 1E9 / total_nanos);
    }

    // printf("end of batch!\n\n\n");
  }

  return 0;
}
