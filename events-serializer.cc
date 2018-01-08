// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "events-serializer.h"

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

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "varint_codec.h"
#include "malloc_trace_encoder.h"

typedef tcmalloc::VarintCodec VarintCodec;
typedef tcmalloc::EventsEncoder EventsEncoder;

namespace events {
  struct Malloc {
    uint64_t token;
    uint64_t size;
  };
  struct Free {
    uint64_t token;
  };
  struct FreeSized {
    uint64_t token;
    uint64_t size;
  };
  struct Realloc {
    uint64_t old_token;
    uint64_t new_token;
    uint64_t new_size;
  };
  struct Memalign {
    uint64_t token;
    uint64_t size;
    uint64_t alignment;
  };
  struct Tok {
    uint64_t ts;
    uint64_t cpu;
    uint64_t token_base;
  };
} // namespace events

EventsReceiver::~EventsReceiver() {}

static void panic(const char *reason) {
  fprintf(stderr, "%s\n", reason);
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
  const uint64_t kRealizeSize = 1 << 20;
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

class OuterEvStream : public MemStream {
public:
  OuterEvStream(Mapper* mapper) : MemStream(mapper) {}
  ~OuterEvStream() {}

  bool next(OuterEvent *ev);
};

struct BufEvent {
  uint64_t ts;
  uint64_t cpu;
  uint64_t thread_id;
  uint64_t size;
};

bool OuterEvStream::next(OuterEvent *ev) {
  if (!has_more()) {
    return false;
  }

  memset(ev, 0, sizeof(*ev));

  uint64_t first_word;
  bool ok = try_read_varint(&first_word);
  if (!ok) {
    return false;
  }
  unsigned evtype = EventsEncoder::decode_type(first_word);
  ev->type = evtype;

  switch (evtype) {
    case EventsEncoder::kEventDeath: {
      EventsEncoder::decode_death(ev, first_word, must_read_varint());
      break;
    }
    case EventsEncoder::kEventBuf: {
      uint64_t second_word;
      uint64_t third_word;
      ok = try_read_varint(&second_word);
      if (!ok) {
        return false;
      }
      ok = try_read_varint(&third_word);
      if (!ok) {
        return false;
      }

      BufEvent buf;
      EventsEncoder::decode_buffer(&buf, first_word, second_word, third_word);
      ev->thread_id = buf.thread_id;
      ev->ts = buf.ts;
      ev->cpu = buf.cpu;
      ok = try_advance_by(buf.size, &ev->buf_start);
      if (!ok) {
        return false;
      }
      ev->buf_end = ev->buf_start + buf.size;
      break;
    }
    case EventsEncoder::kEventEnd:
      break;
    case EventsEncoder::kEventSyncBarrier: {
      uint64_t second_word;
      ok = try_read_varint(&second_word);
      if (!ok) {
        return false;
      }
      EventsEncoder::decode_sync_barrier(ev, first_word, second_word);
      break;
    }
  default:
    printf("unknown type: %u\n", evtype);
    abort();
  }

  return true;
}

class ThreadState {
public:
  explicit ThreadState(uint64_t thread_id) : thread_id(thread_id) {}

  const uint64_t thread_id;
  uint64_t prev_size = 0;
  uint64_t prev_token = 0;
  uint64_t malloc_tok_seq = 0;
  uint64_t last_ts;
  uint64_t last_cpu;

  template <typename T>
  void consume_malloc(T *m, uint64_t first_word) {
    EventsEncoder::decode_malloc(m, first_word, &prev_size, &malloc_tok_seq);
  }

  template <typename T>
  void consume_free(T *f, uint64_t first_word) {
    EventsEncoder::decode_free(f, first_word, &prev_token);
  }

  template <typename T>
  void consume_free_sized(T *f, uint64_t first_word, uint64_t second_word) {
    EventsEncoder::decode_free_sized(f, first_word, second_word,
                                     &prev_token, &prev_size);
  }

  template <typename T>
  void consume_realloc(T *r, uint64_t first_word, uint64_t second_word) {
    EventsEncoder::decode_realloc(r, first_word, second_word,
                                  &prev_size, &prev_token, &malloc_tok_seq);
  }

  template <typename T>
  void consume_memalign(T *m, uint64_t first_word, uint64_t second_word) {
    EventsEncoder::decode_memalign(m, first_word, second_word,
                                   &prev_size, &malloc_tok_seq);
  }

  template <typename T>
  void consume_tok(T *ev, uint64_t first_word,
                   uint64_t second_word) {
    EventsEncoder::decode_token(ev, first_word, second_word);
    last_cpu = ev->cpu;
    last_ts = ev->ts;
    malloc_tok_seq = ev->token_base;
  }

  void update_with_buf(OuterEvent *ev) {
    last_cpu = ev->cpu;
    last_ts = ev->ts;
  }
};


struct InnerEvent {
  uint64_t type;
  union {
    events::Malloc malloc;
    events::Memalign memalign;
    events::Free free;
    events::FreeSized free_sized;
    events::Realloc realloc;
  };
};

static size_t mallocs_decoded;
static size_t frees_decoded;
static size_t sized_frees_decoded;
static size_t reallocs_decoded;
static size_t memaligns_decoded;
static size_t toks_decoded;

static __attribute__((destructor))
void dump_stats() {
  fprintf(stderr, "mallocs_decoded = %zu\n", mallocs_decoded);
  fprintf(stderr, "frees_decoded = %zu\n", frees_decoded);
  fprintf(stderr, "sized_frees_decoded = %zu\n", sized_frees_decoded);
  fprintf(stderr, "reallocs_decoded = %zu\n", reallocs_decoded);
  fprintf(stderr, "memaligns_decoded = %zu\n", memaligns_decoded);
  fprintf(stderr, "toks_decoded = %zu\n", memaligns_decoded);
}


class FullThreadState : public ThreadState {
public:
  FullThreadState(uint64_t thread_id) : ThreadState(thread_id) {}

  OuterEvent active_buf;
  InnerEvent last_event;
  std::unique_ptr<MemStream> active_stream;
  std::deque<OuterEvent> bufs;
  bool dead{};
  bool signalled{};
  bool in_pending_frees{};

  bool update_last_event_inner() {
    memset(&last_event, 0, sizeof(last_event));
    if (!active_stream->has_more()) {
      return false;
    }

    uint64_t first_word = active_stream->must_read_varint();
    unsigned evtype = EventsEncoder::decode_type(first_word);
    last_event.type = evtype;
    switch (evtype) {
    case EventsEncoder::kEventMalloc:
      mallocs_decoded++;
      consume_malloc(&last_event.malloc, first_word);
      break;
    case EventsEncoder::kEventFree:
      frees_decoded++;
      consume_free(&last_event.free, first_word);
      break;
    case EventsEncoder::kEventTok: {
      auto second_word = active_stream->must_read_varint();
      events::Tok tok;
      toks_decoded++;
      consume_tok(&tok, first_word, second_word);
      return update_last_event_inner();
    }
    case EventsEncoder::kEventRealloc:
      reallocs_decoded++;
      consume_realloc(&last_event.realloc,
                      first_word, active_stream->must_read_varint());
      break;
    case EventsEncoder::kEventMemalign:
      memaligns_decoded++;
      consume_memalign(&last_event.memalign,
                      first_word, active_stream->must_read_varint());
      break;
    case EventsEncoder::kEventFreeSized:
      sized_frees_decoded++;
      consume_free_sized(&last_event.free_sized,
                         first_word, active_stream->must_read_varint());
      break;
    default:
      panic("unknown type");
    }

    return true;
  }

  bool update_last_event() {
    while (!update_last_event_inner()) {
      update_with_buf(&active_buf);

      bufs.pop_front();
      active_stream.reset();
      if (bufs.empty()) {
        return false;
      }
      active_buf = bufs.front();
      active_stream.reset(new ConstMemStream(active_buf.buf_start, active_buf.buf_end));
    }
    return true;
  }
};

struct ThreadStateHash {
  size_t operator ()(const FullThreadState& t) const noexcept {
    return std::hash<uint64_t>{}(t.thread_id);
  }
};

struct ThreadStateEq {
  constexpr bool operator()(const FullThreadState &lhs,
                            const FullThreadState &rhs) const {
    return lhs.thread_id == rhs.thread_id;
  }
};

struct ActiveThreadStateGreater {
  bool operator()(FullThreadState *a, FullThreadState *b) {
    return a->last_ts > b->last_ts;
  }
};

struct SerializeState {
  typedef std::unordered_set<FullThreadState,
                             ThreadStateHash,
                             ThreadStateEq> set_of_threads;

  typedef std::priority_queue<FullThreadState *,
                              std::vector<FullThreadState*>,
                              ActiveThreadStateGreater> threads_heap;

  set_of_threads threads;
  threads_heap heap;
  std::vector<FullThreadState*> to_die;
  std::unordered_map<uint64_t, FullThreadState*> pending_frees;

  static constexpr uint64_t kInvalidThreadID = ~uint64_t{0};
  uint64_t recv_thread_id = kInvalidThreadID;
  uint64_t recv_ts = 0;
  uint64_t recv_cpu = 0;

  void maybe_switch_thread(EventsReceiver *receiver,
                           FullThreadState* thread) {
    uint64_t thread_id = thread->thread_id;
    if (thread_id != recv_thread_id) {
      recv_thread_id = thread_id;
      receiver->SwitchThread(recv_thread_id);
    }
    if (thread->last_ts != recv_ts || thread->last_cpu != recv_cpu) {
      recv_ts = thread->last_ts;
      recv_cpu = thread->last_cpu;
      receiver->SetTS(recv_ts, recv_cpu);
    }
  }

  void add_allocated(uint64_t tok) {
    auto it = pending_frees.find(tok);
    if (it == pending_frees.end()) {
      return;
    }
    heap.push(it->second);
    it->second->in_pending_frees = false;
    // printf("\nwoke up thread %d via tok %d\n", (int)(it->second->thread_id),(int)tok);
    pending_frees.erase(it);
  }

  FullThreadState *find_thread(uint64_t thread_id, bool may_insert) {
    auto it = threads.find(thread_id);
    if (it == threads.end()) {
      if (!may_insert) {
        panic("needed thread to exist");
      }
      it = threads.emplace(thread_id).first;
    }
    return const_cast<FullThreadState*>(&*it);
  }
};

void SerializeMallocEvents(Mapper* mapper, EventsReceiver* receiver) {
  OuterEvStream s(mapper);
  SerializeState state;

  auto& heap = state.heap;

  bool seen_end = false;

  const char* last_barriers[2] = {mapper->GetBegin(), mapper->GetBegin()};
  int last_barriers_index = 0;

  while (!seen_end) {
    OuterEvent ev;
    for (;;) {
      bool ok = s.next(&ev);
      if (!ok) {
        seen_end = true;
        break;
      }
      if (ev.type == EventsEncoder::kEventEnd
          || ev.type == EventsEncoder::kEventSyncBarrier) {
        break;
      }
      switch (ev.type) {
      case EventsEncoder::kEventBuf: {
        FullThreadState *thread = state.find_thread(ev.thread_id, true);
        assert(!thread->dead);
        bool was_empty = false;
        if (thread->bufs.empty()) {
          was_empty = true;
        }
        thread->bufs.push_back(ev);
        if (was_empty) {
          thread->active_buf = ev;
          assert(thread->active_stream.get() == nullptr);
          thread->active_stream.reset(new ConstMemStream(ev.buf_start, ev.buf_end));

          bool ok = thread->update_last_event();
          if (ok) {
            heap.push(thread);
          }
        }
        break;
      }
      case EventsEncoder::kEventDeath: {
        FullThreadState *thread = state.find_thread(ev.thread_id, false);
        assert(!thread->dead);
        thread->dead = true;
        state.to_die.push_back(thread);
        break;
      }
      default:
        panic("unknown type");
      }
    }
    if (ev.type == EventsEncoder::kEventEnd) {
      seen_end = true;
    } else {
      last_barriers_index = (last_barriers_index + 1) % 2;
      const char* prev_ptr = last_barriers[last_barriers_index];
      s.unrealize_up_to(prev_ptr);
      last_barriers[last_barriers_index] = s.get_ptr();
      // printf("\nBarrier at %p\n", s.get_ptr());
    }

    while (!heap.empty()) {
      FullThreadState* thread = heap.top();
      assert(thread->active_stream.get() != nullptr);
      assert(!thread->bufs.empty());
      uint64_t last_ts = thread->last_ts;
      bool processed = false;
      constexpr uint64_t kEmptyTok = ~uint64_t{0};
      uint64_t new_tok = kEmptyTok;
      uint64_t wait_tok;
      InnerEvent *ev = &thread->last_event;
      switch (ev->type) {
      case EventsEncoder::kEventMalloc:
        new_tok = ev->malloc.token;
        state.maybe_switch_thread(receiver, thread);
        receiver->Malloc(ev->malloc.token,
                         ev->malloc.size);
        processed = true;
        break;
      case EventsEncoder::kEventMemalign:
        new_tok = ev->memalign.token;
        state.maybe_switch_thread(receiver, thread);
        receiver->Memalign(ev->memalign.token,
                           ev->memalign.size,
                           ev->memalign.alignment);
        processed = true;
        break;
      case EventsEncoder::kEventFree:
        wait_tok = ev->free.token;
        if (receiver->HasAllocated(wait_tok)) {
          processed = true;
          state.maybe_switch_thread(receiver, thread);
          receiver->Free(wait_tok);
        }
        break;
      case EventsEncoder::kEventFreeSized:
        wait_tok = ev->free_sized.token;
        if (receiver->HasAllocated(wait_tok)) {
          processed = true;
          state.maybe_switch_thread(receiver, thread);
          receiver->FreeSized(wait_tok, ev->free_sized.size);
        }
        break;
      case EventsEncoder::kEventRealloc:
        wait_tok = ev->realloc.old_token;
        if (receiver->HasAllocated(wait_tok)) {
          processed = true;
          new_tok = ev->realloc.new_token;
          state.maybe_switch_thread(receiver, thread);
          receiver->Realloc(ev->realloc.old_token,
                            ev->realloc.new_token, ev->realloc.new_size);
        }
        break;
      default:
        panic("unknown event");
      }

      assert(processed || new_tok == kEmptyTok);

      if (processed) {
        bool ok = thread->update_last_event();
        if (!ok) {
          if (thread->dead) {
            receiver->KillCurrentThread();
            thread->signalled = true;
          }
          heap.pop();
        } else if (thread->last_ts != last_ts) {
          heap.pop();
          heap.push(thread);
        }
      } else {
        heap.pop();
        thread->in_pending_frees = true;
        assert(state.pending_frees.count(wait_tok) == 0);
        // printf("\nthread: %d goes to sleep for tok %d\n", (int)thread->thread_id, (int)wait_tok);
        state.pending_frees.insert({wait_tok, thread});
      }

      // now that we're not expecting thread to be at the top of heap,
      // we can process adding new token to allocated which may "wake
      // up" some pending frees (and thus add some thread to heap)
      if (new_tok != kEmptyTok) {
        state.add_allocated(new_tok);
      }
    }

    if (!state.pending_frees.empty()) {
      fprintf(stderr, "\n\ngot a case with %d threads in pending frees\n", int(state.pending_frees.size()));
    }

    std::vector<FullThreadState*> next_to_die;
    for (auto thread : state.to_die) {
      assert(thread->dead);
      assert(thread->bufs.empty());
      if (thread->in_pending_frees) {
        assert(!thread->signalled);
        next_to_die.push_back(thread);
        continue;
      }
      if (!thread->signalled) {
        receiver->SwitchThread(thread->thread_id);
        receiver->KillCurrentThread();
        state.recv_thread_id = SerializeState::kInvalidThreadID;
      }
      assert(thread->bufs.empty());
      assert(thread->active_stream.get() == nullptr);
      state.threads.erase(*thread);
    }
    state.to_die.swap(next_to_die);

    receiver->Barrier();
  }

  // for (const auto& a_thread : state.threads) {
  //   assert(a_thread.bufs.empty());
  //   assert(a_thread.active_stream.get() == nullptr);
  //   assert(!a_thread.dead);
  //   assert(!a_thread.in_pending_frees);
  // }
}
