(module
  (import "marmot" "syscall" (global i64))

  (func (export "invoke"))

  (memory 1 1)
  (export "memory" (memory 0))
)
