use magnesium_sys as sys;

use crate::value::Value;
use crate::vm::Vm;

pub struct NativeContext<'vm> {
    pub(crate) vm: &'vm mut Vm,
    pub(crate) args: &'vm [Value],
}

impl<'vm> NativeContext<'vm> {
    pub fn vm(&mut self) -> &mut Vm {
        self.vm
    }

    pub fn arg(&self, index: usize) -> Option<Value> {
        self.args.get(index).copied()
    }

    pub fn arg_count(&self) -> usize {
        self.args.len()
    }

    pub fn arg_bool(&self, index: usize) -> Option<bool> {
        self.args.get(index).and_then(|v| v.try_as_bool())
    }

    pub fn arg_int(&self, index: usize) -> Option<i32> {
        self.args.get(index).and_then(|v| v.try_as_int())
    }

    pub fn arg_number(&self, index: usize) -> Option<f64> {
        self.args.get(index).and_then(|v| v.try_as_number())
    }

    pub fn arg_numeric(&self, index: usize) -> Option<f64> {
        self.args.get(index).and_then(|v| v.try_as_numeric())
    }

    pub fn arg_string(&self, index: usize) -> Option<&str> {
        self.args.get(index).and_then(|v| v.try_as_string())
    }

    pub fn expect_arg(&self, index: usize) -> Value {
        self.args.get(index).copied().unwrap_or_else(|| {
            panic!("argument index {} out of range (have {} args)", index, self.args.len())
        })
    }

    pub fn expect_bool(&self, index: usize) -> bool {
        self.arg_bool(index).unwrap_or_else(|| {
            panic!("argument {} is not a bool", index)
        })
    }

    pub fn expect_int(&self, index: usize) -> i32 {
        self.arg_int(index).unwrap_or_else(|| {
            panic!("argument {} is not an int", index)
        })
    }

    pub fn expect_number(&self, index: usize) -> f64 {
        self.arg_number(index).unwrap_or_else(|| {
            panic!("argument {} is not a number", index)
        })
    }

    pub fn expect_numeric(&self, index: usize) -> f64 {
        self.arg_numeric(index).unwrap_or_else(|| {
            panic!("argument {} is not numeric", index)
        })
    }

    pub fn expect_string(&self, index: usize) -> &str {
        self.arg_string(index).unwrap_or_else(|| {
            panic!("argument {} is not a string", index)
        })
    }

    pub unsafe fn from_raw(vm: *mut sys::VM, arg_count: libc::c_int, args: *mut sys::Value) -> Self {
        let slice = if args.is_null() || arg_count <= 0 {
            &[]
        } else {
            std::slice::from_raw_parts(args as *const Value, arg_count as usize)
        };
        NativeContext {
            vm: &mut *(vm as *mut Vm),
            args: slice,
        }
    }
}

type NativeClosure = Box<dyn FnMut(NativeContext) -> Value>;

struct NativeClosureWrap {
    inner: NativeClosure,
}

pub struct MethodClosure<T> {
    pub type_name_cstr: std::ffi::CString,
    pub f: std::sync::Mutex<Box<dyn FnMut(&mut T, NativeContext) -> Value>>,
}

unsafe extern "C" fn native_closure_finalizer(data: *mut libc::c_void) {
    if !data.is_null() {
        let _ = Box::from_raw(data as *mut NativeClosureWrap);
    }
}

impl<T: 'static> MethodClosure<T> {
    pub fn new(type_name_cstr: std::ffi::CString, f: impl FnMut(&mut T, NativeContext) -> Value + 'static) -> Self {
        MethodClosure {
            type_name_cstr,
            f: std::sync::Mutex::new(Box::new(f)),
        }
    }
}

pub unsafe extern "C" fn method_finalizer<T: 'static>(data: *mut libc::c_void) {
    if !data.is_null() {
        let _ = Box::from_raw(data as *mut MethodClosure<T>);
    }
}

pub unsafe extern "C" fn method_trampoline<T: 'static>(
    vm: *mut sys::VM,
    arg_count: libc::c_int,
    args: *mut sys::Value,
) -> sys::Value {
    if args.is_null() || arg_count < 1 {
        return Value::null().to_raw();
    }

    let userdata = sys::mg_get_native_userdata(vm);
    if userdata.is_null() {
        return Value::null().to_raw();
    }

    let closure = &*(userdata as *const MethodClosure<T>);

    let self_val = *(args as *const Value);
    let data = sys::vm_native_handle_data(self_val.to_raw(), closure.type_name_cstr.as_ptr());
    if data.is_null() {
        return Value::null().to_raw();
    }
    let typed_data = &mut *(data as *mut T);

    let mut guard = match closure.f.lock() {
        Ok(g) => g,
        Err(_) => return Value::null().to_raw(),
    };

    let slice = if arg_count > 1 {
        std::slice::from_raw_parts(
            (args as *const Value).add(1),
            (arg_count - 1) as usize,
        )
    } else {
        &[]
    };

    let ctx = NativeContext {
        vm: &mut *(vm as *mut Vm),
        args: slice,
    };

    (*guard)(typed_data, ctx).to_raw()
}

unsafe extern "C" fn native_trampoline(
    vm: *mut sys::VM,
    arg_count: libc::c_int,
    args: *mut sys::Value,
) -> sys::Value {
    let userdata = sys::mg_get_native_userdata(vm);
    if userdata.is_null() {
        return Value::null().to_raw();
    }

    let wrap = &mut *(userdata as *mut NativeClosureWrap);
    let closure = &mut wrap.inner;

    let slice = if args.is_null() || arg_count <= 0 {
        &[]
    } else {
        std::slice::from_raw_parts(args as *const Value, arg_count as usize)
    };

    let vm_ref = &mut *(vm as *mut Vm);
    let ctx = NativeContext { vm: vm_ref, args: slice };
    (closure)(ctx).to_raw()
}

impl Vm {
    pub fn register_native_fn(
        &mut self,
        name: &str,
        arity: libc::c_int,
        f: impl FnMut(NativeContext) -> Value + 'static,
    ) -> Result<(), crate::error::HostError> {
        let c_name = crate::vm::cstring("name", name)?;

        let closure: NativeClosure = Box::new(f);
        let wrap = Box::new(NativeClosureWrap { inner: closure });
        let userdata = Box::into_raw(wrap) as *mut libc::c_void;

        unsafe {
            sys::vm_register_native(
                self.raw_mut(),
                c_name.as_ptr(),
                Some(native_trampoline),
                arity,
                userdata,
                Some(native_closure_finalizer),
            );
        }
        Ok(())
    }
}

pub trait TryFromMgValue: Sized {
    fn try_from_mg_value(v: Value) -> Option<Self>;
}

impl TryFromMgValue for bool {
    fn try_from_mg_value(v: Value) -> Option<Self> {
        v.try_as_bool()
    }
}

impl TryFromMgValue for i32 {
    fn try_from_mg_value(v: Value) -> Option<Self> {
        v.try_as_int()
    }
}

impl TryFromMgValue for f64 {
    fn try_from_mg_value(v: Value) -> Option<Self> {
        v.try_as_numeric()
    }
}

impl TryFromMgValue for Value {
    fn try_from_mg_value(v: Value) -> Option<Self> {
        Some(v)
    }
}

#[macro_export]
macro_rules! mg_native {
    (
        $(#[$meta:meta])*
        $name:ident ( $ctx:ident $(, $arg:ident : $ty:ty)* ) -> Value {
            $($body:tt)*
        }
    ) => {
        $(#[$meta])*
        unsafe extern "C" fn $name(
            _vm: *mut $crate::magnesium_sys::VM,
            _arg_count: libc::c_int,
            _args: *mut $crate::magnesium_sys::Value,
        ) -> $crate::magnesium_sys::Value {
            let mut _arg_idx: usize = 0;
            $(
                let $arg: $ty = {
                    let val = if !_args.is_null() && _arg_idx < _arg_count as usize {
                        *(_args as *const $crate::Value).add(_arg_idx)
                    } else {
                        return $crate::Value::null().to_raw();
                    };
                    _arg_idx += 1;
                    match <$ty as $crate::native::TryFromMgValue>::try_from_mg_value(val) {
                        Some(v) => v,
                        None => return $crate::Value::null().to_raw(),
                    }
                };
            )*
            let $ctx = unsafe { $crate::NativeContext::from_raw(_vm, _arg_count, _args) };
            ($($body)*).to_raw()
        }
    };

    (
        $(#[$meta:meta])*
        $name:ident ( $ctx:ident $(, $arg:ident : $ty:ty)* ) {
            $($body:tt)*
        }
    ) => {
        $crate::mg_native! {
            $(#[$meta])*
            $name ( $ctx $(, $arg : $ty )* ) -> Value {
                $($body)*
            }
        }
    };
}
