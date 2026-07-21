use magnesium_sys as sys;

use crate::string::MgString;
use crate::value::Value;
use crate::vm::Vm;

pub struct MgDict(*mut sys::ObjDict);

impl MgDict {
    pub unsafe fn from_raw(raw: *mut sys::ObjDict) -> Self {
        MgDict(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjDict {
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

    pub fn version(&self) -> u32 {
        unsafe { (*self.0).version }
    }

    pub fn get(&self, key: &MgString) -> Option<Value> {
        let mut value: sys::Value = 0;
        let found = unsafe { sys::dict_get(self.0, key.raw(), &mut value) };
        if found {
            Some(Value::from_raw(value))
        } else {
            None
        }
    }

    pub fn set(vm: &mut Vm, dict: &mut Self, key: &MgString, value: Value) -> bool {
        unsafe { sys::dict_set(vm.raw_mut(), dict.0, key.raw(), value.to_raw()) }
    }

    pub fn delete(&mut self, key: &MgString) -> bool {
        unsafe { sys::dict_delete(self.0, key.raw()) }
    }
}

pub fn new_dict(vm: &mut Vm) -> MgDict {
    let raw = unsafe { sys::new_dict(vm.raw_mut()) };
    MgDict(raw)
}
