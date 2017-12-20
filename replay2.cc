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
#include <utility>
#include <sys/mman.h>
#include <signal.h>

#include <boost/intrusive/unordered_set.hpp>

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

using namespace boost::intrusive;

class AllocatedMap {
public:
  struct Element : public boost::intrusive::unordered_set_base_hook<> {
    uint64_t token;
    uint64_t reg;
  };
  struct ElemenKeyOp {
    typedef uint64_t type;
    const type& operator()(const Element& v) const {
      return v.token;
    }
  };

  AllocatedMap();

  Element* Lookup(uint64_t token);
  void Erase(Element *e);
  void Insert(uint64_t token, uint64_t reg);

private:
  typedef unordered_set<Element,
                        key_of_value<ElemenKeyOp>,
                        power_2_buckets<true>> set_type;

  static constexpr size_t kMinBucketSize = 16 << 10;
  static_assert((kMinBucketSize & (kMinBucketSize - 1)) == 0, "kMinBucketSize is power of 2");

  size_t buckets_size_{kMinBucketSize};
  std::unique_ptr<set_type::bucket_type[]> buckets_;
  set_type set_;
};

AllocatedMap::AllocatedMap() : buckets_(new set_type::bucket_type[kMinBucketSize]),
                               set_(set_type::bucket_traits(buckets_.get(), kMinBucketSize)) {
}

AllocatedMap::Element* AllocatedMap::Lookup(uint64_t token) {
  auto it = set_.find(token);
  if (it == set_.end()) {
    return nullptr;
  }
  return &*it;
}

void AllocatedMap::Erase(AllocatedMap::Element* e) {
  set_.erase(set_.iterator_to(*e));
  delete e;
}

void AllocatedMap::Insert(uint64_t token, uint64_t reg) {
  assert(Lookup(token) == nullptr);
  if (set_.size() > buckets_size_ * 3 / 4) {
    buckets_size_ *= 2;
    decltype(buckets_) new_buckets{new set_type::bucket_type[buckets_size_]};
    set_.rehash(set_type::bucket_traits(new_buckets.get(), buckets_size_));
    buckets_.swap(new_buckets);
  }
  auto e = new Element;
  e->token = token;
  e->reg = reg;
  set_.insert(*e);
}

using capnp::MallocMessageBuilder;
using capnp::Orphan;
using capnp::AnyPointer;

typedef void (*set_instr_ptr)(replay::Instruction::Builder* instr,
                              Orphan<AnyPointer>&& v);

template <typename T>
struct SetInstr { };

#define I(K) \
template <> struct SetInstr<replay::K> {                                \
  static void setInstr(replay::Instruction::Builder* instr,             \
                       Orphan<AnyPointer>&& v) {                        \
    instr->adopt##K(v.releaseAs<replay::K>());                          \
  }                                                                     \
};

I(Malloc)
I(Free)
I(Memalign)
I(Realloc)
I(FreeSized)
I(KillThread)
I(SwitchThread)

#undef I

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

  template <typename T>
  capnp::BuilderFor<T> appendInstr() {
    Orphan<AnyPointer> instr = builder_.getOrphanage().newOrphan<T>();
    auto builder = instr.getAs<T>();
    auto ptr = &SetInstr<T>::setInstr;
    instructions_.emplace_back(std::make_pair(ptr, std::move(instr)));
    return builder;
  }

  void reset_instructions() {
    instructions_.clear();
    builder_.~MallocMessageBuilder();
    new (&builder_) MallocMessageBuilder(first_segment_.asPtr());
    builder_.initRoot<replay::Batch>();
  }

  writer_fn_t writer_fn_;
  IdTree ids_space_;
  AllocatedMap allocated_;

  kj::Array<capnp::word> first_segment_;
  MallocMessageBuilder builder_;
  std::vector<std::pair<set_instr_ptr, Orphan<AnyPointer>>> instructions_;
};

static constexpr int kFirstSegmentWordsCount = 1 << 20;

ReplayReceiver::ReplayReceiver(const writer_fn_t& writer_fn)
    : writer_fn_(writer_fn),
      first_segment_(kj::heapArray<capnp::word>(kFirstSegmentWordsCount)),
      builder_(first_segment_.asPtr()) {

  builder_.initRoot<replay::Batch>();
}

void ReplayReceiver::KillCurrentThread() {
  appendInstr<replay::KillThread>();
}

void ReplayReceiver::SwitchThread(uint64_t thread_id) {
  auto instr = appendInstr<replay::SwitchThread>();
  instr.setThreadID(thread_id);
}

void ReplayReceiver::SetTS(uint64_t ts, uint64_t cpu) {
}

void ReplayReceiver::Malloc(uint64_t tok, uint64_t size) {
  auto reg = ids_space_.allocate_id();
  allocated_.Insert(tok, reg);

  auto m = appendInstr<replay::Malloc>();
  m.setReg(reg);
  m.setSize(size);
}

void ReplayReceiver::Memalign(uint64_t tok, uint64_t size, uint64_t align) {
  auto reg = ids_space_.allocate_id();
  allocated_.Insert(tok, reg);

  auto m = appendInstr<replay::Memalign>();
  m.setReg(reg);
  m.setSize(size);
  m.setAlignment(align);
}

void ReplayReceiver::Realloc(uint64_t old_tok,
                             uint64_t new_tok, uint64_t new_size) {
  auto* e = allocated_.Lookup(old_tok);
  auto new_reg = ids_space_.allocate_id();
  auto old_reg = e->reg;
  allocated_.Erase(e);
  allocated_.Insert(new_tok, new_reg);
  ids_space_.free_id(old_reg);

  auto r = appendInstr<replay::Realloc>();
  r.setOldReg(old_reg);
  r.setNewReg(new_reg);
  r.setSize(new_size);
}

void ReplayReceiver::Free(uint64_t tok) {
  auto* e = allocated_.Lookup(tok);
  auto old_reg = e->reg;
  allocated_.Erase(e);
  ids_space_.free_id(old_reg);

  auto f = appendInstr<replay::Free>();
  f.setReg(old_reg);
}

void ReplayReceiver::FreeSized(uint64_t tok, uint64_t size) {
  auto* e = allocated_.Lookup(tok);
  auto old_reg = e->reg;
  allocated_.Erase(e);
  ids_space_.free_id(old_reg);

  auto f = appendInstr<replay::FreeSized>();
  f.setReg(old_reg);
  f.setSize(size);
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

  replay::Batch::Builder batch = builder_.getRoot<replay::Batch>();
  capnp::List<replay::Instruction>::Builder inst_list = batch.initInstructions(size);
  for (int i = 0; i < size; i++) {
    auto &p = instructions_[i];
    auto builder = inst_list[i];
    p.first(&builder, std::move(p.second));
  }

  {
    FunctionOutputStream os(writer_fn_);
    capnp::writePackedMessage(os, builder_);
    // capnp::writeMessage(os, builder_);
  }

  reset_instructions();
}

bool ReplayReceiver::HasAllocated(uint64_t tok) {
  return allocated_.Lookup(tok) != nullptr;
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

  rv = madvise(mmap_result, st.st_size, MADV_SEQUENTIAL);
  if (rv < 0) {
    pabort("madvise");
  }
  rv = posix_fadvise(fd2, 0, st.st_size, POSIX_FADV_SEQUENTIAL);
  if (rv < 0) {
    pabort("posix_fadvise");
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
