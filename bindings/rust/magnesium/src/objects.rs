use magnesium_sys as sys;

use crate::string::MgString;
use crate::value::Value;
use crate::vm::Vm;

pub struct MgFunction(*mut sys::ObjFunction);

impl MgFunction {
    pub unsafe fn from_raw(raw: *mut sys::ObjFunction) -> Self {
        MgFunction(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjFunction {
        self.0
    }

    pub fn arity(&self) -> libc::c_int {
        unsafe { (*self.0).arity }
    }

    pub fn name(&self) -> &str {
        unsafe {
            let name_ptr = (*self.0).name;
            if name_ptr.is_null() {
                "<script>"
            } else {
                let obj = &*name_ptr;
                if obj.chars.is_null() || obj.is_rope {
                    "<unnamed>"
                } else {
                    let slice =
                        std::slice::from_raw_parts(obj.chars as *const u8, obj.length as usize);
                    std::str::from_utf8_unchecked(slice)
                }
            }
        }
    }

    pub fn upvalue_count(&self) -> libc::c_int {
        unsafe { (*self.0).upvalue_count }
    }

    pub fn reg_count(&self) -> libc::c_int {
        unsafe { (*self.0).reg_count }
    }
}

pub fn new_function(vm: &mut Vm) -> MgFunction {
    let raw = unsafe { sys::new_function(vm.raw_mut()) };
    MgFunction(raw)
}

pub struct MgClosure(*mut sys::ObjClosure);

impl MgClosure {
    pub unsafe fn from_raw(raw: *mut sys::ObjClosure) -> Self {
        MgClosure(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjClosure {
        self.0
    }

    pub fn function(&self) -> MgFunction {
        unsafe { MgFunction((*self.0).function) }
    }

    pub fn upvalue_count(&self) -> libc::c_int {
        unsafe { (*self.0).upvalue_count }
    }

    pub unsafe fn upvalue(&self, index: usize) -> Option<*mut sys::ObjUpvalue> {
        if index >= self.upvalue_count() as usize {
            return None;
        }
        Some((*self.0).upvalue(index))
    }
}

pub fn new_closure(vm: &mut Vm, function: &MgFunction) -> MgClosure {
    let raw = unsafe { sys::new_closure(vm.raw_mut(), function.0) };
    MgClosure(raw)
}

pub struct MgStruct(*mut sys::ObjStruct);

impl MgStruct {
    pub unsafe fn from_raw(raw: *mut sys::ObjStruct) -> Self {
        MgStruct(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjStruct {
        self.0
    }

    pub fn name(&self) -> &str {
        unsafe {
            let name_ptr = (*self.0).name;
            if name_ptr.is_null() {
                "<unnamed>"
            } else {
                let obj = &*name_ptr;
                if obj.chars.is_null() || obj.is_rope {
                    "<unnamed>"
                } else {
                    let slice =
                        std::slice::from_raw_parts(obj.chars as *const u8, obj.length as usize);
                    std::str::from_utf8_unchecked(slice)
                }
            }
        }
    }

    pub fn field_count(&self) -> libc::c_int {
        unsafe { (*self.0).field_count }
    }

    pub fn methods(&self) -> Option<&sys::ObjDict> {
        unsafe {
            let m = (*self.0).methods;
            if m.is_null() {
                None
            } else {
                Some(&*m)
            }
        }
    }
}

pub fn new_struct(vm: &mut Vm, name: &MgString) -> MgStruct {
    let raw = unsafe { sys::new_struct(vm.raw_mut(), name.raw()) };
    MgStruct(raw)
}

pub struct MgInstance(*mut sys::ObjInstance);

impl MgInstance {
    pub unsafe fn from_raw(raw: *mut sys::ObjInstance) -> Self {
        MgInstance(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjInstance {
        self.0
    }

    pub fn klass(&self) -> MgStruct {
        unsafe { MgStruct((*self.0).klass) }
    }

    pub unsafe fn field(&self, index: usize) -> Value {
        Value::from_raw((*self.0).field(index))
    }

    pub unsafe fn set_field(&mut self, index: usize, value: Value) {
        (*self.0).set_field(index, value.to_raw())
    }
}

pub fn new_instance(vm: &mut Vm, klass: &MgStruct) -> MgInstance {
    let raw = unsafe { sys::new_instance(vm.raw_mut(), klass.raw()) };
    MgInstance(raw)
}

pub struct MgCoroutine(*mut sys::ObjCoroutine);

impl MgCoroutine {
    pub unsafe fn from_raw(raw: *mut sys::ObjCoroutine) -> Self {
        MgCoroutine(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjCoroutine {
        self.0
    }

    pub fn state(&self) -> sys::CoroutineState {
        unsafe { (*self.0).state }
    }

    pub fn closure(&self) -> MgClosure {
        unsafe { MgClosure((*self.0).closure) }
    }

    pub fn frame_count(&self) -> libc::c_int {
        unsafe { (*self.0).frame_count }
    }
}

pub fn new_coroutine(vm: &mut Vm, closure: &MgClosure) -> MgCoroutine {
    let raw = unsafe { sys::new_coroutine(vm.raw_mut(), closure.raw()) };
    MgCoroutine(raw)
}

pub struct MgFfi(*mut sys::ObjFFI);

impl MgFfi {
    pub unsafe fn from_raw(raw: *mut sys::ObjFFI) -> Self {
        MgFfi(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjFFI {
        self.0
    }

    pub fn name(&self) -> &str {
        unsafe {
            let name_ptr = (*self.0).name;
            if name_ptr.is_null() {
                ""
            } else {
                std::ffi::CStr::from_ptr(name_ptr).to_str().unwrap_or("")
            }
        }
    }

    pub fn arity(&self) -> libc::c_int {
        unsafe { (*self.0).arity }
    }
}
