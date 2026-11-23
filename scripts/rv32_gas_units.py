"""Shared units for opcode and syscall schedule proposals.

The committed schedule uses one gas per baseline instruction. A different
resolution is an explicit proposal to rescale BOTH schedules and gas limits.
"""

import math
import statistics

DEFAULT_RESOLUTION = 1


def baseline_ns(data):
    costs = sorted(cell["ns_saturated"] for cell in data.get("opcodes", [])
                   if cell.get("usable") and not cell.get("pair")
                   and cell.get("ns_saturated", 0.0) > 0.0)
    if costs:
        return statistics.median(costs)
    return data.get("baseline", {}).get("ns_per_filler_instruction", 0.0)


def gas_unit_ns(baseline, resolution):
    if not isinstance(resolution, int) or resolution <= 0:
        raise ValueError("gas resolution must be a positive integer")
    if not math.isfinite(baseline) or baseline <= 0:
        raise ValueError("gas baseline must be finite and positive")
    return baseline / resolution
