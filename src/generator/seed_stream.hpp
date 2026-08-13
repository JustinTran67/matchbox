#pragma once

#include <cstdint>

namespace matchbox {

// splitmix64. Spreads one caller-supplied seed into independent streams, so that state
// which must evolve at a fixed rate never shares a generator with state that is drawn
// from at a variable rate.
inline std::uint64_t nth_stream(std::uint64_t seed, int index) {
  std::uint64_t state = seed;
  std::uint64_t value = 0;
  for (int i = 0; i <= index; ++i) {
    state += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    value = z ^ (z >> 31);
  }
  return value;
}

}  // namespace matchbox
