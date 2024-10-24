(module
  (memory 1 1)

  (func (export "size") (result i32) (memory.size))

  (func (export "store") (result i32)
    (i64.store8 (i32.const 12) (i64.const 0xFFAABBCC))
    (i64.store (i32.const 44) (i64.const 0xFFAABBCC))
    (i32.const 0)
  )

  (func (export "load") (result i32)
    (i32.load (i32.const 32))
  )

  (export "memory" (memory 0))
)