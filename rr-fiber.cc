// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "rr-fiber.h"

#include <assert.h>

static std::vector<PooledFiber*> pool;

PooledFiber::PooledFiber() : SimpleFiber([this] () {pooled_body();}) {
}

PooledFiber::~PooledFiber() {}

void PooledFiber::pooled_body() {
  while (true) {
    real_body_();
    pool.push_back(this);
    SimpleFiber::SwapInto(nullptr);
  }
}

PooledFiber* PooledFiber::New(const std::function<void ()>& body) {
  if (pool.empty()) {
    pool.push_back(new PooledFiber());
  }

  auto fiber = *pool.rbegin();
  pool.pop_back();
  fiber->real_body_ = body;

  return fiber;
}

RRFiber* RRFiber::runnable_list{};
RRFiber** RRFiber::runnable_list_end = &runnable_list;
RRFiber* RRFiber::current{};
RRFiber* RRFiber::being_joined{};

RRFiber::RRFiber(const std::function<void ()>& body) {
  engine_ = PooledFiber::New([body, this] () {
      body();
      finished = true;
      if (being_joined != this) {
        Yield();
        assert(being_joined == this);
      }
      being_joined = nullptr;
      current = nullptr;
      actually_joined = true;
      // pooled fiber does SimpleFiber::SwapInto(nullptr);
    });
  append_runnable(this);
}

RRFiber::~RRFiber() {
  assert(finished);
  assert(joined);
  assert(actually_joined);
}

void RRFiber::Join() {
  assert(current == nullptr);
  assert(being_joined == nullptr);
  assert(!joined);
  if (!actually_joined) {
    being_joined = this;
    current = this;
    if (!finished) {
      remove_runnable(this);
    }
    assert(engine_ != nullptr);
    SimpleFiber::SwapInto(engine_);
    assert(finished);
  }
  joined = true;
  return;
}

void RRFiber::Yield() {
  RRFiber *old_current = current;
  assert(old_current != nullptr);

  if (!old_current->finished) {
    append_runnable(old_current);
  }

  RRFiber *new_current = pop_runnable();

  if (new_current == old_current) {
    return;
  }

  current = new_current;
  if (new_current == nullptr) {
    assert(runnable_list == nullptr);
    SimpleFiber::SwapInto(nullptr);
  } else {
    assert(new_current->engine_ != nullptr);
    SimpleFiber::SwapInto(new_current->engine_);
  }
  assert(current == old_current);
}
