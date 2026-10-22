#include "sktrace.h"

#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <utility>
#include <vector>

extern "C" {
extern sktrace::EventDescriptor __start_sktrace_events[];
extern sktrace::EventDescriptor __stop_sktrace_events[];
extern sktrace::PatchSite __start_sktrace_sites[];
extern sktrace::PatchSite __stop_sktrace_sites[];
}

namespace sktrace {
namespace {

constexpr uint32_t kNop = 0xd503201f;

uint32_t encode_branch(const void *from, const void *to) {
  intptr_t off = reinterpret_cast<intptr_t>(to) - reinterpret_cast<intptr_t>(from);
  // B has a +/-128MB range; the target is always within the same function.
  if (off & 3 || off < -(intptr_t{1} << 27) || off >= (intptr_t{1} << 27))
    std::abort();
  return 0x14000000u | ((static_cast<uint32_t>(off) >> 2) & 0x03ffffffu);
}

std::once_flag init_once;
std::mutex patch_mutex;

// Replace a single instruction. Other threads may be executing it; they will
// observe either the old or the new instruction, both of which are valid.
void patch_insn(void *addr, uint32_t insn) {
  auto *code = static_cast<uint32_t *>(addr);
  if (__atomic_load_n(code, __ATOMIC_RELAXED) == insn)
    return;

  static const uintptr_t page_size = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
  void *page = reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(code) & ~(page_size - 1));

  // The page must remain executable throughout as other threads may be
  // running code on it.
  if (mprotect(page, page_size, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
    std::abort();
  __atomic_store_n(code, insn, __ATOMIC_RELAXED); // single-copy atomic
  mprotect(page, page_size, PROT_READ | PROT_EXEC);

  // DC CVAU; DSB ISH; IC IVAU; DSB ISH; ISB. The IC IVAU is broadcast, so
  // other cores will eventually fetch the new instruction.
  __builtin___clear_cache(reinterpret_cast<char *>(code),
                          reinterpret_cast<char *>(code + 1));
}

void set_enabled(EventDescriptor &d, bool on) {
  for (PatchSite *s = d.site; s; s = s->next)
    patch_insn(s->code, on ? encode_branch(s->code, s->target) : kNop);
  d.enabled.store(on, std::memory_order_relaxed);
}

size_t set_matching(std::string_view name, bool on) {
  init();
  std::lock_guard lock(patch_mutex);
  size_t n = 0;
  for (EventDescriptor &d : events()) {
    if (name == "*" || name == d.name) {
      set_enabled(d, on);
      n++;
    }
  }
  return n;
}

// Global buffer ---------------------------------------------------------------

Event *buffer = nullptr;
size_t buffer_capacity = 0;
std::atomic<size_t> buffer_head{0};
std::atomic<uint64_t> buffer_dropped{0};

uint64_t counter_frequency() {
  uint64_t f;
  asm volatile("mrs %0, cntfrq_el0" : "=r"(f));
  return f;
}

} // namespace

std::span<EventDescriptor> events() {
  return {__start_sktrace_events, __stop_sktrace_events};
}

void init() {
  std::call_once(init_once, [] {
    for (PatchSite *s = __start_sktrace_sites; s != __stop_sktrace_sites; s++) {
      if (*static_cast<uint32_t *>(s->code) != kNop)
        std::abort(); // corrupt site table
      s->next = s->desc->site;
      s->desc->site = s;
    }

    std::vector<std::pair<uint64_t, std::string_view>> ids;
    for (const EventDescriptor &d : events())
      ids.emplace_back(d.id, d.name);
    std::sort(ids.begin(), ids.end());
    for (size_t i = 1; i < ids.size(); i++) {
      if (ids[i].first == ids[i - 1].first && ids[i].second != ids[i - 1].second) {
        std::fprintf(stderr, "sktrace: events \"%.*s\" and \"%.*s\" have the same ID\n",
                     int(ids[i - 1].second.size()), ids[i - 1].second.data(),
                     int(ids[i].second.size()), ids[i].second.data());
        std::abort();
      }
    }
  });
}

size_t enable(std::string_view name) { return set_matching(name, true); }
size_t disable(std::string_view name) { return set_matching(name, false); }

void init_buffer(size_t capacity) {
  buffer = new Event[capacity];
  buffer_capacity = capacity;
  buffer_head.store(0, std::memory_order_relaxed);
}

uint64_t dropped() { return buffer_dropped.load(std::memory_order_relaxed); }

void detail::record(const EventDescriptor &desc) {
  uint64_t ts;
  asm volatile("mrs %0, cntvct_el0" : "=r"(ts));
  size_t i = buffer_head.fetch_add(1, std::memory_order_relaxed);
  if (i >= buffer_capacity) {
    buffer_dropped.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  buffer[i] = Event{desc.id, ts};
}

bool write_dump(FILE *fp) {
  auto put = [fp](const void *p, size_t n) { return std::fwrite(p, 1, n, fp) == n; };
  bool ok = put("SKTRACE2", 8);

  uint64_t freq = counter_frequency();
  ok = ok && put(&freq, sizeof(freq));

  uint32_t count = static_cast<uint32_t>(events().size());
  ok = ok && put(&count, sizeof(count));
  for (const EventDescriptor &d : events()) {
    uint32_t len = static_cast<uint32_t>(std::strlen(d.name));
    ok = ok && put(&d.id, sizeof(d.id)) && put(&len, sizeof(len)) && put(d.name, len);
  }

  uint64_t n = std::min(buffer_head.load(std::memory_order_acquire), buffer_capacity);
  ok = ok && put(&n, sizeof(n)) && (n == 0 || put(buffer, n * sizeof(Event)));
  return ok;
}

} // namespace sktrace
