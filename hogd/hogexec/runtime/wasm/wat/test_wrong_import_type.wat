(module
  ;; Declares the real "marmot"/"syscall" host import with the wrong arity (1
  ;; param instead of 8). All params are i32 so the contract passes wasm-meter
  ;; instrumentation; it is then rejected by the host's import verification
  ;; (validate_imports) for the arity mismatch.
  (import "marmot" "syscall" (func $sys (param i32) (result i32)))

  (func (export "invoke")
    i32.const 0
    call $sys
    drop
  )

  (memory 1 1)
  (export "memory" (memory 0))
)
