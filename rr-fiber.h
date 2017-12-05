// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef RR_FIBER_H
#define RR_FIBER_H

#include "simple-fiber.h"

#include <vector>
#include <memory>
#include <functional>

#include <assert.h>

class PooledFiber : public SimpleFiber {
public:
  static PooledFiber* New(const std::function<void ()>& body);
private:
  PooledFiber();
  ~PooledFiber();

  void pooled_body();

  std::function<void ()> real_body_;
};

class RRFiber {
public:
  RRFiber(const std::function<void ()>& body);
  virtual ~RRFiber();

  static void Yield();
  static RRFiber* Current() {
    return current;
  }
  virtual void Join();
private:
  PooledFiber *engine_;
  RRFiber *next_{};
  RRFiber **pprev_{};
  bool finished{};
  bool joined{};
  bool actually_joined{};

  static RRFiber *current;
  static RRFiber *runnable_list;
  static RRFiber **runnable_list_end;
  static RRFiber *being_joined;

  static void append_runnable(RRFiber* f) {
    assert(f->next_ == nullptr);
    assert(*runnable_list_end == nullptr);
    *runnable_list_end = f;
    f->pprev_ = runnable_list_end;
    runnable_list_end = &f->next_;
  }

  static void remove_runnable(RRFiber *f) {
    assert(f->pprev_ != nullptr);
    *f->pprev_ = f->next_;
    if (f->next_) {
      f->next_->pprev_ = f->pprev_;
    } else {
      runnable_list_end = f->pprev_;
    }
    f->next_ = nullptr;
    f->pprev_ = nullptr;
  }

  static RRFiber* pop_runnable() {
    RRFiber *rv = runnable_list;
    if (rv == nullptr) {
      return rv;
    }
    remove_runnable(rv);
    return rv;
  }
};

#endif
