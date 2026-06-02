(module
  (import "notmarmot" "something" (func $sys (param i32 i32 i32 i32 i32 i32 i32 i32) (result i32)))

  (func (export "invoke")
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    i32.const 0
    call $sys
    drop
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
