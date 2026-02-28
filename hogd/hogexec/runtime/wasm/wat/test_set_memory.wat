(module
  (memory 1 1)

  (func (export "size") 
    (memory.size)
    drop)

  (func (export "store")
    (i64.store8 (i32.const 12) (i64.const 0xFFAABBCC))
    (i64.store (i32.const 44) (i64.const 0xFFAABBCC))
  )

  (func (export "load")
    (i32.load (i32.const 32))
    drop
  )
  (func (export "load_expect")
    (i32.load (i32.const 32))
    i32.const 0xABCDEF01
    i32.eq
    if
    else
      unreachable
    end
  )

  (export "memory" (memory 0))
)