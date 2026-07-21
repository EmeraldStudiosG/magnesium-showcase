use magnesium_sys as sys;

pub struct Scanner {
    raw: sys::Scanner,
}

impl Scanner {
    pub fn new(source: &str) -> Self {
        let c_source = std::ffi::CString::new(source).expect("source contains null byte");
        let mut scanner: sys::Scanner = unsafe { std::mem::zeroed() };
        unsafe { sys::scanner_init(&mut scanner, c_source.as_ptr()) };
        Scanner { raw: scanner }
    }

    pub fn scan_token(&mut self) -> Token {
        let raw = unsafe { sys::scan_token(&mut self.raw) };
        Token { raw }
    }

    pub fn raw(&self) -> &sys::Scanner {
        &self.raw
    }

    pub fn raw_mut(&mut self) -> &mut sys::Scanner {
        &mut self.raw
    }
}

pub struct Token {
    raw: sys::Token,
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

    pub fn text(&self) -> &str {
        if self.raw.start.is_null() || self.raw.length == 0 {
            return "";
        }
        unsafe {
            let slice =
                std::slice::from_raw_parts(self.raw.start as *const u8, self.raw.length as usize);
            std::str::from_utf8_unchecked(slice)
        }
    }
}
