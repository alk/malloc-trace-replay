// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "simple-fiber.h"

#include <stdio.h>
#include <assert.h>
#include <vector>
#include <memory>

constexpr int kSwitchLimit = 256 << 20;
volatile int count;

static void fiber_body(SimpleFiber *next) {
  auto ptr = __builtin_alloca(1);
  auto fib = SimpleFiber::current();
  printf("fiber %p, %p\n", ptr, fib);
  while (count < kSwitchLimit) {
    count++;
    SimpleFiber::SwapInto(next);
    assert(SimpleFiber::current() == fib);
  }
  printf("fiber %p done!\n", ptr);
}

int main() {
  std::vector<std::unique_ptr<SimpleFiber>> fibers;

  for (int i = 0; i < 16; i++) {
    auto f = new SimpleFiber([&fibers, i] () {
        // yes, this only works if we first switch here after vector
        // is complete growing
        fiber_body(fibers[(i+1) % fibers.size()].get());
      });
    fibers.emplace_back(f);
  }

  assert(SimpleFiber::current() == nullptr);

  printf("about to switch into first fiber\n");
  SimpleFiber::SwapInto(fibers[0].get());

  assert(SimpleFiber::current() == nullptr);

  printf("count = %d\n", count);
}
