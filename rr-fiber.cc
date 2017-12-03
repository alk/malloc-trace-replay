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
      if (being_joined == this) {
        current = nullptr;
      } else {
        Yield();
      }
      // pooled fiber does SimpleFiber::SwapInto(nullptr);
    });
  append_runnable(this);
}

RRFiber::~RRFiber() {
  assert(finished);
  assert(joined);
}

void RRFiber::Join() {
  assert(current == nullptr);
  if (!finished) {
    Yield();
    assert(finished);
  }
  joined = true;
  return;
}

void RRFiber::Yield() {
  RRFiber *old_current = current;
  if (old_current != nullptr && !old_current->finished) {
    append_runnable(old_current);
  }
  RRFiber *new_current = pop_runnable();
  if (new_current == old_current) {
    return;
  }
  current = new_current;
  if (new_current == nullptr) {
    SimpleFiber::SwapInto(nullptr);
  } else {
    SimpleFiber::SwapInto(new_current->engine_);
  }
  assert(current == old_current);
}
