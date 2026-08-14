#include "partition_assignment.hpp"

#include <cctype>
#include <charconv>

namespace matchbox::service {

std::vector<std::int32_t> partitions_for(int ordinal, int replica_count, int partition_count) {
  std::vector<std::int32_t> partitions;
  if (ordinal < 0 || replica_count <= 0 || partition_count <= 0 || ordinal >= replica_count) {
    return partitions;
  }

  const int base = partition_count / replica_count;
  const int remainder = partition_count % replica_count;

  // The first `remainder` ordinals take one extra partition each, so `start` has to account
  // for the extras already handed out below this ordinal.
  const int start = ordinal * base + (ordinal < remainder ? ordinal : remainder);
  const int count = base + (ordinal < remainder ? 1 : 0);

  partitions.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    partitions.push_back(static_cast<std::int32_t>(start + i));
  }
  return partitions;
}

std::optional<int> ordinal_from_pod_name(std::string_view pod_name) {
  std::size_t end = pod_name.size();
  std::size_t begin = end;
  while (begin > 0 && std::isdigit(static_cast<unsigned char>(pod_name[begin - 1])) != 0) {
    --begin;
  }
  if (begin == end) {
    return std::nullopt;
  }

  int value = 0;
  const char* first = pod_name.data() + begin;
  const char* last = pod_name.data() + end;
  const std::from_chars_result parsed = std::from_chars(first, last, value);
  if (parsed.ec != std::errc{} || parsed.ptr != last) {
    return std::nullopt;
  }
  return value;
}

}  // namespace matchbox::service
