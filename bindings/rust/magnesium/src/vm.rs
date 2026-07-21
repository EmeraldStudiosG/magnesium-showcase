use magnesium_sys as sys;
use std::ffi::{CStr, CString};
use std::ptr::NonNull;

use crate::error::{HostError, InterpretResult};
use crate::value::Value;

pub type Ffi1 = extern "C" fn(f64) -> f64;
pub type Ffi2 = extern "C" fn(f64, f64) -> f64;

pub(crate) fn cstring(field: &'static str, value: &str) -> Result<CString, HostError> {
    CString::new(value).map_err(|_| HostError::InteriorNul { field })
}

fn c_string(ptr: *const libc::c_char) -> String {
    if ptr.is_null() {
        String::new()
    } else {
        unsafe { CStr::from_ptr(ptr).to_string_lossy().into_owned() }
    }
}

#[derive(Clone, Copy, Debug)]
pub struct FunctionHandle {
    raw: NonNull<sys::ObjFunction>,
    vm: NonNull<sys::VM>,
}

impl FunctionHandle {
    pub fn as_raw(self) -> *mut sys::ObjFunction {
        self.raw.as_ptr()
    }
}

pub struct Vm {
    raw: NonNull<sys::VM>,
}

impl Vm {
    pub fn new() -> Self {
        let raw = unsafe { sys::vm_new() };
        Self {
            raw: NonNull::new(raw).expect("vm_new returned null"),
        }
    }

    pub fn try_interpret(&mut self, source: &str) -> Result<InterpretResult, HostError> {
        let c_source = cstring("source", source)?;
        let result = unsafe { sys::vm_interpret(self.raw_mut(), c_source.as_ptr()) };
        Ok(InterpretResult::from_raw_with_vm(result, self.raw_mut()))
    }

    pub fn interpret(&mut self, source: &str) -> InterpretResult {
        match self.try_interpret(source) {
            Ok(result) => result,
            Err(error) => InterpretResult::host_error(error),
        }
    }

    pub fn try_interpret_named(
        &mut self,
        source: &str,
        name: &str,
    ) -> Result<InterpretResult, HostError> {
        let c_source = cstring("source", source)?;
        let c_name = cstring("name", name)?;
        let result =
            unsafe { sys::vm_interpret_named(self.raw_mut(), c_source.as_ptr(), c_name.as_ptr()) };
        Ok(InterpretResult::from_raw_with_vm(result, self.raw_mut()))
    }

    pub fn interpret_named(&mut self, source: &str, name: &str) -> InterpretResult {
        match self.try_interpret_named(source, name) {
            Ok(result) => result,
            Err(error) => InterpretResult::host_error(error),
        }
    }

    pub fn try_compile(
        &mut self,
        source: &str,
    ) -> Result<Option<*mut sys::ObjFunction>, HostError> {
        Ok(self.try_compile_handle(source)?.map(FunctionHandle::as_raw))
    }

    pub fn compile(&mut self, source: &str) -> Option<*mut sys::ObjFunction> {
        self.try_compile(source).ok().flatten()
    }

    pub fn try_compile_handle(
        &mut self,
        source: &str,
    ) -> Result<Option<FunctionHandle>, HostError> {
        let c_source = cstring("source", source)?;
        let func = unsafe { sys::vm_compile(self.raw_mut(), c_source.as_ptr()) };
        Ok(NonNull::new(func).map(|raw| FunctionHandle { raw, vm: self.raw }))
    }

    pub fn compile_handle(&mut self, source: &str) -> Option<FunctionHandle> {
        self.try_compile_handle(source).ok().flatten()
    }

    pub fn try_compile_named(
        &mut self,
        source: &str,
        name: &str,
    ) -> Result<Option<*mut sys::ObjFunction>, HostError> {
        Ok(self
            .try_compile_named_handle(source, name)?
            .map(FunctionHandle::as_raw))
    }

    pub fn compile_named(&mut self, source: &str, name: &str) -> Option<*mut sys::ObjFunction> {
        self.try_compile_named(source, name).ok().flatten()
    }

    pub fn try_compile_named_handle(
        &mut self,
        source: &str,
        name: &str,
    ) -> Result<Option<FunctionHandle>, HostError> {
        let c_source = cstring("source", source)?;
        let c_name = cstring("name", name)?;
        let func =
            unsafe { sys::vm_compile_named(self.raw_mut(), c_source.as_ptr(), c_name.as_ptr()) };
        Ok(NonNull::new(func).map(|raw| FunctionHandle { raw, vm: self.raw }))
    }

    pub fn compile_named_handle(&mut self, source: &str, name: &str) -> Option<FunctionHandle> {
        self.try_compile_named_handle(source, name).ok().flatten()
    }

    pub fn try_type_check(&mut self, source: &str, force_check: bool) -> Result<bool, HostError> {
        let c_source = cstring("source", source)?;
        let mut had_error = false;
        let ast = unsafe { sys::parse(c_source.as_ptr(), &mut had_error) };
        if ast.is_null() {
            return Ok(false);
        }
        if had_error {
            unsafe { sys::ast_free(ast) };
            return Ok(false);
        }
        let ok = unsafe { sys::mg_typecheck_ast(ast, force_check) };
        unsafe { sys::ast_free(ast) };
        Ok(ok)
    }

    pub fn type_check(&mut self, source: &str, force_check: bool) -> bool {
        self.try_type_check(source, force_check).unwrap_or(false)
    }

    pub fn try_run_compiled(
        &mut self,
        function: FunctionHandle,
    ) -> Result<InterpretResult, HostError> {
        if function.vm != self.raw {
            return Err(HostError::InvalidFunctionHandle);
        }
        let result = unsafe { sys::vm_run_function(self.raw_mut(), function.raw.as_ptr()) };
        Ok(InterpretResult::from_raw_with_vm(result, self.raw_mut()))
    }

    pub fn run_compiled(&mut self, function: FunctionHandle) -> InterpretResult {
        match self.try_run_compiled(function) {
            Ok(result) => result,
            Err(error) => InterpretResult::host_error(error),
        }
    }

    pub unsafe fn run_function(&mut self, function: *mut sys::ObjFunction) -> InterpretResult {
        let result = sys::vm_run_function(self.raw_mut(), function);
        InterpretResult::from_raw_with_vm(result, self.raw_mut())
    }

    pub unsafe fn try_save_bytecode(
        &mut self,
        function: *mut sys::ObjFunction,
        path: &str,
    ) -> Result<bool, HostError> {
        let c_path = cstring("path", path)?;
        Ok(sys::vm_save_bytecode(
            self.raw_mut(),
            function,
            c_path.as_ptr(),
        ))
    }

    pub unsafe fn save_bytecode(&mut self, function: *mut sys::ObjFunction, path: &str) -> bool {
        self.try_save_bytecode(function, path).unwrap_or(false)
    }

    pub fn try_load_bytecode(
        &mut self,
        path: &str,
    ) -> Result<Option<*mut sys::ObjFunction>, HostError> {
        let c_path = cstring("path", path)?;
        let func = unsafe { sys::vm_load_bytecode(self.raw_mut(), c_path.as_ptr()) };
        Ok(if func.is_null() { None } else { Some(func) })
    }

    pub fn load_bytecode(&mut self, path: &str) -> Option<*mut sys::ObjFunction> {
        self.try_load_bytecode(path).ok().flatten()
    }

    pub fn push(&mut self, value: Value) {
        unsafe { sys::vm_push(self.raw_mut(), value.to_raw()) }
    }

    pub fn pop(&mut self) -> Value {
        Value::from_raw(unsafe { sys::vm_pop(self.raw_mut()) })
    }

    pub fn try_set_global(&mut self, name: &str, value: Value) -> Result<(), HostError> {
        let c_name = cstring("name", name)?;
        let ok =
            unsafe { sys::vm_set_global_value(self.raw_mut(), c_name.as_ptr(), value.to_raw()) };
        if ok {
            Ok(())
        } else {
            Err(HostError::AllocationFailed {
                operation: "set global",
            })
        }
    }

    pub fn set_global(&mut self, name: &str, value: Value) {
        let _ = self.try_set_global(name, value);
    }

    pub fn try_set_global_number(&mut self, name: &str, value: f64) -> Result<(), HostError> {
        self.try_set_global(name, Value::auto_val(value))
    }

    pub fn set_global_number(&mut self, name: &str, value: f64) {
        let _ = self.try_set_global_number(name, value);
    }

    pub fn try_set_global_bool(&mut self, name: &str, value: bool) -> Result<(), HostError> {
        self.try_set_global(name, Value::bool_val(value))
    }

    pub fn set_global_bool(&mut self, name: &str, value: bool) {
        let _ = self.try_set_global_bool(name, value);
    }

    pub fn try_set_global_string(&mut self, name: &str, value: &str) -> Result<(), HostError> {
        let c_value = cstring("value", value)?;
        let string = unsafe {
            sys::copy_string(
                self.raw_mut(),
                c_value.as_ptr(),
                value.len() as libc::c_int,
            )
        };
        if string.is_null() {
            return Err(HostError::AllocationFailed {
                operation: "copy string",
            });
        }
        self.try_set_global(name, Value::obj_val(string.cast()))
    }

    pub fn set_global_string(&mut self, name: &str, value: &str) {
        let _ = self.try_set_global_string(name, value);
    }

    pub fn new_array_value(&mut self) -> Result<Value, HostError> {
        let array = unsafe { sys::new_array(self.raw_mut()) };
        if array.is_null() {
            Err(HostError::AllocationFailed {
                operation: "new array",
            })
        } else {
            Ok(Value::obj_val(array.cast()))
        }
    }

    pub fn new_dict_value(&mut self) -> Result<Value, HostError> {
        let dict = unsafe { sys::new_dict(self.raw_mut()) };
        if dict.is_null() {
            Err(HostError::AllocationFailed {
                operation: "new dict",
            })
        } else {
            Ok(Value::obj_val(dict.cast()))
        }
    }

    pub fn try_get_global(&mut self, name: &str) -> Result<Option<Value>, HostError> {
        let c_name = cstring("name", name)?;
        let mut value: sys::Value = 0;
        let found =
            unsafe { sys::vm_get_global_value(self.raw_mut(), c_name.as_ptr(), &mut value) };
        Ok(if found {
            Some(Value::from_raw(value))
        } else {
            None
        })
    }

    pub fn get_global(&mut self, name: &str) -> Option<Value> {
        self.try_get_global(name).ok().flatten()
    }

    pub fn try_register_native(
        &mut self,
        name: &str,
        func: sys::NativeFn,
        arity: libc::c_int,
    ) -> Result<(), HostError> {
        let c_name = cstring("name", name)?;
        unsafe { sys::vm_register_native(self.raw_mut(), c_name.as_ptr(), func, arity,
                                         std::ptr::null_mut(), None) };
        Ok(())
    }

    pub fn register_native(&mut self, name: &str, func: sys::NativeFn, arity: libc::c_int) {
        let _ = self.try_register_native(name, func, arity);
    }

    pub fn register_ffi1(&mut self, name: &str, func: Ffi1) -> Result<(), HostError> {
        unsafe { self.register_ffi_raw_unchecked(name, func as *const () as *mut libc::c_void, 1) }
    }

    pub fn register_ffi2(&mut self, name: &str, func: Ffi2) -> Result<(), HostError> {
        unsafe { self.register_ffi_raw_unchecked(name, func as *const () as *mut libc::c_void, 2) }
    }

    pub unsafe fn register_ffi_raw_unchecked(
        &mut self,
        name: &str,
        func: *mut libc::c_void,
        arity: libc::c_int,
    ) -> Result<(), HostError> {
        let c_name = cstring("name", name)?;
        let ffi = sys::vm_register_ffi(self.raw_mut(), c_name.as_ptr(), func, arity);
        if ffi.is_null() {
            Err(HostError::AllocationFailed {
                operation: "register ffi",
            })
        } else {
            Ok(())
        }
    }

    pub fn try_new_native_handle<T: 'static>(
        &mut self,
        type_name: &str,
        data: T,
    ) -> Result<Value, HostError> {
        unsafe extern "C" fn drop_box<T>(data: *mut libc::c_void) {
            if !data.is_null() {
                drop(Box::from_raw(data.cast::<T>()));
            }
        }

        let c_type = cstring("type_name", type_name)?;
        let data = Box::into_raw(Box::new(data)).cast::<libc::c_void>();
        let raw = unsafe {
            sys::vm_new_native_handle(self.raw_mut(), c_type.as_ptr(), data, Some(drop_box::<T>))
        };
        if raw.is_null() {
            unsafe {
                drop(Box::from_raw(data.cast::<T>()));
            }
            Err(HostError::AllocationFailed {
                operation: "new native handle",
            })
        } else {
            Ok(Value::from_raw(unsafe { sys::vm_native_handle_value(raw) }))
        }
    }

    pub fn new_native_handle<T: 'static>(&mut self, type_name: &str, data: T) -> Value {
        self.try_new_native_handle(type_name, data)
            .unwrap_or_else(|_| Value::null())
    }

    pub fn try_set_native_handle_method(
        &mut self,
        handle: Value,
        name: &str,
        func: sys::NativeFn,
        arity: libc::c_int,
    ) -> Result<(), HostError> {
        if !handle.is_native_handle() {
            return Err(HostError::InvalidNativeHandle);
        }
        let c_name = cstring("name", name)?;
        let ok = unsafe {
            sys::vm_native_handle_set_method(
                self.raw_mut(),
                handle.as_native_handle(),
                c_name.as_ptr(),
                func,
                arity,
                std::ptr::null_mut(),
                None,
            )
        };
        if ok {
            Ok(())
        } else {
            Err(HostError::AllocationFailed {
                operation: "set native handle method",
            })
        }
    }

    pub fn set_native_handle_method(
        &mut self,
        handle: Value,
        name: &str,
        func: sys::NativeFn,
        arity: libc::c_int,
    ) {
        let _ = self.try_set_native_handle_method(handle, name, func, arity);
    }

    pub fn try_set_native_handle_method_fn<T: 'static>(
        &mut self,
        handle: Value,
        type_name: &str,
        method_name: &str,
        arity: libc::c_int,
        f: impl FnMut(&mut T, crate::native::NativeContext) -> Value + 'static,
    ) -> Result<(), HostError> {
        if !handle.is_native_handle() {
            return Err(HostError::InvalidNativeHandle);
        }
        let c_method = cstring("method_name", method_name)?;

        let type_name_cstr = std::ffi::CString::new(type_name).map_err(|_| {
            HostError::InteriorNul { field: "type_name" }
        })?;

        let closure = crate::native::MethodClosure::<T>::new(type_name_cstr, f);

        let userdata = Box::into_raw(Box::new(closure)) as *mut libc::c_void;

        let ok = unsafe {
            sys::vm_native_handle_set_method(
                self.raw_mut(),
                handle.as_native_handle(),
                c_method.as_ptr(),
                Some(crate::native::method_trampoline::<T>),
                arity,
                userdata,
                Some(crate::native::method_finalizer::<T>),
            )
        };
        if ok {
            Ok(())
        } else {
            unsafe { let _ = Box::from_raw(userdata as *mut crate::native::MethodClosure<T>); }
            Err(HostError::AllocationFailed {
                operation: "set native handle method fn",
            })
        }
    }

    pub fn set_native_handle_method_fn<T: 'static>(
        &mut self,
        handle: Value,
        type_name: &str,
        method_name: &str,
        arity: libc::c_int,
        f: impl FnMut(&mut T, crate::native::NativeContext) -> Value + 'static,
    ) {
        let _ = self.try_set_native_handle_method_fn(handle, type_name, method_name, arity, f);
    }

    pub fn native_handle_data_ref<T>(&self, value: Value, type_name: &str) -> Result<Option<&T>, HostError> {
        let c_type = cstring("type_name", type_name)?;
        let data = unsafe { sys::vm_native_handle_data(value.to_raw(), c_type.as_ptr()) };
        Ok(if data.is_null() {
            None
        } else {
            unsafe { Some(&*(data as *const T)) }
        })
    }

    pub fn native_handle_data_mut<T>(&self, value: Value, type_name: &str) -> Result<Option<&mut T>, HostError> {
        let c_type = cstring("type_name", type_name)?;
        let data = unsafe { sys::vm_native_handle_data(value.to_raw(), c_type.as_ptr()) };
        Ok(if data.is_null() {
            None
        } else {
            unsafe { Some(&mut *(data as *mut T)) }
        })
    }

    pub unsafe fn native_handle_data<T>(
        &self,
        value: Value,
        type_name: &str,
    ) -> Result<Option<*mut T>, HostError> {
        let c_type = cstring("type_name", type_name)?;
        let data = sys::vm_native_handle_data(value.to_raw(), c_type.as_ptr());
        Ok(if data.is_null() {
            None
        } else {
            Some(data.cast::<T>())
        })
    }

    pub fn last_error_message(&self) -> String {
        c_string(unsafe { sys::vm_last_error(self.raw.as_ptr()) })
    }

    pub fn last_error_trace(&self) -> String {
        c_string(unsafe { sys::vm_last_error_trace(self.raw.as_ptr()) })
    }

    pub fn last_error_file(&self) -> String {
        c_string(unsafe { sys::vm_last_error_file(self.raw.as_ptr()) })
    }

    pub fn last_error_function(&self) -> String {
        c_string(unsafe { sys::vm_last_error_function(self.raw.as_ptr()) })
    }

    pub fn last_error_line(&self) -> libc::c_int {
        unsafe { sys::vm_last_error_line(self.raw.as_ptr()) }
    }

    pub fn clear_error(&mut self) {
        unsafe { sys::vm_clear_error(self.raw_mut()) };
    }

    pub fn raw_mut(&mut self) -> *mut sys::VM {
        self.raw.as_ptr()
    }

    pub fn raw(&self) -> *const sys::VM {
        self.raw.as_ptr()
    }

    pub fn frame_count(&self) -> libc::c_int {
        unsafe { self.raw.as_ref().frame_count }
    }

    pub fn stack_top(&self) -> libc::c_int {
        unsafe { self.raw.as_ref().stack_top }
    }

    pub fn bytes_allocated(&self) -> usize {
        unsafe { self.raw.as_ref().bytes_allocated }
    }
}

impl Drop for Vm {
    fn drop(&mut self) {
        unsafe { sys::vm_delete(self.raw_mut()) };
    }
}

impl Default for Vm {
    fn default() -> Self {
        Self::new()
    }
}

struct _NotSendSync(#[allow(dead_code)] *const ());
unsafe impl Send for _NotSendSync {}
unsafe impl Sync for _NotSendSync {}

fn _assert_vm_not_send<T>() {
    fn _check<T>() {}
    _check::<_NotSendSync>();
}
