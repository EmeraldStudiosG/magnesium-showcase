use magnesium::{InterpretResult, Vm};
use std::fs;

fn main() {
    let mut vm = Vm::new();

    let mg_path = concat!(env!("CARGO_MANIFEST_DIR"), "/../../examples/hello.mg");
    let source = fs::read_to_string(mg_path).expect("failed to read hello.mg");

    let result = vm.interpret_named(&source, "examples/hello.mg");

    match result {
        InterpretResult::Ok => {}
        InterpretResult::Err(e) => eprintln!("error: {:?}", e),
    }
}
