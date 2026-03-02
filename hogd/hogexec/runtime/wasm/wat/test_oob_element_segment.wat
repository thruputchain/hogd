(module
  ;; Keep memory export valid so check_memory_limit passes.
  (memory 1 1)
  (export "memory" (memory 0))

  ;; Single-slot table.
  (table 1 funcref)
  (func)

  ;; Active element segment starts at offset 2, so writing 1 element is OOB
  ;; at instantiation time.
  (elem (i32.const 2) func 0)
)
