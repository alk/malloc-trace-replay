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
#include <sys/stat.h>

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <vector>
#include <time.h>
#include <unordered_map>
#include <sys/mman.h>
#include <signal.h>

#include <kj/array.h>
#include <capnp/message.h>
#include <capnp/orphan.h>
#include <capnp/serialize-packed.h>

#include "replay2.capnp.h"
#include "events-serializer.h"
#include "id_tree.h"

static void pabort(const char *t) {
  perror(t);
  abort();
}

class PrintReceiver : public EventsReceiver {
public:
  PrintReceiver(EventsReceiver* target) : target_(target) {}
  ~PrintReceiver() {}

  void KillCurrentThread() {
    printf("KillCurrentThread\n");
    target_->KillCurrentThread();
  }
  void SwitchThread(uint64_t thread_id) {
    printf("SwitchThread(%zu)\n", (size_t)thread_id);
    target_->SwitchThread(thread_id);
  }
  void SetTS(uint64_t ts, uint64_t cpu) {
    printf("SetTS(%zu, %zu)\n", (size_t)ts, (size_t)cpu);
    target_->SetTS(ts, cpu);
  }
  void Malloc(uint64_t tok, uint64_t size) {
    printf("Malloc(%zu, %zu)\n", (size_t)tok, (size_t)size);
    target_->Malloc(tok, size);
  }
  void Memalign(uint64_t tok, uint64_t size, uint64_t align) {
    printf("Memalign(%zu, %zu, %zu)\n", (size_t)tok, (size_t)size, (size_t)align);
    target_->Memalign(tok, size, align);
  }
  void Realloc(uint64_t old_tok,
               uint64_t new_tok, uint64_t new_size) {
    printf("Realloc(%zu, %zu, %zu)\n",
           (size_t)old_tok, (size_t)new_tok, (size_t)new_size);
    target_->Realloc(old_tok, new_tok, new_size);
  }
  void Free(uint64_t tok) {
    printf("Free(%zu)\n", (size_t)tok);
    target_->Free(tok);
  }
  void FreeSized(uint64_t tok, uint64_t size) {
    printf("FreeSized(%zu, %zu)\n",
           (size_t)tok, (size_t)size);
    target_->FreeSized(tok, size);
  }
  void Barrier() {
    printf("Barrier\n");
    target_->Barrier();
  }
  bool HasAllocated(uint64_t tok) {
    return target_->HasAllocated(tok);
  }

private:
  EventsReceiver* const target_;
};

using capnp::MallocMessageBuilder;
using capnp::Orphan;

class ReplayReceiver : public EventsReceiver {
public:
  typedef std::function<int (const void *, size_t)> writer_fn_t;

  ReplayReceiver(const writer_fn_t& writer_fn);
  ~ReplayReceiver() noexcept {}

  virtual void KillCurrentThread();
  virtual void SwitchThread(uint64_t thread_id);
  virtual void SetTS(uint64_t ts, uint64_t cpu);
  virtual void Malloc(uint64_t tok, uint64_t size);
  virtual void Memalign(uint64_t tok, uint64_t size, uint64_t align);
  virtual void Realloc(uint64_t old_tok,
                       uint64_t new_tok, uint64_t new_size);
  virtual void Free(uint64_t tok);
  virtual void FreeSized(uint64_t tok, uint64_t size);
  virtual void Barrier();
  virtual bool HasAllocated(uint64_t tok);

private:
  template <typename Body>
  void appendInstr(const Body& body) {
    auto instr = builder_.getOrphanage().newOrphan<replay::Instruction>();
    body(instr.get());
    instructions_.push_back(std::move(instr));
  }

  static constexpr int kFirstSegmentWordsCount = 1 << 20;

  void reset_instructions() {
    instructions_.clear();
    builder_.~MallocMessageBuilder();

    builder_first_segment_ = kj::heapArray<capnp::word>(kFirstSegmentWordsCount);
    auto fs = builder_first_segment_.asPtr();
    memset(fs.begin(), 0, fs.size() * sizeof(capnp::word));
    new (&builder_) MallocMessageBuilder(fs);
  }

  writer_fn_t writer_fn_;
  IdTree ids_space_;
  std::unordered_map<uint64_t, uint64_t> allocated_;
  uint64_t current_thread_{};

  MallocMessageBuilder builder_;
  std::vector<Orphan<replay::Instruction>> instructions_;
  kj::Array<capnp::word> first_segment_;
  kj::Array<capnp::word> builder_first_segment_;
};

ReplayReceiver::ReplayReceiver(const writer_fn_t& writer_fn)
    : writer_fn_(writer_fn),
      first_segment_(kj::heapArray<capnp::word>(kFirstSegmentWordsCount)),
      builder_first_segment_(kj::heapArray<capnp::word>(kFirstSegmentWordsCount)) {
}

void ReplayReceiver::KillCurrentThread() {
  appendInstr([] (replay::Instruction::Builder instr) {
      instr.initKillThread();
    });
}

void ReplayReceiver::SwitchThread(uint64_t thread_id) {
  appendInstr([&] (replay::Instruction::Builder instr) {
      auto st = instr.initSwitchThread();
      st.setThreadID(thread_id);
    });
  current_thread_ = thread_id;
}

void ReplayReceiver::SetTS(uint64_t ts, uint64_t cpu) {
}

void ReplayReceiver::Malloc(uint64_t tok, uint64_t size) {
  auto reg = ids_space_.allocate_id();
  allocated_[tok] = reg;

  appendInstr([&] (replay::Instruction::Builder instr) {
      auto m = instr.initMalloc();
      m.setReg(reg);
      m.setSize(size);
    });
}

void ReplayReceiver::Memalign(uint64_t tok, uint64_t size, uint64_t align) {
  auto reg = ids_space_.allocate_id();
  allocated_[tok] = reg;

  appendInstr([&] (replay::Instruction::Builder instr) {
      auto m = instr.initMemalign();
      m.setReg(reg);
      m.setSize(size);
      m.setAlignment(align);
    });
}

void ReplayReceiver::Realloc(uint64_t old_tok,
                             uint64_t new_tok, uint64_t new_size) {
  auto new_reg = ids_space_.allocate_id();
  auto old_reg = allocated_[old_tok];
  allocated_.erase(old_tok);
  allocated_[new_tok] = new_reg;
  ids_space_.free_id(old_reg);

  appendInstr([&] (replay::Instruction::Builder instr) {
      auto m = instr.initRealloc();
      m.setOldReg(old_reg);
      m.setNewReg(new_reg);
      m.setSize(new_size);
    });
}

void ReplayReceiver::Free(uint64_t tok) {
  auto old_reg = allocated_[tok];
  allocated_.erase(tok);
  ids_space_.free_id(old_reg);

  appendInstr([&] (replay::Instruction::Builder instr) {
      auto m = instr.initFree();
      m.setReg(old_reg);
    });
}

void ReplayReceiver::FreeSized(uint64_t tok, uint64_t size) {
  auto old_reg = allocated_[tok];
  allocated_.erase(tok);
  ids_space_.free_id(old_reg);

  appendInstr([&] (replay::Instruction::Builder instr) {
      auto m = instr.initFreeSized();
      m.setReg(old_reg);
      m.setSize(size);
    });
}

class FunctionOutputStream : public ::kj::OutputStream {
public:
  FunctionOutputStream(const ReplayReceiver::writer_fn_t& writer) : writer_(writer) {}
  ~FunctionOutputStream() = default;

  virtual void write(const void* buffer, size_t size) {
    writer_(buffer, size);
  }
private:
  const ReplayReceiver::writer_fn_t &writer_;
};

void ReplayReceiver::Barrier() {
  auto size = instructions_.size();
  if (size == 0) {
    return;
  }

  auto seg = kj::heapArray<capnp::word>(kFirstSegmentWordsCount);
  auto fs = seg.asPtr();
  memset(fs.begin(), 0, fs.size() * sizeof(capnp::word));
  MallocMessageBuilder message{fs};

  replay::Batch::Builder batch = message.initRoot<replay::Batch>();
  capnp::List<replay::Instruction>::Builder inst_list = batch.initInstructions(size);
  for (int i = 0; i < size; i++) {
    const auto& instr = instructions_[i];
    inst_list.setWithCaveats(i, instr.getReader());
  }

  reset_instructions();

  {
    FunctionOutputStream os(writer_fn_);
    ::capnp::writePackedMessage(os, message);
  }
}

bool ReplayReceiver::HasAllocated(uint64_t tok) {
  return allocated_.count(tok) != 0;
}

int main(int argc, char **argv) {
  int fd = 0;
  if (argc < 3) {
    fprintf(stderr, "need 2 args\n");
    return 0;
  }

  fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    pabort("open");
  }

  int fd2 = open(argv[2], O_WRONLY|O_CREAT|O_TRUNC, 0666);
  if (fd2 < 0) {
    pabort("open");
  }

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

  ReplayReceiver::writer_fn_t writer = [fd2] (const void *buf, size_t sz) -> int{
    int rv = write(fd2, buf, sz);
    if (rv != sz) {
      pabort("write");
    }
    return 0;
  };

  ReplayReceiver receiver(writer);
  PrintReceiver printer(&receiver);

  signal(SIGINT, [](int dummy) {
      exit(0);
    });

  // SerializeMallocEvents(mmap_area, mmap_area + st.st_size, &printer);
  SerializeMallocEvents(mmap_area, mmap_area + st.st_size, &receiver);
}
