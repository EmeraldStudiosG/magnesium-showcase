use magnesium_sys as sys;

use crate::value::Value;
use crate::vm::Vm;

pub struct GcRoot<'vm> {
    vm: &'vm mut Vm,
}

impl<'vm> GcRoot<'vm> {
    pub fn new(vm: &'vm mut Vm, value: Value) -> Self {
        vm.push(value);
        GcRoot { vm }
    }
}

impl<'vm> Drop for GcRoot<'vm> {
    fn drop(&mut self) {
        self.vm.pop();
    }
}

pub fn gc_collect(vm: &mut Vm) {
    unsafe { sys::gc_collect(vm.raw_mut()) };
}

pub fn gc_mark_value(vm: &mut Vm, value: Value) {
    unsafe { sys::gc_mark_value(vm.raw_mut(), value.to_raw()) };
}

pub unsafe fn gc_mark_object(vm: &mut Vm, obj: *mut sys::Obj) {
    sys::gc_mark_object(vm.raw_mut(), obj)
}
