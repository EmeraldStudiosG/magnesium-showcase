use std::ffi::CStr;

use magnesium_sys as sys;

use crate::vm::Vm;

pub struct MgString(pub(crate) *mut sys::ObjString);

impl MgString {
    pub unsafe fn from_raw(raw: *mut sys::ObjString) -> Self {
        MgString(raw)
    }

    pub fn raw(&self) -> *mut sys::ObjString {
        self.0
    }

    pub fn as_str<'a>(&self) -> Option<&'a str> {
        unsafe {
            let s = &*self.0;
            if s.is_rope || s.chars.is_null() {
                None
            } else {
                let slice = std::slice::from_raw_parts(s.chars as *const u8, s.length as usize);
                Some(std::str::from_utf8_unchecked(slice))
            }
        }
    }

    pub fn resolve<'a>(vm: &mut Vm, s: &Self) -> Option<&'a str> {
        unsafe {
            let ptr = sys::string_chars(vm.raw_mut(), s.0);
            if ptr.is_null() {
                None
            } else {
                let cstr = CStr::from_ptr(ptr);
                Some(cstr.to_str().unwrap_or(""))
            }
        }
    }

    pub fn length(&self) -> libc::c_int {
        unsafe { (*self.0).length }
    }

    pub fn hash(&self) -> u32 {
        unsafe { (*self.0).hash }
    }

    pub fn is_rope(&self) -> bool {
        unsafe { (*self.0).is_rope }
    }

    pub fn char_at(&self, index: libc::c_int) -> Option<u8> {
        unsafe {
            if index < 0 || index >= (*self.0).length {
                return None;
            }
            Some(sys::string_char_at(self.0, index) as u8)
        }
    }
}

pub fn copy_string(vm: &mut Vm, s: &str) -> MgString {
    let c_str = std::ffi::CString::new(s).expect("string contains null byte");
    let raw = unsafe { sys::copy_string(vm.raw_mut(), c_str.as_ptr(), s.len() as libc::c_int) };
    MgString(raw)
}

pub fn concat_strings(vm: &mut Vm, a: &MgString, b: &MgString) -> MgString {
    let raw = unsafe { sys::concat_strings(vm.raw_mut(), a.0, b.0) };
    MgString(raw)
}
