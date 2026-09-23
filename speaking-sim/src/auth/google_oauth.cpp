#include "sim/store.hpp"

//Google sign-in: /auth/login, /auth/callback and the token exchange.
//Listed in CMakeLists.txt from phase 0 so the build shape is settled before
//anything depends on it - the sources list is explicit rather than a glob, and
//a file added later is an unresolved external at link rather than a clear error.

namespace sim {
namespace auth {

}  // namespace auth
}  // namespace sim
