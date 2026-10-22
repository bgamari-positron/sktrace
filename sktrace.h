// sktrace: static-key tracepoints for user space on AArch64 Linux.
//
// Each SKTRACE() site compiles to a single NOP on the hot path. Enabling an
// event rewrites that NOP into a B to an out-of-line block which appends an
// Event record to a shared buffer. NOP <-> B is one of the instruction pairs
// the Arm ARM permits to be modified while other threads may be executing it,
// so (dis)arming needs no stop-the-world.
//
// Usage:
//
//   SKTRACE("alloc");
//   ...
//   sktrace::init_buffer(1 << 16);
//   sktrace::enable("alloc");
//   ...
//   sktrace::write_dump(fp);
//
// Requirements: C++20, GCC or Clang, AArch64 Linux, linking with a linker that
// provides __start_/__stop_ symbols for C-identifier-named sections (all of
// GNU ld, gold, lld and mold do).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

#if !defined(__aarch64__)
#error "sktrace currently only supports AArch64"
#endif

namespace sktrace {

// 64-bit FNV-1a; an event's ID is the hash of its name.
constexpr uint64_t hash_name(std::string_view name) {
  uint64_t h = 0xcbf29ce484222325;
  for (char c : name)
    h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3;
  return h;
}

// A logged event.
struct Event {
  uint64_t id;            // hash_name(name)
  uint64_t timestamp;     // raw CNTVCT_EL0 ticks
};
static_assert(sizeof(Event) == 16);

struct EventDescriptor;

// One per code modification point. Emitted by the inline asm in SKTRACE into
// the `sktrace_sites` section. The compiler may duplicate a site (inlining,
// unrolling, cloning), in which case several PatchSites share a descriptor and
// are chained through `next`.
struct PatchSite {
  void *code;             // address of the NOP
  void *target;           // out-of-line block the NOP becomes a branch to
  EventDescriptor *desc;
  PatchSite *next;        // populated by init()
};
static_assert(sizeof(PatchSite) == 32);

// One per SKTRACE use. These form the global `sktrace_events` array.
struct EventDescriptor {
  const char *name;
  uint64_t id;
  PatchSite *site;        // first code modification point; populated by init()
  std::atomic<bool> enabled;

  consteval EventDescriptor(const char *n)
      : name(n), id(hash_name(n)), site(nullptr), enabled(false) {}
};

// The global descriptor array.
std::span<EventDescriptor> events();

// Link patch sites to their descriptors and check that no two distinct names
// hash to the same ID. Idempotent and thread-safe; called implicitly by
// enable()/disable().
void init();

// Arm or disarm every event named `name` ("*" matches all). Returns the number
// of descriptors affected.
size_t enable(std::string_view name);
size_t disable(std::string_view name);

// Allocate the global buffer of `capacity` events. Must be called before
// enabling events; events fired with no buffer, or once it is full, are
// counted in dropped().
void init_buffer(size_t capacity);
uint64_t dropped();

// Write a self-describing dump:
//
//   "SKTRACE2"
//   u64 counter_frequency
//   u32 descriptor_count, then per descriptor { u64 id; u32 name_length; name }
//   u64 event_count, then event_count Events
//
// Must not race with events being emitted.
bool write_dump(FILE *fp);

namespace detail {
// Kept out of line so that the armed path costs one taken branch and a call.
[[gnu::cold, gnu::noinline]] void record(const EventDescriptor &desc);
} // namespace detail

} // namespace sktrace

// The descriptor is a constant-initialized function-local static placed in
// the `sktrace_events` section. The asm goto emits the NOP and a PatchSite
// recording (NOP address, out-of-line target, descriptor). After inlining the
// lambda, the compiler lowers `if (lambda())` to a fall-through over the NOP
// with the record() call placed in the out-of-line `taken` block.
#define SKTRACE(NAME)                                                          \
  do {                                                                         \
    [[gnu::section("sktrace_events"), gnu::used]] static constinit             \
        ::sktrace::EventDescriptor sktrace_desc_{NAME};                        \
    if ([]() __attribute__((always_inline)) -> bool {                          \
          asm goto("1: nop\n\t"                                                \
                   ".pushsection sktrace_sites, \"aw\"\n\t"                    \
                   ".balign 8\n\t"                                             \
                   ".quad 1b, %l[taken], %c0, 0\n\t"                           \
                   ".popsection"                                               \
                   :                                                           \
                   : "Si"(&sktrace_desc_)                                      \
                   :                                                           \
                   : taken);                                                   \
          return false;                                                        \
        taken:                                                                 \
          return true;                                                         \
        }()) [[unlikely]]                                                      \
      ::sktrace::detail::record(sktrace_desc_);                                \
  } while (0)
