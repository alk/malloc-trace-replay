// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil -*-
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <inttypes.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

#include <deque>
#include <functional>
#include <memory>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <time.h>
#include <sys/mman.h>
#include <signal.h>

#include "events-serializer.h"
#include "actual_replay.h"

static void pabort(const char *t) {
  perror(t);
  abort();
}

class PrintReceiver : public EventsReceiver {
public:
  PrintReceiver(EventsReceiver* target) : target_(target) {}
  ~PrintReceiver() {}

  void KillCurrentThread() {
    printf("KillCurrentThread\n");
    target_->KillCurrentThread();
  }
  void SwitchThread(uint64_t thread_id) {
    printf("SwitchThread(%zu)\n", (size_t)thread_id);
    target_->SwitchThread(thread_id);
  }
  void SetTS(uint64_t ts, uint64_t cpu) {
    printf("SetTS(%zu, %zu)\n", (size_t)ts, (size_t)cpu);
    target_->SetTS(ts, cpu);
  }
  void Malloc(uint64_t tok, uint64_t size) {
    printf("Malloc(%zu, %zu)\n", (size_t)tok, (size_t)size);
    target_->Malloc(tok, size);
  }
  void Memalign(uint64_t tok, uint64_t size, uint64_t align) {
    printf("Memalign(%zu, %zu, %zu)\n", (size_t)tok, (size_t)size, (size_t)align);
    target_->Memalign(tok, size, align);
  }
  void Realloc(uint64_t old_tok,
               uint64_t new_tok, uint64_t new_size) {
    printf("Realloc(%zu, %zu, %zu)\n",
           (size_t)old_tok, (size_t)new_tok, (size_t)new_size);
    target_->Realloc(old_tok, new_tok, new_size);
  }
  void Free(uint64_t tok) {
    printf("Free(%zu)\n", (size_t)tok);
    target_->Free(tok);
  }
  void FreeSized(uint64_t tok, uint64_t size) {
    printf("FreeSized(%zu, %zu)\n",
           (size_t)tok, (size_t)size);
    target_->FreeSized(tok, size);
  }
  void Barrier() {
    printf("Barrier\n");
    target_->Barrier();
  }

private:
  EventsReceiver* const target_;
};

class ReplayReceiver : public EventsReceiver {
public:
  ReplayReceiver(ReplayDumper* dumper) : dumper_(dumper) {}
  ~ReplayReceiver() {}

  virtual void KillCurrentThread();
  virtual void SwitchThread(uint64_t thread_id);
  virtual void SetTS(uint64_t ts, uint64_t cpu);
  virtual void Malloc(uint64_t tok, uint64_t size);
  virtual void Memalign(uint64_t tok, uint64_t size, uint64_t align);
  virtual void Realloc(uint64_t old_tok,
                       uint64_t new_tok, uint64_t new_size);
  virtual void Free(uint64_t tok);
  virtual void FreeSized(uint64_t tok, uint64_t size);
  virtual void Barrier();

private:
  ReplayDumper* dumper_;
  uint64_t current_thread_{};
};

void ReplayReceiver::KillCurrentThread() {
  dumper_->record_death(current_thread_);
}

void ReplayReceiver::SwitchThread(uint64_t thread_id) {
  current_thread_ = thread_id;
}

void ReplayReceiver::SetTS(uint64_t ts, uint64_t cpu) {
}

void ReplayReceiver::Malloc(uint64_t tok, uint64_t size) {
  dumper_->record_malloc(current_thread_, tok, size, 0);
}

void ReplayReceiver::Memalign(uint64_t tok, uint64_t size, uint64_t align) {
  dumper_->record_malloc(current_thread_, tok, size, 0);
}

void ReplayReceiver::Realloc(uint64_t old_tok,
                             uint64_t new_tok, uint64_t new_size) {
  dumper_->record_realloc(current_thread_, old_tok, 0, new_tok, new_size);
}

void ReplayReceiver::Free(uint64_t tok) {
  dumper_->record_free(current_thread_, tok, 0);
}

void ReplayReceiver::FreeSized(uint64_t tok, uint64_t size) {
  dumper_->record_free(current_thread_, tok, 0);
}

void ReplayReceiver::Barrier() {
  dumper_->flush_chunk();
}

int main(int argc, char **argv) {
  int fd = 0;
  if (argc < 3) {
    fprintf(stderr, "need 2 args\n");
    return 0;
  }

  fd = open(argv[1], O_RDONLY);
  if (fd < 0) {
    pabort("open");
  }

  int fd2 = open(argv[2], O_WRONLY|O_CREAT|O_TRUNC);
  if (fd2 < 0) {
    pabort("open");
  }

  struct stat st;
  int rv = fstat(fd, &st);
  if (rv < 0) {
    pabort("fstat");
  }

  auto pagesize = getpagesize();
  auto mmap_size = ((st.st_size + pagesize - 1) & ~(pagesize - 1)) + pagesize;
  auto mmap_result = mmap(nullptr, mmap_size,
                          PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS, 0, 0);
  if (mmap_result == MAP_FAILED) {
    pabort("mmap");
  }
  const char* mmap_area = static_cast<const char *>(mmap_result);
  mmap_result = mmap(mmap_result, st.st_size,
                     PROT_READ, MAP_SHARED|MAP_FIXED, fd, 0);
  if (mmap_result == MAP_FAILED) {
    pabort("mmap");
  }

  ReplayDumper::writer_fn_t writer = [fd2] (const void *buf, size_t sz) -> int{
    int rv = write(fd2, buf, sz);
    if (rv != sz) {
      pabort("write");
    }
    return 0;
  };

  ReplayDumper dumper(writer);
  ReplayReceiver receiver(&dumper);
  PrintReceiver printer(&receiver);

  signal(SIGINT, [](int dummy) {
      exit(0);
    });

  // SerializeMallocEvents(mmap_area, mmap_area + st.st_size, &printer);
  SerializeMallocEvents(mmap_area, mmap_area + st.st_size, &receiver);
}
