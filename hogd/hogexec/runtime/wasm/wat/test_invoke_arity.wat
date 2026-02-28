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


  (func (export "calltest0")
    (call $call0)
    i32.const 100
    i32.eq
    if
    else
      unreachable
    end
  )

  (func (export "calltest1")
    (call $call1 (i64.const 1))
    i32.const 101
    i32.eq
    if
    else
      unreachable
    end
  )

  (func (export "calltest2")
    (call $call2 (i64.const 1) (i64.const 2))
    i32.const 102
    i32.eq
    if
    else
      unreachable
    end
  )

  (func (export "calltest3")
    (call $call3 (i64.const 1) (i64.const 2) (i64.const 3))
    i32.const 103
    i32.eq
    if
    else
      unreachable
    end
  )

  (func (export "calltest4")
    (call $call4 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4))
    i32.const 104
    i32.eq
    if
    else
      unreachable
    end
  )

  (func (export "calltest5")
    (call $call5 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5))
    i32.const 105
    i32.eq
    if
    else
      unreachable
    end
  )

  (func (export "calltest6")
    (call $call6 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5) (i64.const 6))
    i32.const 106
    i32.eq
    if
    else
      unreachable
    end
  )

  (func (export "calltest7")
    (call $call7 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5) (i64.const 6) (i64.const 7))
    i32.const 107
    i32.eq
    if
    else
      unreachable
    end
  ) 

  (func (export "calltest8")
    (call $call8 (i64.const 1) (i64.const 2) (i64.const 3) (i64.const 4) (i64.const 5) (i64.const 6) (i64.const 7) (i64.const 8))
    i32.const 108
    i32.eq
    if
    else
      unreachable
    end
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
