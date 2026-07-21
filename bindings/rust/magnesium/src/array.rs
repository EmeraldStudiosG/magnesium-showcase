use magnesium_sys as sys;

use crate::value::Value;
use crate::vm::Vm;

pub struct MgArray(*mut sys::ObjArray);

impl MgArray {
    pub unsafe fn from_raw(raw: *mut sys::ObjArray) -> Self {
        MgArray(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjArray {
        self.0
    }

    pub fn len(&self) -> libc::c_int {
        unsafe { (*self.0).count }
    }

    pub fn is_empty(&self) -> bool {
        self.len() == 0
    }

    pub fn capacity(&self) -> libc::c_int {
        unsafe { (*self.0).capacity }
    }

    pub fn get(&self, index: libc::c_int) -> Option<Value> {
        if index < 0 || index >= self.len() {
            return None;
        }
        let raw = unsafe { sys::array_get(self.0, index) };
        Some(Value::from_raw(raw))
    }

    pub fn set(&mut self, index: libc::c_int, value: Value) -> bool {
        if index < 0 || index >= self.len() {
            return false;
        }
        unsafe { sys::array_set(self.0, index, value.to_raw()) };
        true
    }

    pub fn push(vm: &mut Vm, array: &mut Self, value: Value) {
        unsafe { sys::array_push(vm.raw_mut(), array.0, value.to_raw()) };
    }
}

pub fn new_array(vm: &mut Vm) -> MgArray {
    let raw = unsafe { sys::new_array(vm.raw_mut()) };
    MgArray(raw)
}
