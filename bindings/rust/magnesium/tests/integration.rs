use magnesium::{HostError, InterpretError, NativeContext, Value, Vm};
use magnesium_sys as sys;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

struct DropSignal(Arc<AtomicBool>);

impl Drop for DropSignal {
    fn drop(&mut self) {
        self.0.store(true, Ordering::SeqCst);
    }
}

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
    assert_eq!(Value::number_val(3.125).try_as_int(), None);
    assert_eq!(Value::null().try_as_int(), None);

    assert!((Value::number_val(2.75).try_as_number().unwrap() - 2.75).abs() < f64::EPSILON);
    assert_eq!(Value::int_val(5).try_as_number(), None);

    assert_eq!(Value::int_val(7).try_as_numeric(), Some(7.0));
    assert!((Value::number_val(1.5).try_as_numeric().unwrap() - 1.5).abs() < f64::EPSILON);
    assert_eq!(Value::null().try_as_numeric(), None);

    assert_eq!(Value::int_val(42).expect_int(), 42);
}

#[test]
fn test_expect_methods() {
    assert!(Value::bool_val(true).expect_bool());
    assert_eq!(Value::int_val(99).expect_int(), 99);
    assert!((Value::number_val(3.125).expect_number() - 3.125).abs() < f64::EPSILON);
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
    assert!(vm.run_compiled(&func).is_ok());
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
    let v2 = unsafe { Value::from_bits(bits) };
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

        unsafe {
            vm.try_set_native_handle_method(&handle, "get", Some(counter_get), 1)
                .expect("native handle method");
        }
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

        unsafe {
            vm.set_native_handle_method_fn(
                &handle,
                "Counter",
                "increment",
                1,
                |counter: &mut Counter, _ctx: NativeContext| {
                    counter.value += 1;
                    Value::int_val(counter.value)
                },
            );

            vm.set_native_handle_method_fn(
                &handle,
                "Counter",
                "get",
                1,
                |counter: &mut Counter, _ctx: NativeContext| Value::int_val(counter.value),
            );
        }

        vm.set_global("counter", handle);

        let result = vm
            .interpret("counter.increment()\ncounter.increment()\nlet v = counter.get()\nprint(v)");
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
    let data_ref = unsafe {
        vm.native_handle_data_ref::<Counter>(&val, "Counter")
            .unwrap()
    };
    assert!(data_ref.is_some());
    assert_eq!(data_ref.unwrap().value, 10);

    let data_mut = unsafe {
        vm.native_handle_data_mut::<Counter>(&val, "Counter")
            .unwrap()
    };
    assert!(data_mut.is_some());
    data_mut.unwrap().value = 99;

    let data_ref2 = unsafe {
        vm.native_handle_data_ref::<Counter>(&val, "Counter")
            .unwrap()
    };
    assert_eq!(data_ref2.unwrap().value, 99);
}

#[test]
fn test_register_native_fn() {
    let mut vm = Vm::new();

    vm.register_native_fn("double", 1, |ctx: NativeContext| {
        let n = ctx.expect_numeric(0);
        Value::auto_val(n * 2.0)
    })
    .unwrap();

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
    })
    .unwrap();

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
    })
    .unwrap();

    let result = vm.interpret("let ok = is_hello(\"hello\")\nprint(ok)");
    assert!(result.is_ok());
}

#[test]
fn test_number_value_sanitizes_nan_box_payloads() {
    let tag_shaped_nan = f64::from_bits(sys::QNAN | sys::TAG_NULL);
    let value = Value::number_val(tag_shaped_nan);
    assert!(value.is_number());
    assert!(value.as_number().is_nan());
    assert!(!value.is_null());
    assert!(!value.is_obj());
}

#[test]
fn test_function_handle_rejects_a_different_vm() {
    let mut first = Vm::new();
    let function = first.compile_handle("return 1").expect("compile handle");
    let mut second = Vm::new();
    assert!(matches!(
        second.try_run_compiled(&function),
        Err(HostError::InvalidFunctionHandle)
    ));
}

#[test]
fn test_object_value_root_survives_global_replacement_and_collection() {
    let mut vm = Vm::new();
    vm.set_global_string("held", "rooted value");
    let held = vm.get_global("held").expect("held string");
    vm.set_global("held", Value::null());
    magnesium::gc::gc_collect(&mut vm);
    assert_eq!(held.to_owned_string().as_deref(), Some("rooted value"));
}

#[test]
fn test_object_value_keeps_its_vm_alive() {
    let held = {
        let mut vm = Vm::new();
        vm.set_global_string("held", "still alive");
        vm.get_global("held").expect("held string")
    };
    assert_eq!(held.to_owned_string().as_deref(), Some("still alive"));
}

#[test]
fn test_gc_root_does_not_lock_the_vm_borrow() {
    let mut vm = Vm::new();
    vm.set_global_string("held", "usable root");
    let value = vm.get_global("held").expect("held string");
    let root = magnesium::GcRoot::new(&vm, value);
    vm.set_global("held", Value::null());
    magnesium::gc::gc_collect(&mut vm);
    assert_eq!(root.get().to_owned_string().as_deref(), Some("usable root"));
}

#[test]
fn test_global_string_preserves_embedded_null_bytes() {
    let mut vm = Vm::new();
    vm.try_set_global_string("binary", "a\0b")
        .expect("set string containing a null byte");
    let value = vm.get_global("binary").expect("binary string");
    assert_eq!(value.to_owned_string().as_deref(), Some("a\0b"));
}

#[test]
fn test_foreign_object_value_is_rejected() {
    let mut first = Vm::new();
    let value = first.new_array_value().expect("new array");
    let mut second = Vm::new();
    assert!(matches!(
        second.try_set_global("foreign", value),
        Err(HostError::ForeignValue)
    ));
}

#[test]
fn test_native_callback_panic_does_not_cross_the_c_boundary() {
    let mut vm = Vm::new();
    vm.register_native_fn("panic_in_rust", 0, |_ctx| {
        panic!("intentional callback panic")
    })
    .unwrap();

    let call = catch_unwind(AssertUnwindSafe(|| vm.interpret("panic_in_rust()")));
    assert!(call.is_ok(), "Rust panic escaped through the C VM");
    assert!(vm.last_error_message().contains("panicked"));
}

#[test]
fn test_native_callback_capture_does_not_retain_its_own_vm() {
    let closure_dropped = Arc::new(AtomicBool::new(false));
    {
        let mut vm = Vm::new();
        vm.set_global_string("captured", "same VM");
        let captured = vm.get_global("captured").expect("captured value");
        let signal = DropSignal(Arc::clone(&closure_dropped));

        vm.register_native_fn("captures_vm_value", 0, move |_ctx| {
            let _ = (&captured, &signal);
            Value::null()
        })
        .expect("register native closure");
    }

    assert!(
        closure_dropped.load(Ordering::SeqCst),
        "dropping Vm must release callbacks that capture rooted values from that VM"
    );
}

#[test]
fn test_native_method_capture_does_not_retain_its_own_vm() {
    let closure_dropped = Arc::new(AtomicBool::new(false));
    {
        let mut vm = Vm::new();
        let captured = vm.new_array_value().expect("captured array");
        let handle = vm
            .try_new_native_handle("Counter", Counter { value: 0 })
            .expect("native handle");
        let signal = DropSignal(Arc::clone(&closure_dropped));

        unsafe {
            vm.try_set_native_handle_method_fn(
                &handle,
                "Counter",
                "captures_vm_value",
                1,
                move |_counter: &mut Counter, _ctx| {
                    let _ = (&captured, &signal);
                    Value::null()
                },
            )
            .expect("register native method closure");
        }
        vm.set_global("cycle_handle", handle);
    }

    assert!(
        closure_dropped.load(Ordering::SeqCst),
        "dropping Vm must release method callbacks that capture rooted same-VM values"
    );
}

#[test]
fn test_native_handle_payload_does_not_retain_its_own_vm() {
    struct Payload {
        _captured: Value,
        _signal: DropSignal,
    }

    let payload_dropped = Arc::new(AtomicBool::new(false));
    {
        let mut vm = Vm::new();
        let captured = vm.new_dict_value().expect("captured dict");
        let payload = Payload {
            _captured: captured,
            _signal: DropSignal(Arc::clone(&payload_dropped)),
        };
        let handle = vm
            .try_new_native_handle("Payload", payload)
            .expect("native handle");
        vm.set_global("cycle_payload", handle);
    }

    assert!(
        payload_dropped.load(Ordering::SeqCst),
        "dropping Vm must release native-handle payloads containing rooted same-VM values"
    );
}

#[test]
fn test_native_registration_rejects_invalid_metadata() {
    let mut vm = Vm::new();
    assert!(matches!(
        vm.register_native_fn("too_many", 256, |_ctx| Value::null()),
        Err(HostError::InvalidArity { arity: 256 })
    ));
    assert!(matches!(
        unsafe { vm.try_register_native("missing", None, 0) },
        Err(HostError::MissingCallback)
    ));
}

#[test]
fn test_scanner_token_keeps_source_alive() {
    let token = {
        let source = String::from("retained_name");
        let mut scanner = magnesium::Scanner::new(&source).expect("scanner source");
        scanner.scan_token()
    };
    assert_eq!(token.text().unwrap(), "retained_name");
}

#[test]
fn test_ast_keeps_source_tokens_alive() {
    let ast = {
        let source = String::from("let retained_name = 1");
        magnesium::ast::parse(&source).expect("parse")
    };
    unsafe {
        let block = &(*ast.raw()).as_.block;
        let declaration = *block.stmts.nodes;
        let token = (*declaration).as_.var_decl.name;
        let bytes = std::slice::from_raw_parts(token.start.cast::<u8>(), token.length as usize);
        assert_eq!(std::str::from_utf8(bytes).unwrap(), "retained_name");
    }
}

#[test]
fn test_vm_aware_collection_wrappers() {
    let mut vm = Vm::new();

    let mut array = magnesium::array::new_array(&mut vm);
    assert!(magnesium::MgArray::push(
        &mut vm,
        &mut array,
        &Value::int_val(10)
    ));
    assert!(array.set(0, &Value::int_val(20)));
    assert_eq!(array.get(0).and_then(|value| value.try_as_int()), Some(20));

    let key = magnesium::string::copy_string(&mut vm, "key");
    let mut dict = magnesium::dict::new_dict(&mut vm);
    assert!(magnesium::MgDict::set(
        &mut vm,
        &mut dict,
        &key,
        &Value::int_val(30)
    ));
    assert_eq!(
        dict.get(&key).and_then(|value| value.try_as_int()),
        Some(30)
    );
}

#[test]
fn test_rope_string_wrapper_resolves_and_copies_utf8() {
    let mut vm = Vm::new();
    let left = magnesium::string::copy_string(&mut vm, "hello ");
    let right = magnesium::string::copy_string(&mut vm, "world");
    let rope = magnesium::string::concat_strings(&mut vm, &left, &right).expect("concat");
    assert_eq!(rope.resolve(), Some("hello world"));
    assert_eq!(rope.to_owned_string().as_deref(), Some("hello world"));
}
