// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#ifndef FD_INPUT_MAPPER_H
#define FD_INPUT_MAPPER_H
#include <stddef.h>

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

class FDInputMapper : public Mapper {
public:
  FDInputMapper(int fd);
  ~FDInputMapper() {}
  const char* GetBegin() override {
    return mmap_area_;
  }
  size_t Realize(const char* start, size_t len) override;
private:
  const int fd_;
  const char* mmap_area_;
  const char* last_start_;
  const char* last_end_;
  bool seen_eof_{};
};

#endif
