
%#include "types.hh"

namespace hogd {

const kCoordPort = 10078;

struct StoragePrefix {
  AddressAndKey prefix;
  unsigned int length;
};

program CoordProg {
  version CoordV1 {
    void null(void) = 0;
  } = 1;
} = 0x1d88d309;

} // namespace hogd
