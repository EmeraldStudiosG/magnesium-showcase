use magnesium::{NativeContext, Value, Vm};
use std::fs;

fn main() {
    let mut vm = Vm::new();

    vm.register_native_fn("rust_add", 2, |ctx: NativeContext| {
        let a = ctx.expect_numeric(0);
        let b = ctx.expect_numeric(1);
        Value::auto_val(a + b)
    }).unwrap();

    vm.register_native_fn("rust_greet", 1, |ctx: NativeContext| {
        let name = ctx.arg_string(0).unwrap_or("world");
        println!("Hello, {}! You called Rust from Magnesium.", name);
        Value::null()
    }).unwrap();

    let mg_path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../examples/rust_call.mg");
    let source = fs::read_to_string(mg_path).expect("failed to read rust_call.mg");

    let result = vm.interpret_named(&source, "examples/rust_call.mg");

    match result {
        magnesium::InterpretResult::Ok => {}
        magnesium::InterpretResult::Err(e) => eprintln!("error: {:?}", e),
    }
}
