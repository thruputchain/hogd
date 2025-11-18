
#include "x23/nmvec.h"

namespace x23 {

PerCpuSet<MVar<AllocCache>> memcaches{(init_cpumaps(), *llc_map), populate};

void
foo()
{
  NMVec<int>::iterator_type it;
}


} // namespace x23
