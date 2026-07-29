use magnesium_sys as sys;
use std::ffi::CString;

pub struct AstNode {
    raw: *mut sys::ASTNode,
    // Parsed AST tokens point into the original source allocation.
    _source: Option<CString>,
}

impl AstNode {
    pub unsafe fn from_raw(raw: *mut sys::ASTNode) -> Option<Self> {
        if raw.is_null() {
            None
        } else {
            Some(AstNode { raw, _source: None })
        }
    }

    pub fn raw(&self) -> *mut sys::ASTNode {
        self.raw
    }

    pub fn type_(&self) -> sys::NodeType {
        unsafe { (*self.raw).type_ }
    }

    pub fn line(&self) -> libc::c_int {
        unsafe { (*self.raw).line }
    }
}

pub fn parse(source: &str) -> Result<AstNode, bool> {
    let c_source = match CString::new(source) {
        Ok(source) => source,
        Err(_) => return Err(true),
    };
    let mut had_error: bool = false;
    let node = unsafe { sys::parse(c_source.as_ptr(), &mut had_error) };
    if had_error || node.is_null() {
        if !node.is_null() {
            unsafe { sys::ast_free(node) };
        }
        Err(true)
    } else {
        Ok(AstNode {
            raw: node,
            _source: Some(c_source),
        })
    }
}

pub fn optimize_ast(node: &mut AstNode) {
    unsafe { sys::optimize_ast(node.raw) };
}

impl Drop for AstNode {
    fn drop(&mut self) {
        if !self.raw.is_null() {
            unsafe { sys::ast_free(self.raw) };
        }
    }
}

pub fn ast_alloc(node_type: sys::NodeType, line: libc::c_int) -> Option<AstNode> {
    let raw = unsafe { sys::ast_alloc(node_type, line) };
    if raw.is_null() {
        None
    } else {
        Some(AstNode { raw, _source: None })
    }
}
