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

static __attribute__((noinline)) void setup_context(ucontext_t* place,
                                                    ucontext_t* tmp,
                                                    bool *to_clear,
                                                    const std::function<void ()>& body) {
  constexpr int kStackSpace = 4 << 10;
  char some_stack_space[kStackSpace];

  memset(some_stack_space, 0, sizeof(kStackSpace));
  stack_space_noopt = &some_stack_space;

  *to_clear = false;
  memset(place, 0, sizeof(*place));
  must_getcontext(place);
  must_swapcontext(place, tmp);

  body();

  SimpleFiber::SwapInto(nullptr);
}

static void sc(ucontext_t* place, bool* volatile flag,
               const std::function<void ()>& body) {
  ucontext_t tmp;
  memset(&tmp, 0, sizeof(tmp));
  must_getcontext(&tmp);
  while (*flag) {
    setup_context(place, &tmp, flag, body);
  }
}

namespace simple_fiber_hidden {

void (* volatile set_context_fn)(ucontext_t* place,
                                 bool * volatile to_clear,
                                 const std::function<void ()>& body) = sc;
} // namespace simple_fiber_hidden

void SimpleFiber::thread_body(SimpleFiber* it, bool *done) {
  bool in_setup = true;
  simple_fiber_hidden::set_context_fn(
    &it->context_, &in_setup, it->body_);

  std::unique_lock<std::mutex> g(it->m_);
  *done = true;
  it->c_.notify_all();
  while (!it->shutdown_flag) {
    it->c_.wait(g);
  }
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

SimpleFiber* SimpleFiber::SwapInto(SimpleFiber* target) {
  SimpleFiber* old = ::current;
  ucontext_t* target_context = fibers_context(target);
  ucontext_t* old_context = fibers_context(old);

  ::current = target;
  must_swapcontext(old_context, target_context);
}
