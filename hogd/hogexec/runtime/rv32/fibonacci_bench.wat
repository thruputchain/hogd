(module
  (memory (export "memory") 1)
  (global $__groundhog_stack_limit
    (export "__groundhog_stack_limit")
    (mut i64)
    (i64.const 4096))

  (func $fib (param $n i32) (result i32)
    local.get $n
    i32.const 2
    i32.lt_u
    if (result i32)
      local.get $n
    else
      local.get $n
      i32.const 1
      i32.sub
      call $fib
      local.get $n
      i32.const 2
      i32.sub
      call $fib
      i32.add
    end)

  (func (export "14")
    i32.const 20
    call $fib
    drop))
