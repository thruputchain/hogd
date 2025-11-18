
#include "x23/nmvec.h"

namespace x23 {

PerCpuSet<MVar<AllocCache>> memcaches{(init_cpumaps(), *llc_map), populate};

void
foo()
{
  NMVec<int> v;
  v.resize(0x1'0000);
}

} // namespace x23
