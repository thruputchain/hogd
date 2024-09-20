(module
  (type (;0;) (func (result i64)))
  (import "test" "external_call" (func (;0;) (type 0)))
  (import "test" "good_call" (func (;0;) (type 0)))

  (func (export "call1") (result i64)
    call 0
    drop
    i64.const 0)

  (func (export "call2") (result i64)
    call 1
    drop
    i64.const 1)

  (func (export "spin") (result i64)
    (loop $spinloop
      i64.const 1
      i64.const 2
      call $add
      drop
      br $spinloop
    )
    unreachable
  )

  (func $add (param i64 i64) (result i64)
    local.get 0
    local.get 1
    i64.add
  )

  (func (export "unreachable") (result i64)
    unreachable)
)