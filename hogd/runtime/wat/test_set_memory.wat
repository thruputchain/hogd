(module
  (memory 1 1)

  (func (export "size") (result i64) (i64.extend_i32_u (memory.size)))

  (func (export "store") (result i64)
    (i64.store8 (i32.const 12) (i64.const 0xFFAABBCC))
    (i64.store (i32.const 44) (i64.const 0xFFAABBCC))
    (i64.const 0)
  )

  (func (export "load") (result i64)
    (i64.load (i32.const 32))
  )

  (export "memory" (memory 0))
)