// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef ACTUAL_REPLAY_H
#define ACTUAL_REPLAY_H
#include <assert.h>
#include <functional>
#include <stdint.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <memory>

#include "id_tree.h"

struct Instruction {
  static constexpr uint64_t kMalloc = 0;
  static constexpr uint64_t kFree   = 1;
  static constexpr uint64_t kRealloc = 2;

  uint64_t type:8;
  uint64_t reg:56;
  uint64_t size{};
  uint64_t old_reg{};
  // alignment...

  static Instruction Malloc(int reg, uint64_t size) {
    Instruction rv;
    rv.type = kMalloc;
    rv.reg = reg;
    rv.size = size;
    return rv;
  }
  static Instruction Realloc(int reg, int old_reg, uint64_t new_size) {
    Instruction rv;
    rv.type = kRealloc;
    rv.reg = reg;
    rv.size = new_size;
    rv.old_reg = old_reg;
    return rv;
  }
  static Instruction Free(int reg) {
    Instruction rv;
    rv.type = kFree;
    rv.reg = reg;
    return rv;
  }
};

class ReplayDumper {
public:
  struct ThreadState {
    const uint64_t thread_id;
    bool live{true};
    bool used_in_this_chunk{};
    std::vector<Instruction> instructions;

    ThreadState(uint64_t thread_id)
      : thread_id(thread_id) {
    }
  };
  typedef std::function<int (const void *, size_t)> writer_fn_t;
  ReplayDumper(const writer_fn_t& writer_fn);
  ~ReplayDumper();

  void record_malloc(uint64_t thread_id, uint64_t tok, uint64_t size,
		     uint64_t timestamp);

  void record_free(uint64_t thread_id, uint64_t tok, uint64_t timestamp);

  void record_realloc(uint64_t thread_id, uint64_t old_tok, uint64_t timestamp,
                      uint64_t new_tok, uint64_t new_size);

  void record_death(uint64_t thread_id);

  bool has_allocated(uint64_t tok) {
    return allocated_.count(tok) != 0;
  }

  void flush_chunk();

private:
  ThreadState* find_thread(uint64_t thread_id);

  void after_record();

  writer_fn_t writer_fn_;
  IdTree ids_space_;
  // std::vector<bool> allocated_this_iteration;
  std::unordered_map<uint64_t, ThreadState> per_thread_instructions_;
  std::unordered_set<uint64_t> freed_this_iteration_;
  std::vector<ThreadState *> threads_in_this_chunk;
  // maps tok -> register number
  std::unordered_map<uint64_t, uint64_t> allocated_;
  int iteration_size{};

  std::unique_ptr<uint64_t[]> first_segment;
};

#endif
