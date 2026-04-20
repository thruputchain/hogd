(module
  (import "notmarmot" "something" (func $sys (param i64 i64 i64 i64 i64 i64 i64 i64) (result i32)))

  (func (export "invoke")
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    i64.const 0
    call $sys
    drop
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
