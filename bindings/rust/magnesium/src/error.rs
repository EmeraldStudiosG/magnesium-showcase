use std::ffi::CStr;
use std::fmt;

use magnesium_sys as sys;

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum HostError {
    InteriorNul { field: &'static str },
    AllocationFailed { operation: &'static str },
    InvalidFunctionHandle,
    InvalidNativeHandle,
}

impl fmt::Display for HostError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            HostError::InteriorNul { field } => {
                write!(f, "{} contains an interior null byte", field)
            }
            HostError::AllocationFailed { operation } => {
                write!(f, "{} failed to allocate or register", operation)
            }
            HostError::InvalidFunctionHandle => {
                write!(f, "function handle does not belong to this VM")
            }
            HostError::InvalidNativeHandle => write!(f, "value is not a native handle"),
        }
    }
}

impl std::error::Error for HostError {}

#[derive(Debug)]
pub enum InterpretError {
    CompileError,
    RuntimeError(MgError),
    Yield,
    HostError(HostError),
}

#[derive(Debug)]
pub struct MgError {
    raw: *mut sys::ObjError,
    kind: String,
    message: String,
    file: String,
    function: String,
    hint: String,
    line: libc::c_int,
}

impl MgError {
    pub unsafe fn from_raw(raw: *mut sys::ObjError) -> Self {
        if raw.is_null() {
            return MgError::empty(raw);
        }

        MgError {
            raw,
            kind: Self::obj_string_to_string((*raw).kind),
            message: Self::obj_string_to_string((*raw).message),
            file: Self::obj_string_to_string((*raw).file),
            function: Self::obj_string_to_string((*raw).function_),
            hint: Self::obj_string_to_string((*raw).hint),
            line: (*raw).line,
        }
    }

    pub unsafe fn from_vm(vm: *mut sys::VM) -> Self {
        if vm.is_null() {
            return MgError::empty(std::ptr::null_mut());
        }

        MgError {
            raw: std::ptr::null_mut(),
            kind: "RuntimeError".to_string(),
            message: Self::c_str_to_string(sys::vm_last_error(vm)),
            file: Self::c_str_to_string(sys::vm_last_error_file(vm)),
            function: Self::c_str_to_string(sys::vm_last_error_function(vm)),
            hint: String::new(),
            line: sys::vm_last_error_line(vm),
        }
    }

    pub fn raw(&self) -> *mut sys::ObjError {
        self.raw
    }

    fn empty(raw: *mut sys::ObjError) -> Self {
        MgError {
            raw,
            kind: String::new(),
            message: String::new(),
            file: String::new(),
            function: String::new(),
            hint: String::new(),
            line: 0,
        }
    }

    fn c_str_to_string(s: *const libc::c_char) -> String {
        if s.is_null() {
            String::new()
        } else {
            unsafe { CStr::from_ptr(s).to_string_lossy().into_owned() }
        }
    }

    fn obj_string_to_string(s: *mut sys::ObjString) -> String {
        if s.is_null() {
            String::new()
        } else {
            unsafe {
                let obj = &*s;
                if obj.chars.is_null() || obj.is_rope {
                    String::new()
                } else {
                    let slice =
                        std::slice::from_raw_parts(obj.chars as *const u8, obj.length as usize);
                    String::from_utf8_lossy(slice).into_owned()
                }
            }
        }
    }

    pub fn kind(&self) -> &str {
        &self.kind
    }

    pub fn message(&self) -> &str {
        &self.message
    }

    pub fn file(&self) -> &str {
        &self.file
    }

    pub fn line(&self) -> libc::c_int {
        self.line
    }

    pub fn function(&self) -> &str {
        &self.function
    }

    pub fn hint(&self) -> &str {
        &self.hint
    }
}

impl fmt::Display for MgError {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "[{}:{}] {}", self.kind(), self.line(), self.message())
    }
}

impl std::error::Error for MgError {}

pub enum InterpretResult {
    Ok,
    Err(InterpretError),
}

impl InterpretResult {
    pub fn from_raw(raw: sys::InterpretResult) -> Self {
        Self::from_raw_with_vm(raw, std::ptr::null_mut())
    }

    pub fn from_raw_with_vm(raw: sys::InterpretResult, vm: *mut sys::VM) -> Self {
        match raw {
            sys::InterpretResult::Ok => InterpretResult::Ok,
            sys::InterpretResult::CompileError => {
                InterpretResult::Err(InterpretError::CompileError)
            }
            sys::InterpretResult::RuntimeError => {
                InterpretResult::Err(InterpretError::RuntimeError(unsafe {
                    MgError::from_vm(vm)
                }))
            }
            sys::InterpretResult::Yield => InterpretResult::Err(InterpretError::Yield),
        }
    }

    pub fn host_error(error: HostError) -> Self {
        InterpretResult::Err(InterpretError::HostError(error))
    }

    pub fn is_ok(&self) -> bool {
        matches!(self, InterpretResult::Ok)
    }

    pub fn is_err(&self) -> bool {
        !self.is_ok()
    }
}

impl From<InterpretResult> for Result<(), InterpretError> {
    fn from(r: InterpretResult) -> Self {
        match r {
            InterpretResult::Ok => Ok(()),
            InterpretResult::Err(e) => Err(e),
        }
    }
}
