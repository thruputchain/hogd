(module
  ;; Memory of exactly 1 page (64KB = 65536 bytes)
  (memory 1 1)
  (export "memory" (memory 0))
  ;; Data segment at offset 70000, beyond the 1-page memory.
  ;; This fails during instantiation with MemoryOutOfBounds (not OutOfFuel),
  ;; triggering the panic in WasmtimeRuntime::new (audit issue #1).
  (data (i32.const 70000) "hello")
)
