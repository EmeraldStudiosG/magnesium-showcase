use magnesium_sys as sys;

use crate::value::Value;

pub struct MgChunk(*mut sys::Chunk);

impl MgChunk {
    pub unsafe fn from_raw(raw: *mut sys::Chunk) -> Self {
        MgChunk(raw)
    }

    pub fn raw(&self) -> *mut sys::Chunk {
        self.0
    }

    pub fn count(&self) -> libc::c_int {
        unsafe { (*self.0).count }
    }

    pub fn capacity(&self) -> libc::c_int {
        unsafe { (*self.0).capacity }
    }

    pub fn const_count(&self) -> libc::c_int {
        unsafe { (*self.0).const_count }
    }

    pub unsafe fn instruction(&self, offset: libc::c_int) -> Option<sys::Instruction> {
        if offset < 0 || offset >= (*self.0).count {
            return None;
        }
        Some(*(*self.0).code.add(offset as usize))
    }

    pub unsafe fn constant(&self, index: libc::c_int) -> Option<Value> {
        if index < 0 || index >= (*self.0).const_count {
            return None;
        }
        Some(Value::from_raw(*(*self.0).constants.add(index as usize)))
    }

    pub unsafe fn line(&self, offset: libc::c_int) -> Option<libc::c_int> {
        if offset < 0 || offset >= (*self.0).count {
            return None;
        }
        Some(*(*self.0).lines.add(offset as usize))
    }
}

pub fn chunk_init(chunk: &mut sys::Chunk) {
    unsafe { sys::chunk_init(chunk) };
}

pub unsafe fn chunk_free(chunk: &mut sys::Chunk) {
    sys::chunk_free(chunk);
}

pub unsafe fn chunk_write(chunk: &mut sys::Chunk, inst: sys::Instruction, line: libc::c_int) {
    sys::chunk_write(chunk, inst, line);
}

pub unsafe fn chunk_add_constant(chunk: &mut sys::Chunk, value: &Value) -> libc::c_int {
    sys::chunk_add_constant(chunk, value.raw_ref())
}
