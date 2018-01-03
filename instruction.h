// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef INSTRUCTION_H
#define INSTRUCTION_H
#include <stdint.h>
#include <string.h>

struct Instruction {
  enum Type {
    MALLOC = 0,
    FREE = 1,
    FREE_SIZED = 2,
    MEMALIGN = 3,
    REALLOC = 4,
    SWITCH_THREAD = 5,
    KILL_THREAD = 6,
    SET_TS_CPU = 7
  };
  uint64_t type:8;
  uint64_t reg:56;
  union {
    struct {
      uint64_t size;
      uint64_t align;
    } malloc;
    struct {
      uint64_t new_reg;
      uint64_t new_size;
    } realloc;
    struct {
      uint64_t thread_id;
    } switch_thread;
    struct {
      uint64_t ts;
      uint64_t cpu;
    } ts_cpu;
  };

  explicit Instruction(uint64_t type) {
    memset(this, 0, sizeof(*this));
    this->type = type;
  }
};


#endif
