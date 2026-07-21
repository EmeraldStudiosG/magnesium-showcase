use magnesium_sys as sys;

use crate::string::MgString;
use crate::value::Value;

pub struct MgTable(sys::Table);

impl MgTable {
    pub fn new() -> Self {
        let mut table: sys::Table = unsafe { std::mem::zeroed() };
        unsafe { sys::table_init(&mut table) };
        MgTable(table)
    }

    pub unsafe fn from_raw(raw: sys::Table) -> Self {
        MgTable(raw)
    }

    pub fn raw(&self) -> &sys::Table {
        &self.0
    }

    pub fn raw_mut(&mut self) -> &mut sys::Table {
        &mut self.0
    }

    pub fn len(&self) -> libc::c_int {
        self.0.count
    }

    pub fn is_empty(&self) -> bool {
        self.0.count == 0
    }

    pub fn get(&self, key: &MgString) -> Option<Value> {
        let mut value: sys::Value = 0;
        let found = unsafe { sys::table_get(&self.0 as *const _ as *mut _, key.raw(), &mut value) };
        if found {
            Some(Value::from_raw(value))
        } else {
            None
        }
    }

    pub fn set(&mut self, key: &MgString, value: Value) -> bool {
        unsafe { sys::table_set(&mut self.0, key.raw(), value.to_raw()) }
    }

    pub fn delete(&mut self, key: &MgString) -> bool {
        unsafe { sys::table_delete(&mut self.0, key.raw()) }
    }

    pub fn find_string(&self, chars: &str) -> Option<MgString> {
        let c_str = std::ffi::CString::new(chars).expect("string contains null byte");
        let mut hash: u32 = 2166136261;
        for b in chars.as_bytes() {
            hash ^= *b as u32;
            hash = hash.wrapping_mul(16777619);
        }
        let key = unsafe {
            sys::table_find_string(
                &self.0 as *const _ as *mut _,
                c_str.as_ptr(),
                chars.len() as libc::c_int,
                hash,
            )
        };
        if key.is_null() {
            None
        } else {
            Some(MgString(key))
        }
    }
}

impl Drop for MgTable {
    fn drop(&mut self) {
        unsafe { sys::table_free(&mut self.0) };
    }
}

impl Default for MgTable {
    fn default() -> Self {
        Self::new()
    }
}
