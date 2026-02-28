(module
  (type (;0;) (func (param i64) (result i32)))
  (import "test" "redir_call" (func $foo (type 0)))

  (func (export "calltest")
    i64.const 12
    call $foo
    drop
  )
  (memory 1 1)
  (export "memory" (memory 0))
)
