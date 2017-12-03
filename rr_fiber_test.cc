// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "rr-fiber.h"

#include <stdio.h>
#include <assert.h>
#include <vector>
#include <memory>

constexpr int kSwitchLimit = 256 << 20;
volatile int count;

static void fiber_body(void) {
  auto ptr = __builtin_alloca(1);
  printf("fiber %p\n", ptr);
  while (count < kSwitchLimit) {
    count++;
    RRFiber::Yield();
  }
  printf("fiber %p done!\n", ptr);
}

int main() {
  std::vector<std::unique_ptr<RRFiber>> fibers;

  for (int i = 0; i < 16; i++) {
    auto f = new RRFiber([] () {
        fiber_body();
      });
    fibers.emplace_back(f);
  }

  printf("about to switch into first fiber\n");

  for (auto &p : fibers) {
    p->Join();
  }

  printf("count = %d\n", count);
}
