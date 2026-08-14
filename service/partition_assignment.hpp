#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

namespace matchbox::service {

// Which partitions a StatefulSet replica owns, derived purely from its ordinal.
//
// Ownership is static by construction rather than negotiated at runtime. Partitions are
// handed out in contiguous slices, with the remainder spread one apiece over the lowest
// ordinals, so for any replica_count the slices tile the partition range exactly: every
// partition is claimed by exactly one ordinal, with no gap and no overlap. That property
// is what the tests check directly, because it is the whole safety argument - an engine's
// book is single-owner mutable state, and two replicas claiming one partition would mean
// two divergent books for the same symbol.
std::vector<std::int32_t> partitions_for(int ordinal, int replica_count, int partition_count);

// Pulls the trailing integer off a StatefulSet pod name ("matchbox-engine-2" -> 2).
// Returns nothing if the name has no trailing integer, which is a misconfiguration the
// caller should refuse to start on rather than guess at.
std::optional<int> ordinal_from_pod_name(std::string_view pod_name);

}  // namespace matchbox::service
