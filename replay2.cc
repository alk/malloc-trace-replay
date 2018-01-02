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
#include <sys/time.h>

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <vector>
#include <time.h>
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
#include "instruction.h"

static void pabort(const char *t) {
  perror(t);
  // abort();
  asm volatile ("int $3");
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
  struct TokenHasher {
    size_t operator()(uint64_t x) const {
      // return x * UINT64_C(0x924924924924925); // yes, it's prime!
      return x;
    }
  };
  struct ElementHasher {
    size_t operator()(const Element& x) const {
      return TokenHasher{}(x.token);
    }
  };
  struct TokenEq {
    bool operator()(uint64_t token, const Element& b) {
      return token == b.token;
    }
  };
  struct ElementEq {
    bool operator()(const Element& a, const Element& b) {
      return a.token == b.token;
    }
  };

  AllocatedMap();

  Element* Lookup(uint64_t token);
  void Erase(Element *e);
  void Insert(uint64_t token, uint64_t reg);

private:
  typedef unordered_multiset<Element,
                             // key_of_value<ElemenKeyOp>,
                             power_2_buckets<true>,
                             hash<ElementHasher>, equal<ElementEq>> set_type;

  static constexpr size_t kMinBucketSize = 16 << 10;
  static_assert((kMinBucketSize & (kMinBucketSize - 1)) == 0, "kMinBucketSize is power of 2");

  static constexpr size_t kBucketsAmount = size_t{64} << 30;
  static set_type::bucket_type* AllocateBuckets() {
    auto mmap_result = mmap(nullptr, kBucketsAmount,
                            PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, 0, 0);
    if (mmap_result == MAP_FAILED) {
      pabort("mmap");
    }
    return new (mmap_result) set_type::bucket_type[kMinBucketSize];
  }
  struct DeallocateBuckets {
    void operator ()(set_type::bucket_type* ptr) {
      munmap(ptr, kBucketsAmount);
    }
  };

  size_t buckets_size_{kMinBucketSize};
  const std::unique_ptr<set_type::bucket_type, DeallocateBuckets> buckets_;
  set_type set_;
};

AllocatedMap::AllocatedMap() : buckets_(AllocateBuckets()),
                               set_(set_type::bucket_traits(buckets_.get(), kMinBucketSize)) {
}

AllocatedMap::Element* AllocatedMap::Lookup(uint64_t token) {
  auto it = set_.find(token, TokenHasher{}, TokenEq{});
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
  if (set_.size() > buckets_size_ / 2) {
    new (buckets_.get() + buckets_size_) set_type::bucket_type[buckets_size_];
    buckets_size_ *= 2;
    set_.rehash(set_type::bucket_traits(buckets_.get(), buckets_size_));
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

uint64_t nanos() {
  struct timeval tv;
  int rv = gettimeofday(&tv, nullptr);
  if (rv != 0) {
    perror("gettimeofday");
    abort();
  }
  return (tv.tv_usec + uint64_t{1000000} * tv.tv_sec) * uint64_t{1000};
}

class SimpleReceiver : public EventsReceiver {
public:
  SimpleReceiver(FILE *out) : out_(out) {}
  ~SimpleReceiver() noexcept {}

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
  FILE* out_;
  IdTree ids_space_;
  AllocatedMap allocated_;

  uint64_t start_nanos_{nanos()};
  uint64_t total_instructions_{};
  uint64_t printed_instructions_{};

  void dump(const Instruction& i) {
    fwrite_unlocked(&i, 1, sizeof(i), out_);

    total_instructions_ += 1;
    if (total_instructions_ - printed_instructions_ > (4 << 20)) {
      uint64_t total_nanos = nanos() - start_nanos_;
      printed_instructions_ = total_instructions_;
      fprintf(stderr,
              "\rtotal_instructions = %lld; rate = %f ops/sec         \b\b\b\b\b\b\b\b\b",
              (long long)total_instructions_,
              (double)total_instructions_ * 1E9 / total_nanos);
      fflush(stderr);
    }
  }
};

void SimpleReceiver::KillCurrentThread() {
  Instruction i(Instruction::Type::KILL_THREAD);
  dump(i);
}

void SimpleReceiver::SwitchThread(uint64_t thread_id) {
  Instruction s(Instruction::Type::SWITCH_THREAD);
  s.switch_thread.thread_id = thread_id;
  dump(s);
}

void SimpleReceiver::SetTS(uint64_t ts, uint64_t cpu) {
}

void SimpleReceiver::Malloc(uint64_t tok, uint64_t size) {
  auto reg = ids_space_.allocate_id();
  allocated_.Insert(tok, reg);

  Instruction m(Instruction::Type::MALLOC);
  m.reg = reg;
  m.malloc.size = size;
  dump(m);
}

void SimpleReceiver::Memalign(uint64_t tok, uint64_t size, uint64_t align) {
 auto reg = ids_space_.allocate_id();
  allocated_.Insert(tok, reg);

  Instruction m(Instruction::Type::MEMALIGN);
  m.reg = reg;
  m.malloc.size = size;
  m.malloc.align = align;
  dump(m);
}

void SimpleReceiver::Realloc(uint64_t old_tok,
                             uint64_t new_tok, uint64_t new_size) {
  auto* e = allocated_.Lookup(old_tok);
  auto new_reg = ids_space_.allocate_id();
  auto old_reg = e->reg;
  allocated_.Erase(e);
  allocated_.Insert(new_tok, new_reg);
  ids_space_.free_id(old_reg);

  Instruction r(Instruction::Type::REALLOC);
  r.reg = old_reg;
  r.realloc.new_reg = old_reg;
  r.realloc.new_size = new_size;
  dump(r);
}

void SimpleReceiver::Free(uint64_t tok) {
  auto* e = allocated_.Lookup(tok);
  auto old_reg = e->reg;
  allocated_.Erase(e);
  ids_space_.free_id(old_reg);

  Instruction f(Instruction::Type::FREE);
  f.reg = old_reg;
  dump(f);
}

void SimpleReceiver::FreeSized(uint64_t tok, uint64_t size) {
  auto* e = allocated_.Lookup(tok);
  auto old_reg = e->reg;
  allocated_.Erase(e);
  ids_space_.free_id(old_reg);

  Instruction f(Instruction::Type::FREE_SIZED);
  f.reg = old_reg;
  f.malloc.size = size;
  dump(f);
}

void SimpleReceiver::Barrier() {
}

bool SimpleReceiver::HasAllocated(uint64_t tok) {
  return allocated_.Lookup(tok) != nullptr;
}

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
  uint64_t start_nanos_{nanos()};
  uint64_t total_instructions_{};
  uint64_t printed_instructions_{};
};

static constexpr int kFirstSegmentWordsCount = 1 << 20;

template <typename T>
kj::Array<T> mkZeroedArray(size_t size) {
  auto rv = kj::heapArray<T>(size);
  memset(rv.begin(), 0, rv.size() * sizeof(T));
  return rv;
}

ReplayReceiver::ReplayReceiver(const writer_fn_t& writer_fn)
    : writer_fn_(writer_fn),
      first_segment_(mkZeroedArray<capnp::word>(kFirstSegmentWordsCount)),
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

  total_instructions_ += size;
  if (total_instructions_ - printed_instructions_ > (4 << 20)) {
    uint64_t total_nanos = nanos() - start_nanos_;
    printed_instructions_ = total_instructions_;
    printf("\rtotal_instructions = %lld; rate = %f ops/sec         \b\b\b\b\b\b\b\b\b",
           (long long)total_instructions_,
           (double)total_instructions_ * 1E9 / total_nanos);
    fflush(stdout);
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

  int rv = posix_fadvise(fd2, 0, 0, POSIX_FADV_SEQUENTIAL);
  if (rv < 0) {
    pabort("posix_fadvise");
  }
  // if our output is pipe to some decompressor, larger pipe buffer is
  // good idea
#ifdef F_SETPIPE_SZ
  fcntl(fd2, F_SETPIPE_SZ, 1 << 20);
  fcntl(fd, F_SETPIPE_SZ, 1 << 20);

  fcntl(fd2, F_SETPIPE_SZ, 16 << 20);
#endif

  signal(SIGINT, [](int dummy) {
      exit(0);
    });

  FILE* output = fdopen(fd2, "w");
  setvbuf(output, nullptr, _IOFBF, 1 << 20);
  SimpleReceiver receiver(output);

  // ConstMapper m{mmap_mapper(fd)};
  FDInputMapper m(fd);
  // SerializeMallocEvents(&m, &printer);
  SerializeMallocEvents(&m, &receiver);

  // ReplayReceiver::writer_fn_t writer = [fd2] (const void *buf, size_t sz) -> int{
  //   int rv = write(fd2, buf, sz);
  //   if (rv != sz) {
  //     pabort("write");
  //   }
  //   return 0;
  // };

  // ReplayReceiver receiver(writer);
  // PrintReceiver printer(&receiver);

  // // ConstMapper m{mmap_mapper(fd)};
  // FDInputMapper m(fd);
  // // SerializeMallocEvents(&m, &printer);
  // SerializeMallocEvents(&m, &receiver);
}
