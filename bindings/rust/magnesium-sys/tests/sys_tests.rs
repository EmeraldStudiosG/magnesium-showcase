use magnesium_sys::*;

const QNAN: u64 = 0x7ffc000000000000;
const SIGN_BIT: u64 = 0x8000000000000000;
const TAG_NULL: u64 = 1;
const TAG_FALSE: u64 = 2;
const TAG_TRUE: u64 = 3;
const TAG_INT: u64 = 4;

#[inline(always)]
const fn null_val() -> Value {
    QNAN | TAG_NULL
}

#[inline(always)]
const fn false_val() -> Value {
    QNAN | TAG_FALSE
}

#[inline(always)]
const fn true_val() -> Value {
    QNAN | TAG_TRUE
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
fn is_null(v: Value) -> bool {
    v == null_val()
}

#[inline(always)]
fn is_bool(v: Value) -> bool {
    (v | 1) == true_val()
}

#[inline(always)]
fn is_int(v: Value) -> bool {
    (v & (SIGN_BIT | QNAN | 0x7)) == (QNAN | TAG_INT)
}

#[inline(always)]
fn is_number(v: Value) -> bool {
    (v & QNAN) != QNAN
}

#[inline(always)]
fn is_obj(v: Value) -> bool {
    (v & (SIGN_BIT | QNAN)) == (SIGN_BIT | QNAN)
}

#[inline(always)]
fn as_bool(v: Value) -> bool {
    v == true_val()
}

#[inline(always)]
fn as_int(v: Value) -> i32 {
    ((v >> 3) & 0xFFFF_FFFF) as i32
}

union ValuePun {
    as_bits: u64,
    as_num: f64,
}

#[inline(always)]
fn as_number(v: Value) -> f64 {
    if is_int(v) {
        as_int(v) as f64
    } else {
        unsafe { ValuePun { as_bits: v }.as_num }
    }
}

#[test]
fn test_vm_init_free() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();
        vm_free(vm);
    }
}

#[test]
fn test_vm_interpret() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let source = std::ffi::CString::new("print(\"hello from sys\")").unwrap();
        let result = vm_interpret(vm, source.as_ptr());
        assert_eq!(result, InterpretResult::Ok);

        vm_free(vm);
    }
}

#[test]
fn test_vm_push_pop() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        vm_push(vm, int_val(42));
        let v = vm_pop(vm);
        assert!(is_int(v));
        assert_eq!(as_int(v), 42);

        vm_free(vm);
    }
}

#[test]
fn test_vm_compile() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let source = std::ffi::CString::new("let x = 1 + 2").unwrap();
        let func = vm_compile(vm, source.as_ptr());
        assert!(!func.is_null());

        vm_free(vm);
    }
}

#[test]
fn test_value_null() {
    let v = null_val();
    assert!(is_null(v));
    assert!(!is_bool(v));
    assert!(!is_number(v));
    assert!(!is_obj(v));
}

#[test]
fn test_value_bool() {
    let t = true_val();
    let f = false_val();
    assert!(is_bool(t));
    assert!(as_bool(t));
    assert!(is_bool(f));
    assert!(!as_bool(f));
}

#[test]
fn test_value_int() {
    let v = int_val(99);
    assert!(is_int(v));
    assert!(!is_number(v));
    assert_eq!(as_int(v), 99);
    assert_eq!(as_number(v), 99.0);
}

#[test]
fn test_value_number() {
    let v = number_val(4.56);
    assert!(is_number(v));
    assert!(!is_int(v));
    assert!((as_number(v) - 4.56).abs() < f64::EPSILON);
}

#[test]
fn test_value_bits_roundtrip() {
    let v = int_val(-7);
    let bits = v;
    let v2: Value = bits;
    assert_eq!(as_int(v2), -7);
}

#[test]
fn test_copy_string() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let s = std::ffi::CString::new("hello").unwrap();
        let str_obj = copy_string(vm, s.as_ptr(), 5);
        assert!(!str_obj.is_null());
        assert_eq!((*str_obj).length, 5);

        vm_free(vm);
    }
}

#[test]
fn test_new_array() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let arr = new_array(vm);
        assert!(!arr.is_null());
        assert_eq!((*arr).count, 0);

        vm_free(vm);
    }
}

#[test]
fn test_array_push_get() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let arr = new_array(vm);
        array_push(vm, arr, int_val(10));
        array_push(vm, arr, int_val(20));
        assert_eq!((*arr).count, 2);

        let v0 = array_get(arr, 0);
        let v1 = array_get(arr, 1);
        assert_eq!(as_int(v0), 10);
        assert_eq!(as_int(v1), 20);

        vm_free(vm);
    }
}

#[test]
fn test_new_dict() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let dict = new_dict(vm);
        assert!(!dict.is_null());
        assert_eq!((*dict).count, 0);

        vm_free(vm);
    }
}

#[test]
fn test_dict_set_get() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let dict = new_dict(vm);
        let key = copy_string(vm, std::ffi::CString::new("x").unwrap().as_ptr(), 1);
        dict_set(vm, dict, key, int_val(42));

        let mut result: Value = 0;
        let found = dict_get(dict, key, &mut result);
        assert!(found);
        assert_eq!(as_int(result), 42);

        vm_free(vm);
    }
}

#[test]
fn test_gc_collect() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let source = std::ffi::CString::new("let x = 1 let y = 2").unwrap();
        vm_interpret(vm, source.as_ptr());
        gc_collect(vm);

        vm_free(vm);
    }
}

#[test]
fn test_table_api() {
    unsafe {
        let mut table: Table = std::mem::zeroed();
        table_init(&mut table);

        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let key = copy_string(vm, std::ffi::CString::new("test").unwrap().as_ptr(), 4);
        table_set(&mut table, key, int_val(99));

        let mut val: Value = 0;
        let found = table_get(&mut table, key, &mut val);
        assert!(found);
        assert_eq!(as_int(val), 99);

        table_free(&mut table);
        vm_free(vm);
    }
}

#[test]
fn test_scanner() {
    unsafe {
        let source = std::ffi::CString::new("let x = 1").unwrap();
        let mut scanner: Scanner = std::mem::zeroed();
        scanner_init(&mut scanner, source.as_ptr());

        let tok = scan_token(&mut scanner);
        assert_eq!(tok.type_, MgTokenType::Let);

        let tok2 = scan_token(&mut scanner);
        assert_eq!(tok2.type_, MgTokenType::Identifier);
    }
}

#[test]
fn test_parse() {
    unsafe {
        let source = std::ffi::CString::new("let x = 1 + 2").unwrap();
        let mut had_error: bool = false;
        let ast = parse(source.as_ptr(), &mut had_error);
        assert!(!had_error);
        assert!(!ast.is_null());
        ast_free(ast);
    }
}

#[test]
fn test_typecheck_api() {
    unsafe {
        let source = std::ffi::CString::new("!strict\nlet x: number = \"bad\"").unwrap();
        let mut had_error: bool = false;
        let ast = parse(source.as_ptr(), &mut had_error);
        assert!(!had_error);
        assert!(!ast.is_null());
        assert!(mg_ast_has_strict(ast));
        assert!(!mg_typecheck_ast(ast, false));
        ast_free(ast);
    }
}

#[test]
fn test_vm_interpret_named() {
    unsafe {
        let mut vm: std::mem::MaybeUninit<VM> = std::mem::MaybeUninit::uninit();
        vm_init(vm.as_mut_ptr());
        let vm = vm.assume_init_mut();

        let source = std::ffi::CString::new("let x = 1 + 2 print(x)").unwrap();
        let name = std::ffi::CString::new("<test>").unwrap();
        let result = vm_interpret_named(vm, source.as_ptr(), name.as_ptr());
        assert_eq!(result, InterpretResult::Ok);

        vm_free(vm);
    }
}
