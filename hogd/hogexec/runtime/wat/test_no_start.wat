(module
  (memory 1 1)

  (func $foo 
    nop
  )

  (start $foo)

  (export "memory" (memory 0))
)