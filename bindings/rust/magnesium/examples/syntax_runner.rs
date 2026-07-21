use magnesium::{InterpretResult, Value, Vm};
use std::fs;

unsafe extern "C" fn rust_add(
    _vm: *mut magnesium_sys::VM,
    arg_count: libc::c_int,
    args: *mut magnesium_sys::Value,
) -> magnesium_sys::Value {
    if arg_count < 2 {
        return Value::null().to_raw();
    }
    let a = Value::from_raw(*args.offset(0)).as_number();
    let b = Value::from_raw(*args.offset(1)).as_number();
    Value::number_val(a + b).to_raw()
}

fn main() {
    let mut vm = Vm::new();

    vm.register_native("rust_add", Some(rust_add), 2);

    let mg_path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../examples/syntax_test.mg");
    let source = fs::read_to_string(mg_path).expect("failed to read syntax_test.mg");

    let result = vm.interpret_named(&source, "examples/syntax_test.mg");

    match result {
        InterpretResult::Ok => println!("\n[Rust] VM finished OK"),
        InterpretResult::Err(e) => eprintln!("\n[Rust] VM error: {:?}", e),
    }
}
