use magnesium_sys as sys;
use std::ptr::NonNull;
use std::rc::Rc;

use crate::value::Value;
use crate::vm::{RootHandle, Vm};

pub struct MgArray {
    raw: NonNull<sys::ObjArray>,
    root: Option<Rc<RootHandle>>,
}

impl MgArray {
    /// The caller must keep the array and its VM alive while this wrapper is
    /// used. Object-valued elements are not returned from an unrooted wrapper.
    pub unsafe fn from_raw(raw: *mut sys::ObjArray) -> Option<Self> {
        Some(Self {
            raw: NonNull::new(raw)?,
            root: None,
        })
    }

    fn from_vm(raw: *mut sys::ObjArray, vm: &Vm) -> Option<Self> {
        let raw = NonNull::new(raw)?;
        let value = sys::SIGN_BIT | sys::QNAN | raw.as_ptr() as u64;
        Some(Self {
            raw,
            root: Some(RootHandle::new(Rc::clone(&vm.inner), value)),
        })
    }

    pub fn raw(&self) -> *mut sys::ObjArray {
        self.raw.as_ptr()
    }

    pub fn len(&self) -> libc::c_int {
        unsafe { self.raw.as_ref().count }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn capacity(&self) -> libc::c_int {
        unsafe { self.raw.as_ref().capacity }
    }

    pub fn get(&self, index: libc::c_int) -> Option<Value> {
        if index < 0 || index >= self.len() {
            return None;
        }
        let raw = unsafe { sys::array_get(self.raw.as_ptr(), index) };
        match self.root.as_ref() {
            Some(root) => Some(Value::from_vm_raw(raw, Rc::clone(root.owner()))),
            None if raw_is_object(raw) => None,
            None => Some(unsafe { Value::from_raw(raw) }),
        }
    }

    pub fn set(&mut self, index: libc::c_int, value: &Value) -> bool {
        if index < 0 || index >= self.len() {
            return false;
        }
        let Some(root) = self.root.as_ref() else {
            return false;
        };
        if !value.belongs_to(root.owner()) {
            return false;
        }
        unsafe {
            sys::array_set(
                root.owner().raw(),
                self.raw.as_ptr(),
                index,
                value.raw_ref(),
            )
        };
        true
    }

    pub fn push(vm: &mut Vm, array: &mut Self, value: &Value) -> bool {
        let Some(root) = array.root.as_ref() else {
            return false;
        };
        if !Rc::ptr_eq(root.owner(), &vm.inner) || !value.belongs_to(&vm.inner) {
            return false;
        }
        unsafe { sys::array_push(vm.raw_mut(), array.raw.as_ptr(), value.raw_ref()) };
        true
    }
}

pub fn new_array(vm: &mut Vm) -> MgArray {
    let raw = unsafe { sys::new_array(vm.raw_mut()) };
    MgArray::from_vm(raw, vm).expect("new_array returned null")
}

fn raw_is_object(raw: sys::Value) -> bool {
    (raw & (sys::SIGN_BIT | sys::QNAN)) == (sys::SIGN_BIT | sys::QNAN) && (raw & 0x7) == 0
}
