(module
  (import "test" "arg0" (func $call0 (result i64)))
  (import "test" "arg1" (func $call1 (param i64) (result i64)))
  (import "test" "arg2" (func $call2 (param i64 i64) (result i64)))
  (import "test" "arg3" (func $call3 (param i64 i64 i64) (result i64)))
  (import "test" "arg4" (func $call4 (param i64 i64 i64 i64) (result i64)))
  (import "test" "arg5" (func $call5 (param i64 i64 i64 i64 i64) (result i64)))


  (func (export "calltest0") (result i64)
    (call $call0)
  )

  (func (export "calltest1") (result i64)
    (call $call1 (i64.const 1))
  )

  (func (export "calltest2") (result i64)
    (call $call2 (i64.const 1) (i64.const 2))
  )

  (func (export "calltest3") (result i64)
    (call $call3 (i64.const 1) (i64.const 2) (i64.const 3))
  )

  (func (export "calltest4") (result i64)
    (call $call4 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4))
  )

  (func (export "calltest5") (result i64)
    (call $call5 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5))
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
