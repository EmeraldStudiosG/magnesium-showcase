use magnesium::{HostError, InterpretError, NativeContext, Value, Vm};
use magnesium_sys as sys;
use std::sync::atomic::{AtomicBool, Ordering};

#[test]
fn test_vm_create_and_drop() {
    let vm = Vm::new();
    drop(vm);
}

#[test]
fn test_interpret_hello() {
    let mut vm = Vm::new();
    let result = vm.interpret("print(\"hello from rust\")");
    assert!(result.is_ok());
}

#[test]
fn test_value_null() {
    let v = Value::null();
    assert!(v.is_null());
    assert!(!v.is_bool());
    assert!(!v.is_number());
    assert!(!v.is_int());
}

#[test]
fn test_value_bool() {
    let t = Value::bool_val(true);
    let f = Value::bool_val(false);
    assert!(t.is_bool());
    assert!(t.as_bool());
    assert!(f.is_bool());
    assert!(!f.as_bool());
}

#[test]
fn test_value_int() {
    let v = Value::int_val(42);
    assert!(v.is_int());
    assert!(!v.is_number());
    assert!(v.is_numeric());
    assert_eq!(v.as_int(), 42);
    assert_eq!(v.as_number(), 42.0);
}

#[test]
fn test_value_float() {
    let v = Value::number_val(4.56);
    assert!(v.is_number());
    assert!(v.is_numeric());
    assert!(!v.is_int());
    assert!((v.as_number() - 4.56).abs() < f64::EPSILON);
}

#[test]
fn test_value_auto_val() {
    let v = Value::auto_val(5.0);
    assert!(v.is_int());
    assert_eq!(v.as_int(), 5);

    let v2 = Value::auto_val(5.5);
    assert!(!v2.is_int());
    assert!(v2.is_number());
}

#[test]
fn test_value_falsey() {
    assert!(Value::null().is_falsey());
    assert!(Value::bool_val(false).is_falsey());
    assert!(!Value::bool_val(true).is_falsey());
    assert!(!Value::int_val(0).is_falsey());
}

#[test]
fn test_value_from_conversions() {
    let v: Value = true.into();
    assert!(v.is_bool() && v.as_bool());

    let v: Value = 42i32.into();
    assert!(v.is_int() && v.as_int() == 42);

    let v: Value = 4.56f64.into();
    assert!(v.is_number());

    let v: Value = ().into();
    assert!(v.is_null());
}

#[test]
fn test_value_try_into_conversions() {
    let v = Value::bool_val(true);
    let b: bool = v.try_into().unwrap();
    assert!(b);

    let v = Value::int_val(99);
    let n: i32 = v.try_into().unwrap();
    assert_eq!(n, 99);

    let v = Value::number_val(1.618);
    let f: f64 = v.try_into().unwrap();
    assert!((f - 1.618).abs() < f64::EPSILON);
}

#[test]
fn test_try_as_typed_methods() {
    assert_eq!(Value::bool_val(true).try_as_bool(), Some(true));
    assert_eq!(Value::bool_val(false).try_as_bool(), Some(false));
    assert_eq!(Value::null().try_as_bool(), None);

    assert_eq!(Value::int_val(42).try_as_int(), Some(42));
    assert_eq!(Value::number_val(3.14).try_as_int(), None);
    assert_eq!(Value::null().try_as_int(), None);

    assert!((Value::number_val(2.718).try_as_number().unwrap() - 2.718).abs() < f64::EPSILON);
    assert_eq!(Value::int_val(5).try_as_number(), None);

    assert_eq!(Value::int_val(7).try_as_numeric(), Some(7.0));
    assert!((Value::number_val(1.5).try_as_numeric().unwrap() - 1.5).abs() < f64::EPSILON);
    assert_eq!(Value::null().try_as_numeric(), None);

    assert_eq!(Value::int_val(42).expect_int(), 42);
}

#[test]
fn test_expect_methods() {
    assert_eq!(Value::bool_val(true).expect_bool(), true);
    assert_eq!(Value::int_val(99).expect_int(), 99);
    assert!((Value::number_val(3.14).expect_number() - 3.14).abs() < f64::EPSILON);
    assert_eq!(Value::int_val(5).expect_numeric(), 5.0);
    assert!((Value::number_val(2.5).expect_numeric() - 2.5).abs() < f64::EPSILON);
}

#[test]
fn test_push_pop() {
    let mut vm = Vm::new();
    vm.push(Value::int_val(100));
    let v = vm.pop();
    assert_eq!(v.as_int(), 100);
}

#[test]
fn test_set_get_global() {
    let mut vm = Vm::new();
    vm.set_global("x", Value::int_val(42));
    let val = vm.get_global("x");
    assert!(val.is_some());
    assert_eq!(val.unwrap().as_int(), 42);
}

#[test]
fn test_get_global_missing() {
    let mut vm = Vm::new();
    assert!(vm.get_global("nonexistent").is_none());
}

#[test]
fn test_compile() {
    let mut vm = Vm::new();
    let func = vm.compile("let x = 1 + 2");
    assert!(func.is_some());
}

#[test]
fn test_compile_handle_run() {
    let mut vm = Vm::new();
    let func = vm.compile_handle("let x = 1 + 2").expect("compile handle");
    assert!(vm.run_compiled(func).is_ok());
}

#[test]
fn test_optional_type_check_api() {
    let mut vm = Vm::new();
    assert!(vm.type_check("let score: number = \"dynamic\"", false));
    assert!(!vm.type_check("!strict\nlet score: number = \"bad\"", false));
    assert!(vm.type_check("!nocheck\nlet score: number = \"bad\"", true));
}

#[test]
fn test_extern_declaration_with_host_global() {
    let mut vm = Vm::new();
    vm.set_global_number("HOST_SCORE", 42.0);
    let result = vm.interpret(
        "!strict\nextern const HOST_SCORE: number\nlet score: number = HOST_SCORE\nprint(score)",
    );
    assert!(result.is_ok());
}

#[test]
fn test_interpret_arithmetic() {
    let mut vm = Vm::new();
    let result = vm.interpret("let x = 2 + 3");
    assert!(result.is_ok());
}

#[test]
fn test_interpret_compile_error() {
    let mut vm = Vm::new();
    let result = vm.interpret("let x = ");
    assert!(result.is_err());
}

#[test]
fn test_interpret_rejects_interior_nul_without_panic() {
    let mut vm = Vm::new();
    let result = vm.interpret("print(\"ok\")\0");
    match result {
        magnesium::InterpretResult::Err(InterpretError::HostError(HostError::InteriorNul {
            field,
        })) => {
            assert_eq!(field, "source");
        }
        _ => panic!("expected host interior-nul error"),
    }
}

#[test]
fn test_interpret_named() {
    let mut vm = Vm::new();
    let result = vm.interpret_named("print(\"test\")", "test_script");
    assert!(result.is_ok());
}

#[test]
fn test_bits_roundtrip() {
    let v = Value::int_val(-7);
    let bits = v.to_bits();
    let v2 = Value::from_bits(bits);
    assert_eq!(v2.as_int(), -7);
}

#[test]
fn test_safe_numeric_ffi_registration() {
    extern "C" fn square(x: f64) -> f64 {
        x * x
    }

    extern "C" fn add(a: f64, b: f64) -> f64 {
        a + b
    }

    let mut vm = Vm::new();
    vm.register_ffi1("square", square).unwrap();
    vm.register_ffi2("add", add).unwrap();

    let square_value = vm
        .get_global("square")
        .expect("square should be registered");
    assert!(square_value.is_ffi());
    assert!(square_value.is_callable());

    let result = vm.interpret("let x = square(4)\nlet y = add(x, 2)");
    assert!(result.is_ok());
}

#[test]
fn test_runtime_errors_preserve_message_for_rust() {
    let mut vm = Vm::new();
    let result = vm.interpret("let x = \"a\" + 1");
    match result {
        magnesium::InterpretResult::Err(InterpretError::RuntimeError(err)) => {
            assert_eq!(err.kind(), "RuntimeError");
            assert!(err.message().contains("Operator '+'"));
            assert!(vm.last_error_message().contains("Operator '+'"));
            assert!(vm.last_error_line() > 0);
        }
        _ => panic!("expected runtime error"),
    }
}

static FINALIZER_CALLED: AtomicBool = AtomicBool::new(false);

struct Counter {
    value: i32,
}

impl Drop for Counter {
    fn drop(&mut self) {
        FINALIZER_CALLED.store(true, Ordering::SeqCst);
    }
}

unsafe extern "C" fn counter_get(
    _vm: *mut sys::VM,
    arg_count: libc::c_int,
    args: *mut sys::Value,
) -> sys::Value {
    if arg_count != 1 || args.is_null() {
        return Value::null().to_raw();
    }

    let type_name = b"Counter\0";
    let data = sys::vm_native_handle_data(*args, type_name.as_ptr().cast::<libc::c_char>());
    if data.is_null() {
        return Value::null().to_raw();
    }

    let counter = &*(data.cast::<Counter>());
    Value::int_val(counter.value).to_raw()
}

#[test]
fn test_native_handle_method_and_finalizer() {
    FINALIZER_CALLED.store(false, Ordering::SeqCst);

    {
        let mut vm = Vm::new();
        let handle = vm
            .try_new_native_handle("Counter", Counter { value: 41 })
            .expect("native handle");
        assert!(handle.is_native_handle());

        vm.try_set_native_handle_method(handle, "get", Some(counter_get), 1)
            .expect("native handle method");
        vm.set_global("counter", handle);

        let result = vm.interpret("let value = counter.get()\nprint(value)");
        assert!(result.is_ok());
    }

    assert!(FINALIZER_CALLED.load(Ordering::SeqCst));
}

#[test]
fn test_native_handle_closure_method() {
    FINALIZER_CALLED.store(false, Ordering::SeqCst);

    {
        let mut vm = Vm::new();

        let handle = vm
            .try_new_native_handle("Counter", Counter { value: 0 })
            .expect("native handle");

        vm.set_native_handle_method_fn(handle, "Counter", "increment", 1,
            |counter: &mut Counter, _ctx: NativeContext| {
                counter.value += 1;
                Value::int_val(counter.value)
            },
        );

        vm.set_native_handle_method_fn(handle, "Counter", "get", 1,
            |counter: &mut Counter, _ctx: NativeContext| {
                Value::int_val(counter.value)
            },
        );

        vm.set_global("counter", handle);

        let result = vm.interpret(
            "counter.increment()\ncounter.increment()\nlet v = counter.get()\nprint(v)"
        );
        assert!(result.is_ok());
    }

    assert!(FINALIZER_CALLED.load(Ordering::SeqCst));
}

#[test]
fn test_native_handle_data_ref_mut() {
    let mut vm = Vm::new();
    let handle = vm
        .try_new_native_handle("Counter", Counter { value: 10 })
        .expect("native handle");
    vm.set_global("ctr", handle);

    let val = vm.get_global("ctr").unwrap();
    let data_ref = vm.native_handle_data_ref::<Counter>(val, "Counter").unwrap();
    assert!(data_ref.is_some());
    assert_eq!(data_ref.unwrap().value, 10);

    let data_mut = vm.native_handle_data_mut::<Counter>(val, "Counter").unwrap();
    assert!(data_mut.is_some());
    data_mut.unwrap().value = 99;

    let data_ref2 = vm.native_handle_data_ref::<Counter>(val, "Counter").unwrap();
    assert_eq!(data_ref2.unwrap().value, 99);
}

#[test]
fn test_register_native_fn() {
    let mut vm = Vm::new();

    vm.register_native_fn("double", 1, |ctx: NativeContext| {
        let n = ctx.expect_numeric(0);
        Value::auto_val(n * 2.0)
    }).unwrap();

    let result = vm.interpret("let x = double(21)\nprint(x)");
    assert!(result.is_ok());
}

#[test]
fn test_register_native_fn_multi_args() {
    let mut vm = Vm::new();

    vm.register_native_fn("add_nums", 2, |ctx: NativeContext| {
        let a = ctx.expect_numeric(0);
        let b = ctx.expect_numeric(1);
        Value::auto_val(a + b)
    }).unwrap();

    let result = vm.interpret("let x = add_nums(3, 4)\nprint(x)");
    assert!(result.is_ok());
}

#[test]
fn test_native_context_string_arg() {
    let mut vm = Vm::new();

    vm.register_native_fn("is_hello", 1, |ctx: NativeContext| {
        let name = ctx.arg_string(0);
        match name {
            Some("hello") => Value::bool_val(true),
            _ => Value::bool_val(false),
        }
    }).unwrap();

    let result = vm.interpret("let ok = is_hello(\"hello\")\nprint(ok)");
    assert!(result.is_ok());
}
