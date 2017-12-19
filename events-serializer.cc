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
  MemStream(const char *begin, const char *end)
    : begin_(begin), end_(end), ptr_(begin) {}
  ~MemStream() {}

  inline uint64_t must_read_varint();
  inline void advance_by(uint64_t amount);

  bool has_more() {
    return ptr_ < end_;
  }

protected:
  const char *const begin_;
  const char *const end_;
  const char *ptr_;
};

void MemStream::advance_by(uint64_t amount) {
  uintptr_t old_ptr = reinterpret_cast<uintptr_t>(ptr_);
  uintptr_t new_ptr = old_ptr + amount;
  if (new_ptr < old_ptr) {
    abort();
  }

  ptr_ += amount;
  if (ptr_ > end_) {
    abort();
  }
}

uint64_t MemStream::must_read_varint() {
  assert(has_more());
  VarintCodec::DecodeResult<uint64_t> res = VarintCodec::decode_unsigned(ptr_);
  advance_by(res.advance);
  return res.value;
}

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
  OuterEvStream(const char *begin, const char *end)
    : MemStream(begin, end) {}
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

  uint64_t first_word = must_read_varint();
  unsigned evtype = EventsEncoder::decode_type(first_word);
  ev->type = evtype;

  switch (evtype) {
    case EventsEncoder::kEventDeath: {
      EventsEncoder::decode_death(ev, first_word, must_read_varint());
      break;
    }
    case EventsEncoder::kEventBuf: {
      uint64_t second_word = must_read_varint();
      uint64_t third_word = must_read_varint();

      BufEvent buf;
      EventsEncoder::decode_buffer(&buf, first_word, second_word, third_word);
      ev->thread_id = buf.thread_id;
      ev->ts = buf.ts;
      ev->cpu = buf.cpu;
      ev->buf_start = ptr_;
      advance_by(buf.size);
      ev->buf_end = ptr_;
      break;
    }
    case EventsEncoder::kEventEnd:
      break;
    case EventsEncoder::kEventSyncAllEnd: {
      uint64_t second_word = must_read_varint();
      EventsEncoder::decode_sync_all_all(ev, first_word, second_word);
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
                   uint64_t second_word, uint64_t third_word) {
    struct Inter {
      uint64_t thread_id;
      uint64_t cpu;
      uint64_t ts;
      uint64_t token_base;
    };
    Inter i;
    EventsEncoder::decode_token(&i, first_word, second_word, third_word);
    ev->cpu = last_cpu = i.cpu;
    ev->ts = last_ts = i.ts;
    ev->token_base = malloc_tok_seq = i.token_base;
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

class FullThreadState : public ThreadState {
public:
  FullThreadState(uint64_t thread_id) : ThreadState(thread_id) {}

  OuterEvent active_buf;
  InnerEvent last_event;
  std::unique_ptr<MemStream> active_stream;
  std::deque<OuterEvent> bufs;
  bool dead{};
  bool signalled{};

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
      consume_malloc(&last_event.malloc, first_word);
      break;
    case EventsEncoder::kEventFree:
      consume_free(&last_event.free, first_word);
      break;
    case EventsEncoder::kEventTok: {
      auto second_word = active_stream->must_read_varint();
      auto third_word = active_stream->must_read_varint();
      events::Tok tok;
      consume_tok(&tok, first_word, second_word, third_word);
      return update_last_event_inner();
    }
    case EventsEncoder::kEventRealloc:
      consume_realloc(&last_event.realloc,
                      first_word, active_stream->must_read_varint());
      break;
    case EventsEncoder::kEventMemalign:
      consume_memalign(&last_event.memalign,
                      first_word, active_stream->must_read_varint());
      break;
    case EventsEncoder::kEventFreeSized:
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
      active_stream.reset(new MemStream(active_buf.buf_start, active_buf.buf_end));
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

  void maybe_switch_thread(EventsReceiver *receiver,
                           uint64_t thread_id,
                           uint64_t last_ts) {
    if (thread_id != recv_thread_id) {
      receiver->SwitchThread(thread_id);
      recv_thread_id = thread_id;
    }
    if (last_ts != recv_ts) {
      receiver->SetTS(last_ts, 0);
      recv_ts = last_ts;
    }
  }

  void add_allocated(uint64_t tok) {
    auto it = pending_frees.find(tok);
    if (it == pending_frees.end()) {
      return;
    }
    heap.push(it->second);
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

void SerializeMallocEvents(const char* begin, const char* end,
                           EventsReceiver* receiver) {
  OuterEvStream s(begin, end);
  SerializeState state;

  auto& heap = state.heap;

  uint64_t recv_thread_id = ~uint64_t{0};
  uint64_t recv_ts = 0;

  for (;;) {
    OuterEvent ev;
    for (;;) {
      bool ok = s.next(&ev);
      if (!ok) {
        panic("unexpected stream end");
      }
      if (ev.type == EventsEncoder::kEventEnd
          || ev.type == EventsEncoder::kEventSyncAllEnd) {
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
          thread->active_stream.reset(new MemStream(ev.buf_start, ev.buf_end));

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
      return;
    }

    while (!heap.empty()) {
      FullThreadState* thread = heap.top();
      assert(thread->active_stream.get() != nullptr);
      assert(!thread->bufs.empty());
      uint64_t last_ts = thread->last_ts;
      bool processed;
      constexpr uint64_t kEmptyTok = ~uint64_t{0};
      uint64_t new_tok = kEmptyTok;
      uint64_t wait_tok;
      InnerEvent *ev = &thread->last_event;
      switch (ev->type) {
      case EventsEncoder::kEventMalloc:
        new_tok = ev->malloc.token;
        state.maybe_switch_thread(receiver, thread->thread_id, last_ts);
        receiver->Malloc(ev->malloc.token,
                         ev->malloc.size);
        processed = true;
        break;
      case EventsEncoder::kEventMemalign:
        new_tok = ev->memalign.token;
        state.maybe_switch_thread(receiver, thread->thread_id, last_ts);
        receiver->Memalign(ev->memalign.token,
                           ev->memalign.size,
                           ev->memalign.alignment);
        processed = true;
        break;
      case EventsEncoder::kEventFree:
        wait_tok = ev->free.token;
        if (receiver->HasAllocated(wait_tok)) {
          processed = true;
          state.maybe_switch_thread(receiver, thread->thread_id, last_ts);
          receiver->Free(wait_tok);
        }
        break;
      case EventsEncoder::kEventFreeSized:
        wait_tok = ev->free_sized.token;
        if (receiver->HasAllocated(wait_tok)) {
          processed = true;
          state.maybe_switch_thread(receiver, thread->thread_id, last_ts);
          receiver->FreeSized(wait_tok, ev->free_sized.size);
        }
        break;
      case EventsEncoder::kEventRealloc:
        wait_tok = ev->realloc.old_token;
        if (receiver->HasAllocated(wait_tok)) {
          processed = true;
          new_tok = ev->realloc.new_token;
          state.maybe_switch_thread(receiver, thread->thread_id, last_ts);
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
        state.pending_frees.insert({wait_tok, thread});
      }

      // now that we're not expecting thread to be at the top of heap,
      // we can process adding new token to allocated which may "wake
      // up" some pending frees (and thus add some thread to heap)
      if (new_tok != kEmptyTok) {
        state.add_allocated(new_tok);
      }
    }

    assert(state.pending_frees.empty());
    assert(heap.empty());

    for (auto thread : state.to_die) {
      assert(thread->dead);
      assert(thread->bufs.empty());
      if (!thread->signalled) {
        receiver->SwitchThread(thread->thread_id);
        receiver->KillCurrentThread();
        state.recv_thread_id = SerializeState::kInvalidThreadID;
      }
      state.threads.erase(*thread);
    }
    state.to_die.clear();

    receiver->Barrier();
  }
}
