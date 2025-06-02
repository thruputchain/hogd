(module
  (type (;0;) (func (result i32)))
  (import "test" "external_call" (func (;0;) (type 0)))
  (import "test" "good_call" (func (;0;) (type 0)))

  (func (export "call1")
    call 0
    drop)

  (func (export "call2")
    call 1
    drop)

  (func (export "call_then_unreachable")
    call 0
    unreachable)

  (func (export "spin")
    (loop $spinloop
      i64.const 1
      i64.const 2
      call $add
      drop
      br $spinloop
    )
    unreachable
  )

  (func $add (param i64 i64) (result i32)
    local.get 0
    local.get 1
    i64.add
    i32.wrap_i64
  )

  (func (export "unreachable")
    unreachable)
)