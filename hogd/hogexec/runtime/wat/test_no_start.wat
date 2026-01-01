(module
  (memory 1 1)

  (func (export "start")
    ;; do nothing
    nop
  )

  (export "memory" (memory 0))
)