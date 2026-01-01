Log of (unsuccessful) changes to wasmtime API usage:

1. Use Module::deserialize_raw() to avoid a bit of overhead over deserialize()
    - This requires using a custom memory publish mechanism (or running on an arch without virtual memory).

1. Cache one (per-process) copy of each code block, which we load into modules via Module::deserialize_raw
    - Goal: avoid excessive mprotects
    - Problem: There is a global data structure in wasmtime used for signal handlers 
    (https://github.com/bytecodealliance/wasmtime/blob/main/crates/wasmtime/src/runtime/module/registry.rs#L312)
    which registers each new block of code when it is loaded into an Engine.
    This global structure assumes each block has a unique address
    This check is present even if signals_based_traps is disabled in the config.

Next plan:
    - Per-engine cache of InstancePre objects.  This should not require the custom code allocator.
