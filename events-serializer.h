// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef EVENTS_SERIALIZER_H
#define EVENTS_SERIALIZER_H

#include <stdint.h>
#include <utility>
#include <stddef.h>

class EventsReceiver {
public:
  virtual ~EventsReceiver() noexcept;
  virtual void KillCurrentThread() = 0;
  virtual void SwitchThread(uint64_t thread_id) = 0;
  virtual void SetTS(uint64_t ts, uint64_t cpu) = 0;
  virtual void Malloc(uint64_t tok, uint64_t size) = 0;
  virtual void Memalign(uint64_t tok, uint64_t size, uint64_t align) = 0;
  virtual void Realloc(uint64_t old_tok,
                       uint64_t new_tok, uint64_t new_size) = 0;
  virtual void Free(uint64_t tok) = 0;
  virtual void FreeSized(uint64_t tok, uint64_t size) = 0;
  virtual void Barrier() = 0;
  virtual bool HasAllocated(uint64_t tok) = 0;
};

class Mapper {
public:
  virtual ~Mapper();
  virtual const char* GetBegin() = 0;
  virtual size_t Realize(const char* start, size_t len) = 0;
};

class ConstMapper final : public Mapper {
public:
  ConstMapper(const char* begin, size_t size) : begin_(begin), size_(size) {}
  ~ConstMapper();

  const char* GetBegin() override {return begin_;}
  size_t Realize(const char* start, size_t len) override {
    const char* end = start + len;
    if (end > begin_ + size_) {
      end = begin_ + size_;
    }
    return end - start;
  }
private:
  const char* begin_;
  size_t size_;
};

void SerializeMallocEvents(Mapper* mapper, EventsReceiver* receiver);

#endif
