#pragma once

namespace matchbox {

// Reports how matchbox_core itself was compiled, not the calling translation unit. The
// benchmark needs this because assertions inside the engine are what would distort its
// timings, and those are governed by the library's own flags.
bool assertions_enabled();

}  // namespace matchbox
