// Print an sktrace dump.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

struct Event {
  uint64_t id;
  uint64_t timestamp;
};

int main(int argc, char **argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s trace.bin [-q]\n", argv[0]);
    return 2;
  }
  bool quiet = argc > 2 && std::strcmp(argv[2], "-q") == 0;
  FILE *fp = std::fopen(argv[1], "rb");
  if (!fp)
    return 1;
  auto get = [fp](void *p, size_t n) { return std::fread(p, 1, n, fp) == n; };

  char magic[8];
  uint64_t freq;
  uint32_t count;
  if (!get(magic, 8) || std::memcmp(magic, "SKTRACE2", 8) != 0 || !get(&freq, 8) ||
      !get(&count, 4)) {
    std::fprintf(stderr, "bad header\n");
    return 1;
  }
  std::map<uint64_t, std::string> names;
  for (uint32_t i = 0; i < count; i++) {
    uint64_t id;
    uint32_t len;
    if (!get(&id, 8) || !get(&len, 4))
      return 1;
    std::string name(len, '\0');
    if (!get(name.data(), len))
      return 1;
    names[id] = name;
  }

  uint64_t n;
  if (!get(&n, 8))
    return 1;
  std::vector<Event> events(n);
  if (!get(events.data(), n * sizeof(Event)))
    return 1;

  std::map<uint64_t, uint64_t> counts;
  for (const Event &e : events) {
    auto it = names.find(e.id);
    if (it == names.end()) {
      std::fprintf(stderr, "unknown event ID %016llx\n", (unsigned long long)e.id);
      return 1;
    }
    counts[e.id]++;
    if (!quiet)
      std::printf("%12.3fus %s\n",
                  1e6 * double(e.timestamp - events[0].timestamp) / double(freq),
                  it->second.c_str());
  }
  for (const auto &[id, name] : names)
    std::printf("# %016llx %s: %llu events\n", (unsigned long long)id, name.c_str(),
                (unsigned long long)counts[id]);
  return 0;
}
