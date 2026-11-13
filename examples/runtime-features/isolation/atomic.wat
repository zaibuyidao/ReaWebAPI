(module
  (import "env" "memory" (memory 1 1 shared))
  (func (export "add") (param i32) (result i32)
    i32.const 0
    local.get 0
    i32.atomic.rmw.add))
