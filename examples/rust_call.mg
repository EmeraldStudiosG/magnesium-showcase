// Magnesium calling into Rust via registered native functions.
// Run with: cargo run --example rust_call (from bindings/rust/magnesium)

extern fn rust_add(a: number, b: number): number
extern fn rust_greet(name: string): null

let sum = rust_add(10, 32)
print("rust_add(10, 32) = " + tostring(sum))

let result = rust_add(100, 200)
print("rust_add(100, 200) = " + tostring(result))

rust_greet("Magnesium")
