(module
  (import "test" "arg0" (func $call0 (result i32)))
  (import "test" "arg1" (func $call1 (param i64) (result i32)))
  (import "test" "arg2" (func $call2 (param i64 i64) (result i32)))
  (import "test" "arg3" (func $call3 (param i64 i64 i64) (result i32)))
  (import "test" "arg4" (func $call4 (param i64 i64 i64 i64) (result i32)))
  (import "test" "arg5" (func $call5 (param i64 i64 i64 i64 i64) (result i32)))
  (import "test" "arg6" (func $call6 (param i64 i64 i64 i64 i64 i64) (result i32)))
  (import "test" "arg7" (func $call7 (param i64 i64 i64 i64 i64 i64 i64) (result i32)))
  (import "test" "arg8" (func $call8 (param i64 i64 i64 i64 i64 i64 i64 i64) (result i32)))


  (func (export "calltest0") (result i32)
    (call $call0)
  )

  (func (export "calltest1") (result i32)
    (call $call1 (i64.const 1))
  )

  (func (export "calltest2") (result i32)
    (call $call2 (i64.const 1) (i64.const 2))
  )

  (func (export "calltest3") (result i32)
    (call $call3 (i64.const 1) (i64.const 2) (i64.const 3))
  )

  (func (export "calltest4") (result i32)
    (call $call4 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4))
  )

  (func (export "calltest5") (result i32)
    (call $call5 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5))
  )

  (func (export "calltest6") (result i32)
    (call $call6 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5) (i64.const 6))
  )

  (func (export "calltest7") (result i32)
    (call $call7 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5) (i64.const 6) (i64.const 7))
  )

  (func (export "calltest8") (result i32)
    (call $call8 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5) (i64.const 6) (i64.const 7) (i64.const 8))
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
