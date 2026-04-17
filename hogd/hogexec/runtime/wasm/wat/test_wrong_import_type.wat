(module
  (import "marmot" "syscall" (func $sys (param i64) (result i32)))

  (func (export "invoke")
    i64.const 0
    call $sys
    drop
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
