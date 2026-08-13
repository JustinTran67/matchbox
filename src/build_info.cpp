#include "build_info.hpp"

namespace matchbox {

bool assertions_enabled() {
#ifdef NDEBUG
  return false;
#else
  return true;
#endif
}

}  // namespace matchbox
