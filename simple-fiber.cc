// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "simple-fiber.h"

#include <string.h>

static void* volatile stack_space_noopt;

static __thread SimpleFiber* current;
static __thread ucontext_t top_context;

SimpleFiber* SimpleFiber::current() {
  return ::current;
}

extern "C" int swapcontext_light(ucontext_t *ctx, ucontext_t *ctx2);

ucontext_t* SimpleFiber::fibers_context(SimpleFiber* f) {
  if (f == nullptr) {
    return &top_context;
  }
  return &f->context_;
}

static void must_getcontext(ucontext_t* place) {
  int rv = getcontext(place);
  if (rv) {
    perror("getcontext");
    abort();
  }
}

static void must_swapcontext(ucontext_t* old, ucontext_t *newc) {
  int rv = swapcontext_light(old, newc);
  if (rv) {
    perror("swapcontext");
    abort();
  }
}

static void * volatile some_noopt_ptr;

static void call_function(std::function<void ()>* body) {
  (*body)();
}

static void (* volatile call_function_ptr)(std::function<void ()>*) = call_function;

namespace {
template <typename B>
void no_inline(B b) {
  std::function<void ()> f{b};
  call_function_ptr(&f);
}
} // namespace

static void park_thread(std::mutex *m, std::condition_variable* c,
                        bool *shutdown_flag, bool *done_flag) {
  constexpr int kStackSpace = 512;
  char some_stack_space[kStackSpace];

  some_noopt_ptr = &some_stack_space;
  memset(some_stack_space, 0, sizeof(kStackSpace));

  std::unique_lock<std::mutex> g(*m);
  *done_flag = true;
  c->notify_all();
  while (!*shutdown_flag) {
    c->wait(g);
  }
}

static void get_context_and_switch(ucontext_t *place,
                                   bool *first_time,
                                   const std::function<void ()>& body) {
  constexpr int kStackOffset = 4 << 10;
  char some_stack_space[kStackOffset];
  some_noopt_ptr = &some_stack_space;
  memset(some_stack_space, 0, sizeof(kStackOffset));

  must_getcontext(place);
  if (!*first_time) {
    body();
    SimpleFiber::SwapInto(nullptr);
    abort();
  }
}

void SimpleFiber::thread_body(SimpleFiber* it, bool *done) {
  bool first_time = true;

  no_inline([&] () {
      get_context_and_switch(&it->context_, &first_time, it->body_);
    });

  first_time = false;

  no_inline([&] () {
      park_thread(&it->m_, &it->c_, &it->shutdown_flag, done);
    });
  some_noopt_ptr = &first_time;
}

SimpleFiber::SimpleFiber(const std::function<void ()>& body) : body_(body) {
  bool done = false;

  thread_.reset(new std::thread(&SimpleFiber::thread_body, this, &done));

  {
    std::unique_lock<std::mutex> g(m_);
    while (!done) {
      c_.wait(g);
    }
  }
}

SimpleFiber::~SimpleFiber() {
  {
    std::unique_lock<std::mutex> g(m_);
    shutdown_flag = true;
    c_.notify_all();
  }
  thread_->join();
}

void SimpleFiber::SwapInto(SimpleFiber* target) {
  SimpleFiber* old = ::current;
  ucontext_t* target_context = fibers_context(target);
  ucontext_t* old_context = fibers_context(old);

  ::current = target;
  must_swapcontext(old_context, target_context);
}
