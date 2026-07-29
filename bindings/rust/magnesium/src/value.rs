use std::ffi::c_void;
use std::rc::Rc;

use magnesium_sys as sys;

use crate::vm::{RootHandle, VmInner};

const QNAN: u64 = sys::QNAN;
const SIGN_BIT: u64 = sys::SIGN_BIT;
const TAG_NULL: u64 = sys::TAG_NULL;
const TAG_FALSE: u64 = sys::TAG_FALSE;
const TAG_TRUE: u64 = sys::TAG_TRUE;
const TAG_INT: u64 = sys::TAG_INT;

#[derive(Clone)]
pub struct Value {
    raw: u64,
    // VM-backed values keep their VM alive and retain the object through the
    // host-root API. Clones share one root.
    root: Option<Rc<RootHandle>>,
    // Only unsafe constructors can create an unowned object value.
    trusted_unowned: bool,
}

impl Value {
    #[inline(always)]
    const fn primitive(raw: u64) -> Self {
        Self {
            raw,
            root: None,
            trusted_unowned: false,
        }
    }

    #[inline(always)]
    pub const fn null() -> Self {
        Self::primitive(QNAN | TAG_NULL)
    }

    #[inline(always)]
    pub const fn false_val() -> Self {
        Self::primitive(QNAN | TAG_FALSE)
    }

    #[inline(always)]
    pub const fn true_val() -> Self {
        Self::primitive(QNAN | TAG_TRUE)
    }

    #[inline(always)]
    pub const fn bool_val(b: bool) -> Self {
        if b {
            Self::true_val()
        } else {
            Self::false_val()
        }
    }

    #[inline(always)]
    pub const fn int_val(n: i32) -> Self {
        Self::primitive(QNAN | TAG_INT | ((n as u32 as u64) << 3))
    }

    #[inline(always)]
    pub fn number_val(n: f64) -> Self {
        let bits = n.to_bits();
        // A user-controlled NaN payload must never be interpreted as an
        // object pointer by the NaN-boxing scheme.
        if (bits & QNAN) == QNAN {
            Self::primitive(f64::NAN.to_bits())
        } else {
            Self::primitive(bits)
        }
    }

    #[inline(always)]
    pub unsafe fn obj_val(ptr: *const c_void) -> Self {
        Self {
            raw: SIGN_BIT | QNAN | (ptr as u64),
            root: None,
            trusted_unowned: true,
        }
    }

    #[inline(always)]
    pub fn auto_val(n: f64) -> Self {
        if n >= i32::MIN as f64 && n <= i32::MAX as f64 {
            let w = n as i32;
            if w as f64 == n {
                return Self::int_val(w);
            }
        }
        Self::number_val(n)
    }

    #[inline(always)]
    pub fn is_null(&self) -> bool {
        self.raw == (QNAN | TAG_NULL)
    }

    #[inline(always)]
    pub fn is_bool(&self) -> bool {
        (self.raw | 1) == (QNAN | TAG_TRUE)
    }

    #[inline(always)]
    pub fn is_int(&self) -> bool {
        (self.raw & (SIGN_BIT | QNAN | 0x7)) == (QNAN | TAG_INT)
    }

    #[inline(always)]
    pub fn is_number(&self) -> bool {
        (self.raw & QNAN) != QNAN
    }

    #[inline(always)]
    pub fn is_numeric(&self) -> bool {
        self.is_int() || self.is_number()
    }

    #[inline(always)]
    pub fn is_obj(&self) -> bool {
        (self.raw & (SIGN_BIT | QNAN)) == (SIGN_BIT | QNAN) && (self.raw & 0x7) == 0
    }

    #[inline(always)]
    pub fn as_bool(&self) -> bool {
        self.raw == (QNAN | TAG_TRUE)
    }

    #[inline(always)]
    pub fn as_int(&self) -> i32 {
        ((self.raw >> 3) & 0xFFFF_FFFF) as i32
    }

    #[inline(always)]
    pub fn as_number(&self) -> f64 {
        if self.is_int() {
            self.as_int() as f64
        } else {
            f64::from_bits(self.raw)
        }
    }

    #[inline(always)]
    pub fn as_double(&self) -> f64 {
        f64::from_bits(self.raw)
    }

    #[inline(always)]
    pub unsafe fn as_obj(&self) -> *mut sys::Obj {
        (self.raw & !(SIGN_BIT | QNAN)) as *mut sys::Obj
    }

    #[inline(always)]
    pub fn is_falsey(&self) -> bool {
        self.is_null() || (self.is_bool() && !self.as_bool())
    }

    #[inline(always)]
    pub fn obj_type(&self) -> Option<sys::ObjType> {
        if self.is_obj() && (self.root.is_some() || self.trusted_unowned) {
            Some(unsafe { (*self.as_obj()).type_ })
        } else {
            None
        }
    }

    #[inline(always)]
    pub fn is_string(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::String)
    }

    #[inline(always)]
    pub fn is_array(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Array)
    }

    #[inline(always)]
    pub fn is_dict(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Dict)
    }

    #[inline(always)]
    pub fn is_function(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Function)
    }

    #[inline(always)]
    pub fn is_closure(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Closure)
    }

    #[inline(always)]
    pub fn is_native(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Native)
    }

    #[inline(always)]
    pub fn is_ffi(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Ffi)
    }

    #[inline(always)]
    pub fn is_native_handle(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::NativeHandle)
    }

    #[inline(always)]
    pub unsafe fn as_native_handle(&self) -> *mut sys::ObjNativeHandle {
        self.as_obj().cast()
    }

    #[inline(always)]
    pub fn is_callable(&self) -> bool {
        self.is_function()
            || self.is_closure()
            || self.is_native()
            || self.is_ffi()
            || self.is_struct()
    }

    #[inline(always)]
    pub fn is_struct(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Struct_)
    }

    #[inline(always)]
    pub fn is_instance(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Instance)
    }

    #[inline(always)]
    pub fn is_error(&self) -> bool {
        self.is_obj() && self.obj_type() == Some(sys::ObjType::Error)
    }

    #[inline(always)]
    pub const fn to_bits(&self) -> u64 {
        self.raw
    }

    #[inline(always)]
    pub const unsafe fn from_bits(bits: u64) -> Self {
        Self {
            raw: bits,
            root: None,
            trusted_unowned: true,
        }
    }

    #[inline(always)]
    pub fn try_as_bool(&self) -> Option<bool> {
        if self.is_bool() {
            Some(self.as_bool())
        } else {
            None
        }
    }

    #[inline(always)]
    pub fn try_as_int(&self) -> Option<i32> {
        if self.is_int() {
            Some(self.as_int())
        } else {
            None
        }
    }

    #[inline(always)]
    pub fn try_as_number(&self) -> Option<f64> {
        if self.is_number() {
            Some(self.as_double())
        } else {
            None
        }
    }

    #[inline(always)]
    pub fn try_as_numeric(&self) -> Option<f64> {
        if self.is_int() {
            Some(self.as_int() as f64)
        } else if self.is_number() {
            Some(self.as_double())
        } else {
            None
        }
    }

    #[inline(always)]
    pub unsafe fn try_as_obj(&self) -> Option<*mut sys::Obj> {
        if self.is_obj() {
            Some(self.as_obj())
        } else {
            None
        }
    }

    pub fn try_as_string(&self) -> Option<&str> {
        if !self.is_string() {
            return None;
        }
        let obj = unsafe { self.as_obj() };
        unsafe {
            let s = &*(obj as *const sys::ObjString);
            if s.length < 0 {
                return None;
            }
            let chars = if s.is_rope {
                let owner = self.owner()?;
                sys::string_chars(owner.raw(), obj.cast())
            } else {
                s.chars
            };
            if chars.is_null() {
                return None;
            }
            let slice = std::slice::from_raw_parts(chars as *const u8, s.length as usize);
            std::str::from_utf8(slice).ok()
        }
    }

    pub fn expect_bool(&self) -> bool {
        self.try_as_bool()
            .unwrap_or_else(|| panic!("expected bool, got {:?}", self))
    }

    pub fn expect_int(&self) -> i32 {
        self.try_as_int()
            .unwrap_or_else(|| panic!("expected int, got {:?}", self))
    }

    pub fn expect_number(&self) -> f64 {
        self.try_as_number()
            .unwrap_or_else(|| panic!("expected number, got {:?}", self))
    }

    pub fn expect_numeric(&self) -> f64 {
        self.try_as_numeric()
            .unwrap_or_else(|| panic!("expected numeric, got {:?}", self))
    }

    pub fn to_raw(&self) -> sys::Value {
        self.raw
    }

    pub unsafe fn from_raw(raw: sys::Value) -> Self {
        Self {
            raw,
            root: None,
            trusted_unowned: true,
        }
    }

    pub fn to_owned_string(&self) -> Option<String> {
        if !self.is_string() {
            return None;
        }
        let owner = self.owner()?;
        let mut required = 0usize;
        unsafe {
            sys::vm_string_copy(
                owner.raw(),
                self.raw,
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
                owner.raw(),
                self.raw,
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

    pub(crate) fn from_vm_raw(raw: sys::Value, owner: Rc<VmInner>) -> Self {
        if Self::raw_is_obj(raw) {
            Self {
                raw,
                root: Some(RootHandle::new(owner, raw)),
                trusted_unowned: false,
            }
        } else {
            Self::primitive(raw)
        }
    }

    pub(crate) fn raw_ref(&self) -> sys::Value {
        self.raw
    }

    pub(crate) fn owner(&self) -> Option<&Rc<VmInner>> {
        self.root.as_ref().map(|root| root.owner())
    }

    pub(crate) fn belongs_to(&self, owner: &Rc<VmInner>) -> bool {
        !self.is_obj()
            || self
                .owner()
                .is_some_and(|value_owner| Rc::ptr_eq(value_owner, owner))
    }

    #[inline(always)]
    fn raw_is_obj(raw: u64) -> bool {
        (raw & (SIGN_BIT | QNAN)) == (SIGN_BIT | QNAN) && (raw & 0x7) == 0
    }
}

impl PartialEq for Value {
    #[inline(always)]
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw
    }
}

impl Eq for Value {}

impl std::fmt::Debug for Value {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        if self.is_null() {
            write!(f, "Value(null)")
        } else if self.is_bool() {
            write!(f, "Value(bool={})", self.as_bool())
        } else if self.is_int() {
            write!(f, "Value(int={})", self.as_int())
        } else if self.is_number() {
            write!(f, "Value(number={})", self.as_number())
        } else if self.is_obj() {
            match self.obj_type() {
                Some(sys::ObjType::String) => write!(f, "Value(string)"),
                Some(sys::ObjType::Array) => write!(f, "Value(array)"),
                Some(sys::ObjType::Dict) => write!(f, "Value(dict)"),
                Some(sys::ObjType::Function) => write!(f, "Value(function)"),
                Some(sys::ObjType::Closure) => write!(f, "Value(closure)"),
                Some(sys::ObjType::Struct_) => write!(f, "Value(struct)"),
                Some(sys::ObjType::Instance) => write!(f, "Value(instance)"),
                Some(sys::ObjType::Error) => write!(f, "Value(error)"),
                Some(sys::ObjType::VmTask) => write!(f, "Value(vm_task)"),
                Some(sys::ObjType::Coroutine) => write!(f, "Value(coroutine)"),
                Some(sys::ObjType::Ffi) => write!(f, "Value(ffi)"),
                Some(sys::ObjType::Native) => write!(f, "Value(native)"),
                Some(sys::ObjType::NativeHandle) => write!(f, "Value(native_handle)"),
                Some(sys::ObjType::Upvalue) => write!(f, "Value(upvalue)"),
                _ => write!(f, "Value(obj)"),
            }
        } else {
            write!(f, "Value(0x{:016x})", self.raw)
        }
    }
}

impl From<bool> for Value {
    #[inline(always)]
    fn from(b: bool) -> Self {
        Self::bool_val(b)
    }
}

impl From<i32> for Value {
    #[inline(always)]
    fn from(n: i32) -> Self {
        Self::int_val(n)
    }
}

impl From<f64> for Value {
    #[inline(always)]
    fn from(n: f64) -> Self {
        Self::number_val(n)
    }
}

impl From<()> for Value {
    #[inline(always)]
    fn from(_: ()) -> Self {
        Self::null()
    }
}

impl TryFrom<Value> for bool {
    type Error = Value;
    #[inline(always)]
    fn try_from(v: Value) -> Result<Self, Value> {
        if v.is_bool() {
            Ok(v.as_bool())
        } else {
            Err(v)
        }
    }
}

impl TryFrom<Value> for i32 {
    type Error = Value;
    #[inline(always)]
    fn try_from(v: Value) -> Result<Self, Value> {
        if v.is_int() {
            Ok(v.as_int())
        } else {
            Err(v)
        }
    }
}

impl TryFrom<Value> for f64 {
    type Error = Value;
    #[inline(always)]
    fn try_from(v: Value) -> Result<Self, Value> {
        if v.is_numeric() {
            Ok(v.as_number())
        } else {
            Err(v)
        }
    }
}
