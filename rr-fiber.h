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
  ~RRFiber();

  static void Yield();
  void Join();
private:
  PooledFiber *engine_;
  RRFiber *next_{};
  bool finished{};
  bool joined{};

  static RRFiber *current;
  static RRFiber *runnable_list;
  static RRFiber **runnable_list_end;
  static RRFiber *being_joined;

  static void append_runnable(RRFiber* f) {
    *runnable_list_end = f;
    runnable_list_end = &f->next_;
  }

  static RRFiber* pop_runnable() {
    RRFiber *rv = runnable_list;
    if (rv == nullptr) {
      return rv;
    }
    runnable_list = rv->next_;
    rv->next_ = nullptr;
    if (runnable_list == nullptr) {
      assert(runnable_list_end == &rv->next_);
      runnable_list_end = &runnable_list;
    }
    return rv;
  }
};

#endif
