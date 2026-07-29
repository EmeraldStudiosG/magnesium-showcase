use magnesium_sys::*;
use std::ffi::CString;
use std::fs;

const QNAN: u64 = 0x7ffc000000000000;
const SIGN_BIT: u64 = 0x8000000000000000;
const TAG_NULL: u64 = 1;
const TAG_INT: u64 = 4;

union ValuePun {
    as_bits: u64,
    as_num: f64,
}

#[inline(always)]
const fn null_val() -> Value {
    QNAN | TAG_NULL
}

#[inline(always)]
const fn int_val(n: i32) -> Value {
    QNAN | TAG_INT | ((n as u32 as u64) << 3)
}

#[inline(always)]
fn number_val(n: f64) -> Value {
    n.to_bits()
}

#[inline(always)]
fn is_int_value(v: Value) -> bool {
    (v & (SIGN_BIT | QNAN | 0x7)) == (QNAN | TAG_INT)
}

#[inline(always)]
fn as_int_value(v: Value) -> i32 {
    ((v >> 3) & 0xFFFF_FFFF) as i32
}

unsafe extern "C" fn rust_add(_vm: *mut VM, arg_count: libc::c_int, args: *mut Value) -> Value {
    if arg_count < 2 {
        return null_val();
    }
    let a = *args.offset(0);
    let b = *args.offset(1);
    if is_int_value(a) && is_int_value(b) {
        return int_val(as_int_value(a) + as_int_value(b));
    }
    let da = if is_int_value(a) {
        as_int_value(a) as f64
    } else {
        ValuePun { as_bits: a }.as_num
    };
    let db = if is_int_value(b) {
        as_int_value(b) as f64
    } else {
        ValuePun { as_bits: b }.as_num
    };
    number_val(da + db)
}

fn main() {
    unsafe {
        let vm = vm_new();
        assert!(!vm.is_null(), "vm_new returned null");

        let c_name = CString::new("rust_add").unwrap();
        vm_register_native(
            vm,
            c_name.as_ptr(),
            Some(rust_add),
            2,
            std::ptr::null_mut(),
            None,
        );

        let mg_path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../../examples/syntax_test.mg"
        );
        let source = fs::read_to_string(mg_path).expect("failed to read syntax_test.mg");
        let c_source = CString::new(source).unwrap();
        let c_script = CString::new("examples/syntax_test.mg").unwrap();

        let result = vm_interpret_named(vm, c_source.as_ptr(), c_script.as_ptr());

        match result {
            InterpretResult::Ok => println!("\n[Rust/sys] VM finished OK"),
            InterpretResult::CompileError => eprintln!("\n[Rust/sys] Compile error"),
            InterpretResult::RuntimeError => eprintln!("\n[Rust/sys] Runtime error"),
            InterpretResult::Yield => eprintln!("\n[Rust/sys] Yielded"),
        }

        vm_delete(vm);
    }
}
