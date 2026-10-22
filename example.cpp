#include "sktrace.h"

#include <cinttypes>
#include <cstdio>
#include <thread>
#include <vector>

// Inlined into both callers below, so its tracepoint becomes two patch sites
// sharing one descriptor.
static inline int step(int x) {
  SKTRACE("step");
  return x * 3 + 1;
}

[[gnu::noinline]] int work(int n) {
  int acc = 0;
  for (int i = 0; i < n; i++) {
    SKTRACE("iter");
    acc = step(acc + i);
  }
  return acc;
}

[[gnu::noinline]] int more_work(int n) {
  int acc = 1;
  for (int i = 0; i < n; i++)
    acc = step(acc ^ i);
  return acc;
}

int main(int argc, char **argv) {
  const char *out = argc > 1 ? argv[1] : "trace.bin";
  sktrace::init_buffer(1 << 16);
  sktrace::init();

  for (const auto &d : sktrace::events()) {
    int sites = 0;
    for (auto *s = d.site; s; s = s->next)
      sites++;
    std::printf("event %016" PRIx64 " %-8s sites=%d first=%p\n", d.id, d.name, sites,
                d.site ? d.site->code : nullptr);
  }

  work(4);                                          // disabled: nothing logged
  SKTRACE("phase");

  sktrace::enable("*");
  SKTRACE("phase");
  work(3);
  more_work(2);

  // Toggle while other threads are executing the sites.
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; t++)
    threads.emplace_back([t] {
      for (int i = 0; i < 2000; i++)
        work(t + 1);
    });
  for (int i = 0; i < 200; i++) {
    sktrace::disable("iter");
    sktrace::enable("iter");
  }
  for (auto &t : threads)
    t.join();

  sktrace::disable("*");
  work(4);                                          // disabled again

  std::printf("dropped=%" PRIu64 "\n", sktrace::dropped());
  FILE *fp = std::fopen(out, "wb");
  if (!fp || !sktrace::write_dump(fp))
    return 1;
  std::fclose(fp);
  return 0;
}
