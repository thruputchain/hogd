(module
  (type (;0;) (func (param i64) (result i64)))
  (import "test" "redir_call" (func $foo (type 0)))

  (func (export "calltest") (result i64)
    i64.const 12
    call $foo
  )
  (memory 1 1)
  (export "memory" (memory 0))
)
