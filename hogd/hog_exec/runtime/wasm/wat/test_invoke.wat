(module
  (type (;0;) (func (param i32) (result i32)))
  (import "test" "redir_call" (func $foo (type 0)))

  (func (export "0000000000000000")
    i32.const 12
    call $foo
    drop
  )
  (memory 1 1)
  (export "memory" (memory 0))
)
