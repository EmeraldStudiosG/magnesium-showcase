use magnesium_sys as sys;
use std::ffi::CString;
use std::rc::Rc;

pub struct Scanner {
    raw: sys::Scanner,
    // The C scanner stores pointers into this allocation.
    source: Rc<CString>,
}

impl Scanner {
    pub fn new(source: &str) -> Result<Self, std::ffi::NulError> {
        let source = Rc::new(CString::new(source)?);
        let mut scanner: sys::Scanner = unsafe { std::mem::zeroed() };
        unsafe { sys::scanner_init(&mut scanner, source.as_ptr()) };
        Ok(Scanner {
            raw: scanner,
            source,
        })
    }

    pub fn scan_token(&mut self) -> Token {
        let raw = unsafe { sys::scan_token(&mut self.raw) };
        Token {
            raw,
            _source: Rc::clone(&self.source),
        }
    }

    pub fn raw(&self) -> &sys::Scanner {
        &self.raw
    }

    pub unsafe fn raw_mut(&mut self) -> &mut sys::Scanner {
        &mut self.raw
    }
}

pub struct Token {
    raw: sys::Token,
    // Tokens may outlive their scanner, so retain the source allocation.
    _source: Rc<CString>,
}

impl Token {
    pub fn raw(&self) -> &sys::Token {
        &self.raw
    }

    pub fn type_(&self) -> sys::MgTokenType {
        self.raw.type_
    }

    pub fn line(&self) -> libc::c_int {
        self.raw.line
    }

    pub fn text(&self) -> Result<&str, std::str::Utf8Error> {
        if self.raw.start.is_null() || self.raw.length == 0 {
            return Ok("");
        }
        debug_assert!(self.raw.length >= 0);
        unsafe {
            let slice =
                std::slice::from_raw_parts(self.raw.start as *const u8, self.raw.length as usize);
            std::str::from_utf8(slice)
        }
    }
}
