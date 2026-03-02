(module
  ;; Memory of exactly 1 page (64KB = 65536 bytes)
  (memory 1 1)
  (export "memory" (memory 0))
  ;; Data segment at offset 70000, beyond the 1-page memory.
  ;; Passes validation and check_memory_limit, but fails during
  ;; instantiation with a non-OutOfFuel trap (audit issue #1).
  (data (i32.const 70000) "hello")
)
