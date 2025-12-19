(module
  ;; Compute Fibonacci number recursively (slow way)
  ;; fib(0) = 0
  ;; fib(1) = 1
  ;; fib(n) = fib(n-1) + fib(n-2)
  (func (export "fib") (param $n i32) (result i32)
    (if (result i32)
      (i32.le_u (local.get $n) (i32.const 1))
      (then
        ;; Base case: fib(0) = 0, fib(1) = 1
        (local.get $n)
      )
      (else
        ;; Recursive case: fib(n-1) + fib(n-2)
        (i32.add
          (call 0
            (i32.sub (local.get $n) (i32.const 1))
          )
          (call 0
            (i32.sub (local.get $n) (i32.const 2))
          )
        )
      )
    )
  )
  ;; fib10 asserts fib(10) == 55 by storing the result at memory[0].
  (func (export "fib10")
    (local $result i32)
    (local.set $result
      (call 0 (i32.const 10))
    )
    ;; Assert that fib(10) == 55, trap if not
    (if
      (i32.ne (local.get $result) (i32.const 55))
      (then
        unreachable
      )
    )
    ;; Optionally store the result at memory[0] for inspection
    (i32.store (i32.const 0) (local.get $result))
  )
  (func (export "fib20")
    (local $result i32)
    (local.set $result
      (call 0 (i32.const 20))
    )
    ;; Assert that fib(20) == 6765, trap if not
    (if
      (i32.ne (local.get $result) (i32.const 6765))
      (then
        unreachable
      )
    )
    ;; Optionally store the result at memory[0] for inspection
    (i32.store (i32.const 0) (local.get $result))
  )

  (func (export "fib30")
    (local $result i32)
    (local.set $result
      (call 0 (i32.const 30))
    )
    ;; Assert that fib(30) == 832040, trap if not
    (if
      (i32.ne (local.get $result) (i32.const 832040))
      (then
        unreachable
      )
    )
    ;; Optionally store the result at memory[0] for inspection
    (i32.store (i32.const 0) (local.get $result))
  )

  (func (export "fib100")
    (call 0 (i32.const 100))
    (drop)
  )
  

  (memory 1 1)
  (export "memory" (memory 0))
)
