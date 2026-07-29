use magnesium_sys as sys;
use std::cell::{Cell, RefCell, UnsafeCell};
use std::mem::ManuallyDrop;
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::rc::{Rc, Weak};

use crate::value::Value;
use crate::vm::{BorrowedVm, Vm, VmInner};

unsafe fn report_callback_panic(vm: *mut sys::VM) {
    const MESSAGE: &[u8] = b"Rust native callback panicked\0";
    if !vm.is_null() {
        sys::mg_runtime_error_simple(vm, MESSAGE.as_ptr().cast());
    }
}

fn is_raw_object(raw: sys::Value) -> bool {
    (raw & (sys::SIGN_BIT | sys::QNAN)) == (sys::SIGN_BIT | sys::QNAN) && (raw & 0x7) == 0
}

unsafe fn raw_string<'a>(vm: *mut sys::VM, raw: sys::Value) -> Option<&'a str> {
    if !is_raw_object(raw) {
        return None;
    }
    let object = (raw & !(sys::SIGN_BIT | sys::QNAN)) as *mut sys::Obj;
    if object.is_null() || (*object).type_ != sys::ObjType::String {
        return None;
    }
    let string = &*(object.cast::<sys::ObjString>());
    if string.length < 0 {
        return None;
    }
    let chars = if string.is_rope {
        sys::string_chars(vm, object.cast())
    } else {
        string.chars
    };
    if chars.is_null() {
        return None;
    }
    let bytes = std::slice::from_raw_parts(chars.cast::<u8>(), string.length as usize);
    std::str::from_utf8(bytes).ok()
}

pub struct NativeContext<'vm> {
    pub(crate) vm: BorrowedVm<'vm>,
    pub(crate) args: &'vm [sys::Value],
}

impl<'vm> NativeContext<'vm> {
    pub fn vm(&mut self) -> &mut BorrowedVm<'vm> {
        &mut self.vm
    }

    pub fn arg(&self, index: usize) -> Option<Value> {
        let raw = *self.args.get(index)?;
        match self.vm.owner.as_ref() {
            Some(owner) => Some(Value::from_vm_raw(raw, Rc::clone(owner))),
            None if is_raw_object(raw) => None,
            None => Some(unsafe { Value::from_raw(raw) }),
        }
    }

    pub fn arg_count(&self) -> usize {
        self.args.len()
    }

    pub fn arg_bool(&self, index: usize) -> Option<bool> {
        self.arg(index).and_then(|v| v.try_as_bool())
    }

    pub fn arg_int(&self, index: usize) -> Option<i32> {
        self.arg(index).and_then(|v| v.try_as_int())
    }

    pub fn arg_number(&self, index: usize) -> Option<f64> {
        self.arg(index).and_then(|v| v.try_as_number())
    }

    pub fn arg_numeric(&self, index: usize) -> Option<f64> {
        self.arg(index).and_then(|v| v.try_as_numeric())
    }

    pub fn arg_string(&self, index: usize) -> Option<&str> {
        let raw = *self.args.get(index)?;
        // Arguments are rooted by the active native call for this lifetime.
        unsafe { raw_string(self.vm.raw_ptr(), raw) }
    }

    pub fn expect_arg(&self, index: usize) -> Value {
        self.arg(index).unwrap_or_else(|| {
            panic!(
                "argument index {} out of range (have {} args)",
                index,
                self.args.len()
            )
        })
    }

    pub fn expect_bool(&self, index: usize) -> bool {
        self.arg_bool(index)
            .unwrap_or_else(|| panic!("argument {} is not a bool", index))
    }

    pub fn expect_int(&self, index: usize) -> i32 {
        self.arg_int(index)
            .unwrap_or_else(|| panic!("argument {} is not an int", index))
    }

    pub fn expect_number(&self, index: usize) -> f64 {
        self.arg_number(index)
            .unwrap_or_else(|| panic!("argument {} is not a number", index))
    }

    pub fn expect_numeric(&self, index: usize) -> f64 {
        self.arg_numeric(index)
            .unwrap_or_else(|| panic!("argument {} is not numeric", index))
    }

    pub fn expect_string(&self, index: usize) -> &str {
        self.arg_string(index)
            .unwrap_or_else(|| panic!("argument {} is not a string", index))
    }

    pub unsafe fn from_raw(
        vm: *mut sys::VM,
        arg_count: libc::c_int,
        args: *mut sys::Value,
    ) -> Self {
        let slice = if args.is_null() || arg_count <= 0 {
            &[]
        } else {
            std::slice::from_raw_parts(args, arg_count as usize)
        };
        NativeContext {
            vm: BorrowedVm::from_raw(vm, None).expect("native callback received a null VM"),
            args: slice,
        }
    }
}

type NativeClosure = Box<dyn FnMut(NativeContext) -> Value>;
type MethodCallback<T> = Box<dyn FnMut(&mut T, NativeContext) -> Value>;

pub(crate) trait VmResourceLifecycle {
    fn clear(&self);
}

struct NativeClosureWrap {
    inner: RefCell<Option<NativeClosure>>,
    owner: Weak<VmInner>,
}

pub(crate) struct MethodClosure<T> {
    pub(crate) type_name_cstr: std::ffi::CString,
    pub(crate) f: RefCell<Option<MethodCallback<T>>>,
    pub(crate) owner: Weak<VmInner>,
}

#[repr(C)]
pub(crate) struct NativeData<T> {
    // This must remain the first field: C exposes the allocation pointer as
    // `T*` through vm_native_handle_data.
    value: UnsafeCell<ManuallyDrop<T>>,
    alive: Cell<bool>,
}

impl<T> NativeData<T> {
    pub(crate) fn new(value: T) -> Self {
        Self {
            value: UnsafeCell::new(ManuallyDrop::new(value)),
            alive: Cell::new(true),
        }
    }

    fn drop_value(&self) {
        if self.alive.replace(false) {
            let _ = catch_unwind(AssertUnwindSafe(|| unsafe {
                ManuallyDrop::drop(&mut *self.value.get());
            }));
        }
    }
}

impl<T> VmResourceLifecycle for NativeData<T> {
    fn clear(&self) {
        self.drop_value();
    }
}

impl<T> Drop for NativeData<T> {
    fn drop(&mut self) {
        self.drop_value();
    }
}

pub(crate) unsafe extern "C" fn native_data_finalizer<T: 'static>(data: *mut libc::c_void) {
    if !data.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(Rc::from_raw(data as *const NativeData<T>));
        }));
    }
}

unsafe extern "C" fn native_closure_finalizer(data: *mut libc::c_void) {
    if !data.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(Rc::from_raw(data as *const NativeClosureWrap));
        }));
    }
}

impl VmResourceLifecycle for NativeClosureWrap {
    fn clear(&self) {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            if let Ok(mut callback) = self.inner.try_borrow_mut() {
                drop(callback.take());
            }
        }));
    }
}

impl<T: 'static> MethodClosure<T> {
    pub(crate) fn new(
        type_name_cstr: std::ffi::CString,
        owner: Weak<VmInner>,
        f: impl FnMut(&mut T, NativeContext) -> Value + 'static,
    ) -> Self {
        MethodClosure {
            type_name_cstr,
            f: RefCell::new(Some(Box::new(f))),
            owner,
        }
    }
}

impl<T: 'static> VmResourceLifecycle for MethodClosure<T> {
    fn clear(&self) {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            if let Ok(mut callback) = self.f.try_borrow_mut() {
                drop(callback.take());
            }
        }));
    }
}

pub(crate) unsafe extern "C" fn method_finalizer<T: 'static>(data: *mut libc::c_void) {
    if !data.is_null() {
        let _ = catch_unwind(AssertUnwindSafe(|| {
            drop(Rc::from_raw(data as *const MethodClosure<T>));
        }));
    }
}

pub(crate) unsafe extern "C" fn method_trampoline<T: 'static>(
    vm: *mut sys::VM,
    arg_count: libc::c_int,
    args: *mut sys::Value,
) -> sys::Value {
    match catch_unwind(AssertUnwindSafe(|| {
        if vm.is_null() || args.is_null() || arg_count < 1 {
            return Value::null().to_raw();
        }

        let userdata = sys::mg_get_native_userdata(vm);
        if userdata.is_null() {
            return Value::null().to_raw();
        }

        let closure = &*(userdata as *const MethodClosure<T>);
        let Some(owner) = closure.owner.upgrade() else {
            return Value::null().to_raw();
        };

        let Ok(mut callback_slot) = closure.f.try_borrow_mut() else {
            return Value::null().to_raw();
        };
        let Some(callback) = callback_slot.as_mut() else {
            return Value::null().to_raw();
        };

        let self_value = *args;
        let data = sys::vm_native_handle_data(self_value, closure.type_name_cstr.as_ptr());
        if data.is_null() {
            return Value::null().to_raw();
        }
        let typed_data = &mut *data.cast::<T>();

        let slice = if arg_count > 1 {
            std::slice::from_raw_parts(args.add(1), (arg_count - 1) as usize)
        } else {
            &[]
        };

        let ctx = NativeContext {
            vm: BorrowedVm::from_raw(vm, Some(Rc::clone(&owner)))
                .expect("callback VM was checked for null"),
            args: slice,
        };
        let result = callback(typed_data, ctx);
        if !result.belongs_to(&owner) {
            return Value::null().to_raw();
        }
        result.to_raw()
    })) {
        Ok(value) => value,
        Err(_) => {
            report_callback_panic(vm);
            Value::null().to_raw()
        }
    }
}

unsafe extern "C" fn native_trampoline(
    vm: *mut sys::VM,
    arg_count: libc::c_int,
    args: *mut sys::Value,
) -> sys::Value {
    match catch_unwind(AssertUnwindSafe(|| {
        if vm.is_null() {
            return Value::null().to_raw();
        }
        let userdata = sys::mg_get_native_userdata(vm);
        if userdata.is_null() {
            return Value::null().to_raw();
        }

        let wrap = &*userdata.cast::<NativeClosureWrap>();
        let Some(owner) = wrap.owner.upgrade() else {
            return Value::null().to_raw();
        };
        let Ok(mut callback_slot) = wrap.inner.try_borrow_mut() else {
            return Value::null().to_raw();
        };
        let Some(callback) = callback_slot.as_mut() else {
            return Value::null().to_raw();
        };
        let slice = if args.is_null() || arg_count <= 0 {
            &[]
        } else {
            std::slice::from_raw_parts(args, arg_count as usize)
        };
        let ctx = NativeContext {
            vm: BorrowedVm::from_raw(vm, Some(Rc::clone(&owner)))
                .expect("callback VM was checked for null"),
            args: slice,
        };
        let result = callback(ctx);
        if !result.belongs_to(&owner) {
            return Value::null().to_raw();
        }
        result.to_raw()
    })) {
        Ok(value) => value,
        Err(_) => {
            report_callback_panic(vm);
            Value::null().to_raw()
        }
    }
}

impl Vm {
    pub fn register_native_fn(
        &mut self,
        name: &str,
        arity: libc::c_int,
        f: impl FnMut(NativeContext) -> Value + 'static,
    ) -> Result<(), crate::error::HostError> {
        crate::vm::validate_arity(arity)?;
        let c_name = crate::vm::cstring("name", name)?;

        let closure: NativeClosure = Box::new(f);
        let wrap = Rc::new(NativeClosureWrap {
            inner: RefCell::new(Some(closure)),
            owner: Rc::downgrade(&self.inner),
        });
        self.track_resource(&wrap);
        let userdata = Rc::into_raw(wrap).cast_mut().cast::<libc::c_void>();

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
            match ::std::panic::catch_unwind(::std::panic::AssertUnwindSafe(|| {
                let mut _arg_idx: usize = 0;
                $(
                    let $arg: $ty = {
                        let val = if !_args.is_null() && _arg_idx < _arg_count as usize {
                            unsafe { $crate::Value::from_raw(*_args.add(_arg_idx)) }
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
            })) {
                Ok(value) => value,
                Err(_) => {
                    const MESSAGE: &[u8] = b"Rust native callback panicked\0";
                    if !_vm.is_null() {
                        unsafe {
                            $crate::magnesium_sys::mg_runtime_error_simple(
                                _vm,
                                MESSAGE.as_ptr().cast(),
                            );
                        }
                    }
                    $crate::Value::null().to_raw()
                }
            }
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
