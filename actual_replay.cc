// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include "actual_replay.h"

#include <capnp/message.h>
#include <capnp/serialize-packed.h>
#include "replay.capnp.h"

static constexpr int kFirstSegmentSize = 10 << 20;

ReplayDumper::ReplayDumper(const writer_fn_t& writer_fn) : writer_fn_(writer_fn) {
  first_segment.reset(new uint64_t[(kFirstSegmentSize + 7)/8]);
  memset(first_segment.get(), 0, kFirstSegmentSize);
}

ReplayDumper::~ReplayDumper() {}

ReplayDumper::ThreadState* ReplayDumper::find_thread(
    uint64_t thread_id) {
  auto pair = per_thread_instructions_.emplace(
    thread_id, ThreadState(thread_id));
  auto rv = &(pair.first->second);
  if (!rv->used_in_this_chunk) {
    rv->used_in_this_chunk = true;
    threads_in_this_chunk.push_back(rv);
  }
  return rv;
}

constexpr int kIterationSize = 4096;

void ReplayDumper::after_record() {
  iteration_size++;
  if (iteration_size < kIterationSize) {
    return;
  }

  flush_chunk();
}

void ReplayDumper::record_malloc(
  uint64_t thread_id, uint64_t tok, uint64_t size,
  uint64_t timestamp) {

  auto state = find_thread(thread_id);

  auto reg = ids_space_.allocate_id();
  allocated_[tok] = reg;
  state->instructions.push_back(Instruction::Malloc(reg, size));

  after_record();
}

void ReplayDumper::record_free(
  uint64_t thread_id, uint64_t tok, uint64_t timestamp) {

  auto state = find_thread(thread_id);

  assert(allocated_.count(tok) == 1);
  auto reg = allocated_[tok];
  allocated_.erase(tok);

  assert(!ids_space_.is_free_at(reg));
  freed_this_iteration_.insert(reg);

  state->instructions.push_back(Instruction::Free(reg));

  after_record();
}

void ReplayDumper::record_realloc(
  uint64_t thread_id, uint64_t tok, uint64_t timestamp,
  uint64_t new_tok, uint64_t new_size) {

  auto state = find_thread(thread_id);

  assert(allocated_.count(tok) == 1);
  auto reg = allocated_[tok];
  allocated_.erase(tok);

  assert(!ids_space_.is_free_at(reg));
  freed_this_iteration_.insert(reg);

  auto new_reg = ids_space_.allocate_id();
  allocated_[new_tok] = new_reg;

  state->instructions.push_back(Instruction::Realloc(new_reg, reg, new_size));

  after_record();
}

void ReplayDumper::record_death(uint64_t thread_id) {
  auto state = find_thread(thread_id);
  state->live = false;
}

class FunctionOutputStream : public ::kj::OutputStream {
public:
  FunctionOutputStream(const ReplayDumper::writer_fn_t& writer) : writer_(writer) {}
  ~FunctionOutputStream() = default;

  virtual void write(const void* buffer, size_t size) {
    writer_(buffer, size);
  }
private:
  const ReplayDumper::writer_fn_t &writer_;
};

void ReplayDumper::flush_chunk() {
  ::capnp::MallocMessageBuilder message(kj::arrayPtr(reinterpret_cast<capnp::word*>(first_segment.get()), kFirstSegmentSize));
  replay::Batch::Builder batch = message.initRoot<replay::Batch>();
  ::capnp::List<replay::ThreadChunk>::Builder threads = batch.initThreads(threads_in_this_chunk.size());

  int idx = 0;

  for (auto &thread_ptr : threads_in_this_chunk) {
    auto thread_id = thread_ptr->thread_id;
    auto &state = *thread_ptr;
    auto live = state.live;

    assert(state.used_in_this_chunk);

    replay::ThreadChunk::Builder tinfo = threads[idx++];

    tinfo.setThreadID(thread_id);
    tinfo.setLive(live);
    ::capnp::List<replay::Instruction>::Builder instructions = tinfo.initInstructions(state.instructions.size());

    int instruction_idx = 0;
    for (auto &instr : state.instructions) {
      auto builder = instructions[instruction_idx++];
      auto type = static_cast<replay::Instruction::Type>(instr.type);
      builder.setType(type);
      builder.setReg(instr.reg);
      builder.setSize(instr.size);
      if (type == replay::Instruction::Type::REALLOC) {
        builder.setOldReg(instr.old_reg);
      }
    }

    if (live) {
      state.instructions.clear();
      state.used_in_this_chunk = false;
    } else {
      per_thread_instructions_.erase(thread_id);
    }
  }

  {
    FunctionOutputStream os(writer_fn_);
    ::capnp::writePackedMessage(os, message);
  }

  for (auto reg : freed_this_iteration_) {
    ids_space_.free_id(reg);
  }

  threads_in_this_chunk.clear();
  freed_this_iteration_.clear();
  iteration_size = 0;
}
