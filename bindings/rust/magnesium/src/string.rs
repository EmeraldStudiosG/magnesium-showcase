use std::ptr::NonNull;
use std::rc::Rc;

use magnesium_sys as sys;

use crate::vm::{RootHandle, Vm};

pub struct MgString {
    raw: NonNull<sys::ObjString>,
    root: Option<Rc<RootHandle>>,
}

impl MgString {
    /// The caller must keep `raw` and its owning VM alive for the lifetime of
    /// the returned wrapper.
    pub unsafe fn from_raw(raw: *mut sys::ObjString) -> Option<Self> {
        Some(Self {
            raw: NonNull::new(raw)?,
            root: None,
        })
    }

    pub(crate) fn from_vm(raw: *mut sys::ObjString, vm: &Vm) -> Option<Self> {
        let raw = NonNull::new(raw)?;
        let value = sys::SIGN_BIT | sys::QNAN | raw.as_ptr() as u64;
        Some(Self {
            raw,
            root: Some(RootHandle::new(Rc::clone(&vm.inner), value)),
        })
    }

    pub fn raw(&self) -> *mut sys::ObjString {
        self.raw.as_ptr()
    }

    pub fn as_str(&self) -> Option<&str> {
        unsafe {
            let string = self.raw.as_ref();
            if string.is_rope || string.chars.is_null() || string.length < 0 {
                return None;
            }
            let bytes =
                std::slice::from_raw_parts(string.chars.cast::<u8>(), string.length as usize);
            std::str::from_utf8(bytes).ok()
        }
    }

    pub fn resolve(&self) -> Option<&str> {
        unsafe {
            let string = self.raw.as_ref();
            if string.length < 0 {
                return None;
            }
            let vm = self.root.as_ref()?.owner();
            let value = sys::SIGN_BIT | sys::QNAN | self.raw.as_ptr() as u64;
            let chars = sys::vm_string_chars_resolved(vm.raw(), value);
            if chars.is_null() {
                return None;
            }
            let bytes = std::slice::from_raw_parts(chars.cast::<u8>(), string.length as usize);
            std::str::from_utf8(bytes).ok()
        }
    }

    pub fn to_owned_string(&self) -> Option<String> {
        let root = self.root.as_ref()?;
        let value = sys::SIGN_BIT | sys::QNAN | self.raw.as_ptr() as u64;
        let mut required = 0usize;
        unsafe {
            sys::vm_string_copy(
                root.owner().raw(),
                value,
                std::ptr::null_mut(),
                0,
                &mut required,
            );
        }
        if required == 0 {
            return None;
        }
        let mut bytes = vec![0u8; required];
        let copied = unsafe {
            sys::vm_string_copy(
                root.owner().raw(),
                value,
                bytes.as_mut_ptr().cast(),
                bytes.len(),
                &mut required,
            )
        };
        if !copied || required == 0 || required > bytes.len() {
            return None;
        }
        bytes.truncate(required - 1);
        String::from_utf8(bytes).ok()
    }

    pub fn length(&self) -> libc::c_int {
        unsafe { self.raw.as_ref().length }
    }

    pub fn hash(&self) -> u32 {
        unsafe { self.raw.as_ref().hash }
    }

    pub fn is_rope(&self) -> bool {
        unsafe { self.raw.as_ref().is_rope }
    }

    pub fn char_at(&self, index: libc::c_int) -> Option<u8> {
        if index < 0 || index >= self.length() {
            return None;
        }
        Some(unsafe { sys::string_char_at(self.raw.as_ptr(), index) as u8 })
    }

    pub(crate) fn belongs_to(&self, vm: &Vm) -> bool {
        self.belongs_to_inner(&vm.inner)
    }

    pub(crate) fn belongs_to_inner(&self, owner: &Rc<crate::vm::VmInner>) -> bool {
        self.root
            .as_ref()
            .is_some_and(|root| Rc::ptr_eq(root.owner(), owner))
    }
}

pub fn copy_string(vm: &mut Vm, string: &str) -> MgString {
    let length = libc::c_int::try_from(string.len()).expect("string is too long for Magnesium");
    let raw = unsafe { sys::copy_string(vm.raw_mut(), string.as_ptr().cast(), length) };
    MgString::from_vm(raw, vm).expect("copy_string returned null")
}

pub fn concat_strings(vm: &mut Vm, left: &MgString, right: &MgString) -> Option<MgString> {
    if !left.belongs_to(vm) || !right.belongs_to(vm) {
        return None;
    }
    let raw = unsafe { sys::concat_strings(vm.raw_mut(), left.raw(), right.raw()) };
    MgString::from_vm(raw, vm)
}
