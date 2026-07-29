use magnesium_sys as sys;

use crate::value::Value;
use crate::vm::Vm;

pub struct GcRoot {
    value: Value,
}

impl GcRoot {
    pub fn new(vm: &Vm, value: Value) -> Self {
        assert!(
            value.belongs_to(&vm.inner),
            "cannot root a value owned by another VM"
        );
        Self { value }
    }

    pub fn get(&self) -> &Value {
        &self.value
    }
}

pub fn gc_collect(vm: &mut Vm) {
    unsafe { sys::gc_collect(vm.raw_mut()) };
}

pub fn gc_mark_value(vm: &mut Vm, value: &Value) {
    if value.belongs_to(&vm.inner) {
        unsafe { sys::gc_mark_value(vm.raw_mut(), value.raw_ref()) };
    }
}

pub unsafe fn gc_mark_object(vm: &mut Vm, obj: *mut sys::Obj) {
    sys::gc_mark_object(vm.raw_mut(), obj)
}
