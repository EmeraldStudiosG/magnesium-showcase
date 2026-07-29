use std::ptr::NonNull;
use std::rc::Rc;

use magnesium_sys as sys;

use crate::string::MgString;
use crate::value::Value;
use crate::vm::{RootHandle, Vm};

struct ObjectHandle<T> {
    raw: NonNull<T>,
    root: Option<Rc<RootHandle>>,
}

impl<T> ObjectHandle<T> {
    unsafe fn from_raw(raw: *mut T) -> Option<Self> {
        Some(Self {
            raw: NonNull::new(raw)?,
            root: None,
        })
    }

    fn from_vm(raw: *mut T, vm: &Vm) -> Option<Self> {
        let raw = NonNull::new(raw)?;
        let value = sys::SIGN_BIT | sys::QNAN | raw.as_ptr() as u64;
        Some(Self {
            raw,
            root: Some(RootHandle::new(Rc::clone(&vm.inner), value)),
        })
    }

    fn child<U>(&self, raw: *mut U) -> Option<ObjectHandle<U>> {
        Some(ObjectHandle {
            raw: NonNull::new(raw)?,
            root: self.root.as_ref().map(Rc::clone),
        })
    }

    fn belongs_to(&self, vm: &Vm) -> bool {
        self.root
            .as_ref()
            .is_some_and(|root| Rc::ptr_eq(root.owner(), &vm.inner))
    }
}

unsafe fn object_string<'a>(raw: *mut sys::ObjString, fallback: &'static str) -> &'a str {
    let Some(string) = raw.as_ref() else {
        return fallback;
    };
    if string.is_rope || string.chars.is_null() || string.length < 0 {
        return fallback;
    }
    let bytes = std::slice::from_raw_parts(string.chars.cast::<u8>(), string.length as usize);
    std::str::from_utf8(bytes).unwrap_or(fallback)
}

pub struct MgFunction(ObjectHandle<sys::ObjFunction>);

impl MgFunction {
    pub unsafe fn from_raw(raw: *mut sys::ObjFunction) -> Option<Self> {
        ObjectHandle::from_raw(raw).map(Self)
    }

    pub fn raw(&self) -> *mut sys::ObjFunction {
        self.0.raw.as_ptr()
    }

    pub fn arity(&self) -> libc::c_int {
        unsafe { self.0.raw.as_ref().arity }
    }

    pub fn name(&self) -> &str {
        let name = unsafe { self.0.raw.as_ref().name };
        unsafe {
            object_string(
                name,
                if name.is_null() {
                    "<script>"
                } else {
                    "<unnamed>"
                },
            )
        }
    }

    pub fn upvalue_count(&self) -> libc::c_int {
        unsafe { self.0.raw.as_ref().upvalue_count }
    }

    pub fn reg_count(&self) -> libc::c_int {
        unsafe { self.0.raw.as_ref().reg_count }
    }
}

pub fn new_function(vm: &mut Vm) -> MgFunction {
    let raw = unsafe { sys::new_function(vm.raw_mut()) };
    MgFunction(ObjectHandle::from_vm(raw, vm).expect("new_function returned null"))
}

pub struct MgClosure(ObjectHandle<sys::ObjClosure>);

impl MgClosure {
    pub unsafe fn from_raw(raw: *mut sys::ObjClosure) -> Option<Self> {
        ObjectHandle::from_raw(raw).map(Self)
    }

    pub fn raw(&self) -> *mut sys::ObjClosure {
        self.0.raw.as_ptr()
    }

    pub fn function(&self) -> Option<MgFunction> {
        let raw = unsafe { self.0.raw.as_ref().function };
        self.0.child(raw).map(MgFunction)
    }

    pub fn upvalue_count(&self) -> libc::c_int {
        unsafe { self.0.raw.as_ref().upvalue_count }
    }

    pub unsafe fn upvalue(&self, index: usize) -> Option<*mut sys::ObjUpvalue> {
        if index >= self.upvalue_count().max(0) as usize {
            return None;
        }
        Some(self.0.raw.as_ref().upvalue(index))
    }
}

pub fn new_closure(vm: &mut Vm, function: &MgFunction) -> Option<MgClosure> {
    if !function.0.belongs_to(vm) {
        return None;
    }
    let raw = unsafe { sys::new_closure(vm.raw_mut(), function.raw()) };
    ObjectHandle::from_vm(raw, vm).map(MgClosure)
}

pub struct MgStruct(ObjectHandle<sys::ObjStruct>);

impl MgStruct {
    pub unsafe fn from_raw(raw: *mut sys::ObjStruct) -> Option<Self> {
        ObjectHandle::from_raw(raw).map(Self)
    }

    pub fn raw(&self) -> *mut sys::ObjStruct {
        self.0.raw.as_ptr()
    }

    pub fn name(&self) -> &str {
        unsafe { object_string(self.0.raw.as_ref().name, "<unnamed>") }
    }

    pub fn field_count(&self) -> libc::c_int {
        unsafe { self.0.raw.as_ref().field_count }
    }

    pub fn methods(&self) -> Option<&sys::ObjDict> {
        unsafe { self.0.raw.as_ref().methods.as_ref() }
    }
}

pub fn new_struct(vm: &mut Vm, name: &MgString) -> Option<MgStruct> {
    if !name.belongs_to(vm) {
        return None;
    }
    let raw = unsafe { sys::new_struct(vm.raw_mut(), name.raw()) };
    ObjectHandle::from_vm(raw, vm).map(MgStruct)
}

pub struct MgInstance(ObjectHandle<sys::ObjInstance>);

impl MgInstance {
    pub unsafe fn from_raw(raw: *mut sys::ObjInstance) -> Option<Self> {
        ObjectHandle::from_raw(raw).map(Self)
    }

    pub fn raw(&self) -> *mut sys::ObjInstance {
        self.0.raw.as_ptr()
    }

    pub fn klass(&self) -> Option<MgStruct> {
        let raw = unsafe { self.0.raw.as_ref().klass };
        self.0.child(raw).map(MgStruct)
    }

    pub unsafe fn field(&self, index: usize) -> Option<Value> {
        let class = self.0.raw.as_ref().klass.as_ref()?;
        if index >= class.field_count.max(0) as usize {
            return None;
        }
        let raw = self.0.raw.as_ref().field(index);
        Some(match self.0.root.as_ref() {
            Some(root) => Value::from_vm_raw(raw, Rc::clone(root.owner())),
            None => Value::from_raw(raw),
        })
    }

    pub unsafe fn set_field(&mut self, index: usize, value: &Value) -> bool {
        let Some(class) = self.0.raw.as_ref().klass.as_ref() else {
            return false;
        };
        if index >= class.field_count.max(0) as usize {
            return false;
        }
        let Some(root) = self.0.root.as_ref() else {
            return false;
        };
        if !value.belongs_to(root.owner()) {
            return false;
        }
        let instance_obj = self.0.raw.as_ptr().cast::<sys::Obj>();
        if (*instance_obj).is_old && value.is_obj() {
            let value_obj = value.as_obj();
            if !(*value_obj).is_old {
                sys::remembered_set_add(root.owner().raw(), instance_obj);
            }
        }
        self.0.raw.as_mut().set_field(index, value.raw_ref());
        true
    }
}

pub fn new_instance(vm: &mut Vm, class: &MgStruct) -> Option<MgInstance> {
    if !class.0.belongs_to(vm) {
        return None;
    }
    let raw = unsafe { sys::new_instance(vm.raw_mut(), class.raw()) };
    ObjectHandle::from_vm(raw, vm).map(MgInstance)
}

pub struct MgCoroutine(ObjectHandle<sys::ObjCoroutine>);

impl MgCoroutine {
    pub unsafe fn from_raw(raw: *mut sys::ObjCoroutine) -> Option<Self> {
        ObjectHandle::from_raw(raw).map(Self)
    }

    pub fn raw(&self) -> *mut sys::ObjCoroutine {
        self.0.raw.as_ptr()
    }

    pub fn state(&self) -> sys::CoroutineState {
        unsafe { self.0.raw.as_ref().state }
    }

    pub fn closure(&self) -> Option<MgClosure> {
        let raw = unsafe { self.0.raw.as_ref().closure };
        self.0.child(raw).map(MgClosure)
    }

    pub fn frame_count(&self) -> libc::c_int {
        unsafe { self.0.raw.as_ref().frame_count }
    }
}

pub fn new_coroutine(vm: &mut Vm, closure: &MgClosure) -> Option<MgCoroutine> {
    if !closure.0.belongs_to(vm) {
        return None;
    }
    let raw = unsafe { sys::new_coroutine(vm.raw_mut(), closure.raw()) };
    ObjectHandle::from_vm(raw, vm).map(MgCoroutine)
}

pub struct MgFfi(ObjectHandle<sys::ObjFFI>);

impl MgFfi {
    pub unsafe fn from_raw(raw: *mut sys::ObjFFI) -> Option<Self> {
        ObjectHandle::from_raw(raw).map(Self)
    }

    pub fn raw(&self) -> *mut sys::ObjFFI {
        self.0.raw.as_ptr()
    }

    pub fn name(&self) -> &str {
        unsafe { object_string(self.0.raw.as_ref().name, "") }
    }

    pub fn arity(&self) -> libc::c_int {
        unsafe { self.0.raw.as_ref().arity }
    }
}
