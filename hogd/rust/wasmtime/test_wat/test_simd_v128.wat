(module
  (func (export "simd_op")
    v128.const i32x4 1 2 3 4
    v128.const i32x4 5 6 7 8
    i8x16.add
    drop
  )
  (memory 1 1)
  (export "memory" (memory 0))
)
