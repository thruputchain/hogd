
#include "x23/nmvec.h"

namespace x23 {

PerCpuSet<MVar<AllocCache>> memcaches{(init_cpumaps(), *llc_map), populate};

} // namespace x23
