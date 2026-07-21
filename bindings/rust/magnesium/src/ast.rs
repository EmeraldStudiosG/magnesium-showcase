use magnesium_sys as sys;

pub struct AstNode(*mut sys::ASTNode);

impl AstNode {
    pub unsafe fn from_raw(raw: *mut sys::ASTNode) -> Self {
        AstNode(raw)
    }

    pub fn raw(&self) -> *mut sys::ASTNode {
        self.0
    }

    pub fn type_(&self) -> sys::NodeType {
        unsafe { (*self.0).type_ }
    }

    pub fn line(&self) -> libc::c_int {
        unsafe { (*self.0).line }
    }
}

pub fn parse(source: &str) -> Result<AstNode, bool> {
    let c_source = std::ffi::CString::new(source).expect("source contains null byte");
    let mut had_error: bool = false;
    let node = unsafe { sys::parse(c_source.as_ptr(), &mut had_error) };
    if had_error || node.is_null() {
        Err(had_error)
    } else {
        Ok(AstNode(node))
    }
}

pub fn optimize_ast(node: &AstNode) {
    unsafe { sys::optimize_ast(node.0) };
}

impl Drop for AstNode {
    fn drop(&mut self) {
        if !self.0.is_null() {
            unsafe { sys::ast_free(self.0) };
        }
    }
}

pub fn ast_alloc(node_type: sys::NodeType, line: libc::c_int) -> AstNode {
    let raw = unsafe { sys::ast_alloc(node_type, line) };
    AstNode(raw)
}
