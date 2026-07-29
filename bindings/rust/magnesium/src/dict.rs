use magnesium_sys as sys;
use std::ptr::NonNull;
use std::rc::Rc;

use crate::string::MgString;
use crate::value::Value;
use crate::vm::{RootHandle, Vm};

pub struct MgDict {
    raw: NonNull<sys::ObjDict>,
    root: Option<Rc<RootHandle>>,
}

impl MgDict {
    /// The caller must keep the dictionary and its VM alive while this
    /// unrooted wrapper is used.
    pub unsafe fn from_raw(raw: *mut sys::ObjDict) -> Option<Self> {
        Some(Self {
            raw: NonNull::new(raw)?,
            root: None,
        })
    }

    fn from_vm(raw: *mut sys::ObjDict, vm: &Vm) -> Option<Self> {
        let raw = NonNull::new(raw)?;
        let value = sys::SIGN_BIT | sys::QNAN | raw.as_ptr() as u64;
        Some(Self {
            raw,
            root: Some(RootHandle::new(Rc::clone(&vm.inner), value)),
        })
    }

    pub fn raw(&self) -> *mut sys::ObjDict {
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

    pub fn version(&self) -> u32 {
        unsafe { self.raw.as_ref().version }
    }

    pub fn get(&self, key: &MgString) -> Option<Value> {
        let root = self.root.as_ref()?;
        if !key.belongs_to_inner(root.owner()) {
            return None;
        }
        let mut value: sys::Value = 0;
        let found = unsafe { sys::dict_get(self.raw.as_ptr(), key.raw(), &mut value) };
        if found {
            Some(Value::from_vm_raw(value, Rc::clone(root.owner())))
        } else {
            None
        }
    }

    pub fn set(vm: &mut Vm, dict: &mut Self, key: &MgString, value: &Value) -> bool {
        let Some(root) = dict.root.as_ref() else {
            return false;
        };
        if !Rc::ptr_eq(root.owner(), &vm.inner)
            || !key.belongs_to(vm)
            || !value.belongs_to(&vm.inner)
        {
            return false;
        }
        unsafe { sys::dict_set(vm.raw_mut(), dict.raw.as_ptr(), key.raw(), value.raw_ref()) }
    }

    pub fn delete(&mut self, key: &MgString) -> bool {
        let Some(root) = self.root.as_ref() else {
            return false;
        };
        if !key.belongs_to_inner(root.owner()) {
            return false;
        }
        unsafe { sys::dict_delete(self.raw.as_ptr(), key.raw()) }
    }
}

pub fn new_dict(vm: &mut Vm) -> MgDict {
    let raw = unsafe { sys::new_dict(vm.raw_mut()) };
    MgDict::from_vm(raw, vm).expect("new_dict returned null")
}
