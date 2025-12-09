(module
  ;; A function we will call at start
  (func $init
    ;; do something trivial
    nop
  )

  ;; Mark $init as the start function
  (start $init)
)
