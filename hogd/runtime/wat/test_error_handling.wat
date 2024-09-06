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

  (func (export "unreachable") (result i64)
    unreachable)
)