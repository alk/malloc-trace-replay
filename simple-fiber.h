// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef SIMPLE_FIBER_H
#define SIMPLE_FIBER_H

#include <functional>
#include <thread>
#include <memory>
#include <mutex>
#include <condition_variable>

#include <ucontext.h>

class SimpleFiber {
public:
  explicit SimpleFiber(const std::function<void ()>& body);
  ~SimpleFiber();

  static SimpleFiber* current();
  static SimpleFiber* SwapInto(SimpleFiber *target);
private:
  static void thread_body(SimpleFiber *it, bool* done);
  static ucontext_t* fibers_context(SimpleFiber* it);

  ucontext_t context_;

  std::mutex m_{};
  std::condition_variable c_{};

  std::function<void ()> body_;
  std::unique_ptr<std::thread> thread_;
  bool shutdown_flag{};
};

#endif
