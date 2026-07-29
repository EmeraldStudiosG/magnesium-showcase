#![allow(non_camel_case_types, non_snake_case, dead_code)]

use libc::{c_char, c_int, c_void, size_t};

// The C header guards pthread types behind `#ifndef _WIN32` and uses
// `void *` for thread/lock fields on Windows. Mirror that here so the
// crate builds on every supported target. The Rust wrapper never
// touches these fields directly.
#[cfg(not(target_os = "windows"))]
type thread_handle = libc::pthread_t;
#[cfg(not(target_os = "windows"))]
type thread_lock = libc::pthread_mutex_t;
#[cfg(target_os = "windows")]
type thread_handle = *mut c_void;
#[cfg(target_os = "windows")]
type thread_lock = *mut c_void;

pub type Value = u64;
pub type Instruction = u32;

// Token Types
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MgTokenType {
    LeftParen = 0,
    RightParen,
    LeftBrace,
    RightBrace,
    LeftBracket,
    RightBracket,
    Comma,
    Dot,
    Minus,
    Plus,
    Semicolon,
    Slash,
    Star,
    Colon,
    At,
    Percent,
    Question,
    Bang,
    BangEqual,
    Equal,
    EqualEqual,
    Greater,
    GreaterEqual,
    Less,
    LessEqual,
    DotDot,
    DotDotEqual,
    PlusEqual,
    MinusEqual,
    StarEqual,
    SlashEqual,
    PercentEqual,
    Identifier,
    String,
    InterpString,
    Number,
    And,
    As,
    Break,
    Catch,
    Const,
    Continue,
    Defer,
    Else,
    Elseif,
    End,
    Enum,
    Export,
    False,
    Fn,
    For,
    If,
    Import,
    In,
    Let,
    Loop,
    Not,
    Null,
    Or,
    Return,
    Struct,
    Then,
    True,
    Try,
    Ampersand,
    Error,
    Eof,
    Count,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct Token {
    pub type_: MgTokenType,
    pub start: *const c_char,
    pub length: c_int,
    pub line: c_int,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct Scanner {
    pub start: *const c_char,
    pub current: *const c_char,
    pub line: c_int,
}

// Static Type Metadata
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MgTypeKind {
    Name = 0,
    Array,
    Dict,
    Function,
    Shape,
}

#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MgDirectiveKind {
    Strict = 0,
    Nocheck,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MgTypeField {
    pub name: Token,
    pub type_: *mut MgTypeRef,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MgTypeName {
    pub name: Token,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MgTypeArray {
    pub element: *mut MgTypeRef,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MgTypeDict {
    pub key: *mut MgTypeRef,
    pub value: *mut MgTypeRef,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MgTypeFunction {
    pub params: *mut *mut MgTypeRef,
    pub param_count: c_int,
    pub return_type: *mut MgTypeRef,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct MgTypeShape {
    pub fields: *mut MgTypeField,
    pub field_count: c_int,
    pub field_capacity: c_int,
}

#[repr(C)]
pub union MgTypeRefAs {
    pub name: MgTypeName,
    pub array: MgTypeArray,
    pub dict: MgTypeDict,
    pub function: MgTypeFunction,
    pub shape: MgTypeShape,
}

#[repr(C)]
pub struct MgTypeRef {
    pub kind: MgTypeKind,
    pub line: c_int,
    pub as_: MgTypeRefAs,
}

// Object Type Enum
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ObjType {
    String = 0,
    Array,
    Dict,
    Function,
    Closure,
    Upvalue,
    Native,
    Ffi,
    NativeHandle,
    Struct_,
    Instance,
    Error,
    VmTask,
    Coroutine,
}

// Base Object
#[repr(C)]
#[derive(Debug)]
pub struct Obj {
    pub type_: ObjType,
    pub is_marked: bool,
    pub is_old: bool,
    pub size_class: u8,
    pub gc_age: u8,
    pub next: *mut Obj,
    pub alloc_size: size_t,
}

// String
#[repr(C)]
#[derive(Debug)]
pub struct ObjString {
    pub obj: Obj,
    pub length: c_int,
    pub capacity: c_int,
    pub hash: u32,
    pub is_rope: bool,
    pub chars: *mut c_char,
    pub left: *mut ObjString,
    pub right: *mut ObjString,
}

// Array
#[repr(C)]
#[derive(Debug)]
pub struct ObjArray {
    pub obj: Obj,
    pub count: c_int,
    pub capacity: c_int,
    pub items: *mut Value,
}

// Dict
#[repr(C)]
#[derive(Debug)]
pub struct DictEntry {
    pub key: *mut ObjString,
    pub hash: u32,
    pub value: Value,
}

#[repr(C)]
#[derive(Debug)]
pub struct ObjDict {
    pub obj: Obj,
    pub count: c_int,
    pub capacity: c_int,
    pub version: u32,
    pub mono_cache_key: *mut ObjString,
    pub mono_cache_entry: *mut DictEntry,
    pub mono_cache_value: Value,
    pub indices: *mut i32,
    pub entry_count: c_int,
    pub entry_capacity: c_int,
    pub entries: *mut DictEntry,
}

// Table
#[repr(C)]
#[derive(Debug)]
pub struct TableEntry {
    pub key: *mut ObjString,
    pub hash: u32,
    pub value: Value,
}

#[repr(C)]
#[derive(Debug)]
pub struct Table {
    pub count: c_int,
    pub capacity: c_int,
    pub entries: *mut TableEntry,
}

// Chunk
#[repr(C)]
#[derive(Debug)]
pub struct Chunk {
    pub count: c_int,
    pub capacity: c_int,
    pub code: *mut Instruction,
    pub lines: *mut c_int,
    pub const_count: c_int,
    pub const_capacity: c_int,
    pub constants: *mut Value,
}

// Function
#[repr(C)]
#[derive(Debug)]
pub struct ObjFunction {
    pub obj: Obj,
    pub arity: c_int,
    pub upvalue_count: c_int,
    pub reg_count: c_int,
    pub chunk: Chunk,
    pub name: *mut ObjString,
    pub source_name: *mut ObjString,
}

// Upvalue
#[repr(C)]
#[derive(Debug)]
pub struct ObjUpvalue {
    pub obj: Obj,
    pub location: *mut Value,
    pub closed: Value,
    pub next: *mut ObjUpvalue,
}

// Closure
// upvalues[] is a flexible array member: access via pointer arithmetic.

#[repr(C)]
#[derive(Debug)]
pub struct ObjClosure {
    pub obj: Obj,
    pub function: *mut ObjFunction,
    pub upvalue_count: c_int,
}

impl ObjClosure {
    #[inline(always)]
    pub unsafe fn upvalues_ptr(&self) -> *const *mut ObjUpvalue {
        (self as *const Self).add(1).cast()
    }

    #[inline(always)]
    pub unsafe fn upvalue(&self, index: usize) -> *mut ObjUpvalue {
        *self.upvalues_ptr().add(index)
    }
}

// CallFrame
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct CallFrame {
    pub closure: *mut ObjClosure,
    pub ip: *mut Instruction,
    pub slots: *mut Value,
    pub call_dest: c_int,
    pub expected_returns: c_int,
}

// SavedVMContext
#[repr(C)]
#[derive(Debug)]
pub struct SavedVMContext {
    pub stack: *mut Value,
    pub stack_capacity: c_int,
    pub stack_size: c_int,
    pub stack_top: c_int,
    pub frames: [CallFrame; 256],
    pub frame_count: c_int,
    pub open_upvalues: *mut ObjUpvalue,
    pub defer_items: *mut *mut ObjClosure,
    pub defer_count: c_int,
    pub defer_capacity: c_int,
    pub current_coroutine: *mut ObjCoroutine,
    pub yield_requested: bool,
    pub yield_values: [Value; 256],
    pub yield_count: c_int,
    pub native_return_values: [Value; 256],
    pub native_return_count: c_int,
    pub next: *mut SavedVMContext,
}

// Native Function
pub type NativeFn =
    Option<unsafe extern "C" fn(vm: *mut VM, arg_count: c_int, args: *mut Value) -> Value>;
pub type NativeHandleFinalizer = Option<unsafe extern "C" fn(data: *mut c_void)>;

#[repr(C)]
#[derive(Debug)]
pub struct ObjNative {
    pub obj: Obj,
    pub function: NativeFn,
    pub name: *mut ObjString,
    pub arity: c_int,
    pub copy_args: bool,
    pub userdata: *mut c_void,
    pub userdata_finalizer: Option<unsafe extern "C" fn(data: *mut c_void)>,
}

// FFI
#[repr(C)]
#[derive(Debug)]
pub struct ObjFFI {
    pub obj: Obj,
    pub c_function: *mut c_void,
    pub name: *mut ObjString,
    pub arity: c_int,
}

#[repr(C)]
#[derive(Debug)]
pub struct ObjNativeHandle {
    pub obj: Obj,
    pub data: *mut c_void,
    pub type_name: *mut ObjString,
    pub methods: *mut ObjDict,
    pub finalizer: NativeHandleFinalizer,
}

// Struct Definition & Instance
#[repr(C)]
#[derive(Debug)]
pub struct ObjStruct {
    pub obj: Obj,
    pub name: *mut ObjString,
    pub methods: *mut ObjDict,
    pub field_names: *mut *mut ObjString,
    pub field_count: c_int,
    pub field_index: Table,
}

// fields[] is a flexible array member: access via pointer arithmetic.
#[repr(C)]
#[derive(Debug)]
pub struct ObjInstance {
    pub obj: Obj,
    pub klass: *mut ObjStruct,
}

impl ObjInstance {
    #[inline(always)]
    pub unsafe fn fields_ptr(&self) -> *const Value {
        (self as *const Self).add(1).cast()
    }

    #[inline(always)]
    pub unsafe fn field(&self, index: usize) -> Value {
        *self.fields_ptr().add(index)
    }

    #[inline(always)]
    pub unsafe fn set_field(&mut self, index: usize, value: Value) {
        *self.fields_ptr().add(index).cast_mut() = value;
    }
}

// Error
#[repr(C)]
#[derive(Debug)]
pub struct ObjError {
    pub obj: Obj,
    pub kind: *mut ObjString,
    pub message: *mut ObjString,
    pub file: *mut ObjString,
    pub function_: *mut ObjString,
    pub hint: *mut ObjString,
    pub line: c_int,
}

// VM Task
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum VMTaskState {
    Running = 0,
    Done,
}

#[repr(C)]
pub struct ObjVMTask {
    pub obj: Obj,
    pub path: *mut c_char,
    pub state: VMTaskState,
    pub result: c_int,
    pub started: bool,
    pub joined: bool,
    pub cancel_requested: bool,
    pub child_vm: *mut VM,
    pub error_kind: [c_char; 64],
    pub error_message: [c_char; 512],
    pub error_hint: [c_char; 512],
    pub thread: thread_handle,
    pub lock: thread_lock,
}

// Coroutine
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CoroutineState {
    New = 0,
    Running,
    Suspended,
    Dead,
}

#[repr(C)]
pub struct ObjCoroutine {
    pub obj: Obj,
    pub closure: *mut ObjClosure,
    pub state: CoroutineState,
    pub stack: *mut Value,
    pub stack_size: c_int,
    pub stack_capacity: c_int,
    pub stack_top: c_int,
    pub frames: [CallFrame; 256],
    pub frame_count: c_int,
    pub open_upvalues: *mut ObjUpvalue,
    pub defer_items: *mut *mut ObjClosure,
    pub defer_count: c_int,
    pub defer_capacity: c_int,
    pub yield_values: [Value; 256],
    pub yield_count: c_int,
}

// Inline Cache Structs
#[repr(C)]
#[derive(Debug)]
pub struct GlobalICEntry {
    pub name: *mut ObjString,
    pub value: Value,
}

#[repr(C)]
#[derive(Debug)]
pub struct FieldICEntry {
    pub klass: *mut ObjStruct,
    pub name: *mut ObjString,
    pub index: c_int,
}

#[repr(C)]
#[derive(Debug)]
pub struct MethodICEntry {
    pub klass: *mut ObjStruct,
    pub name: *mut ObjString,
    pub method: Value,
}

#[repr(C)]
#[derive(Debug)]
pub struct DictICEntry {
    pub dict: *mut ObjDict,
    pub key: *mut ObjString,
    pub version: u32,
    pub entry: *mut DictEntry,
    pub index: c_int,
}

// VM dynamic array (used for defer_stack, task_queue, vm_tasks)
#[repr(C)]
#[derive(Debug)]
pub struct ClosureArray {
    pub items: *mut *mut ObjClosure,
    pub count: c_int,
    pub capacity: c_int,
}

#[repr(C)]
#[derive(Debug)]
pub struct VmTaskArray {
    pub items: *mut *mut ObjVMTask,
    pub count: c_int,
    pub capacity: c_int,
}

#[repr(C)]
#[derive(Debug)]
pub struct VMRoot {
    pub value: Value,
    pub next: *mut VMRoot,
}

// VM is intentionally opaque. Its layout contains C atomics and private
// allocator state and is not part of the stable pointer-based C API.
#[repr(C)]
pub struct VM {
    _private: [u8; 0],
}

// InterpretResult
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InterpretResult {
    Ok = 0,
    CompileError,
    RuntimeError,
    Yield,
}

// OpCode
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OpCode {
    Loadk = 0,
    Loadbool,
    Loadnil,
    Move,
    Getglobal,
    Setglobal,
    Getupval,
    Setupval,
    Add,
    Sub,
    Mul,
    Div,
    Mod,
    Neg,
    Addk,
    Subk,
    Mulk,
    Divk,
    Modk,
    Addi,
    Subi,
    Muli,
    Divi,
    Modi,
    Eq,
    Neq,
    Lt,
    Le,
    Eqi,
    Neqi,
    Lti,
    Lei,
    Gti,
    Gei,
    EqiTest,
    NeqiTest,
    LtiTest,
    LeiTest,
    GtiTest,
    GeiTest,
    ModiEqiTest,
    ModiNeqiTest,
    Not,
    Test,
    Testset,
    Testjmp,
    Testerrjmp,
    Concat,
    Tostring,
    Len,
    Jmp,
    Loop,
    Forprep,
    ForprepNum,
    Forloop,
    ForloopInc,
    ForloopNum,
    ForloopIncNum,
    ForaddlocalFieldProp,
    ForaddlocalFieldPropInc,
    ForaddglobalFieldProp,
    ForaddglobalFieldPropInc,
    ForModiAccum,
    ForModiAccumInc,
    ForField2Accum,
    ForField2AccumInc,
    ArrayMarkFalseStride,
    Closure,
    Call,
    Callg,
    Mcall,
    Callr,
    Callself,
    Addup,
    Addlocal,
    Sublocal,
    AddlocalFieldProp,
    SublocalFieldProp,
    AddlocalLen,
    SublocalLen,
    AddlocalMuli,
    SublocalMuli,
    AddlocalMulk,
    SublocalMulk,
    AddlocalDivi,
    SublocalDivi,
    AddlocalModi,
    SublocalModi,
    Return,
    Newarray,
    Setarray,
    ArrayPush,
    Getindex,
    GetindexTry,
    Setindex,
    Newdict,
    Getfield,
    GetfieldTry,
    GetfieldProp,
    Setfield,
    GetfieldIdx,
    SetfieldIdx,
    Addsub,
    Subadd,
    Muladd,
    Mulsub,
    AddGtTest,
    AddGeTest,
    SubGtTest,
    SubGeTest,
    MullocalAdd,
    MullocalSub,
    IterPrep,
    IterNext,
    Aux,
    CloseUpval,
    Defer,
    Newstruct,
    Mcallfield,
    Mcallfield0,
    AddiLoop,
    Count,
}

// AST Node Types
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum NodeType {
    Number = 0,
    String,
    InterpString,
    Bool,
    Null,
    Identifier,
    Unary,
    Binary,
    Logical,
    Assign,
    Call,
    Index,
    Try,
    FieldGet,
    FieldSet,
    ArrayLiteral,
    DictLiteral,
    StructLiteral,
    ExpressionStmt,
    Let,
    Const,
    Block,
    If,
    Loop,
    ForRange,
    ForIn,
    Break,
    Continue,
    Return,
    TryBlock,
    TryCatch,
    Defer,
    FnDecl,
    StructDecl,
    EnumDecl,
    Import,
    Export,
    Directive,
    TypeAlias,
    ExternDecl,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct NodeList {
    pub nodes: *mut *mut ASTNode,
    pub count: c_int,
    pub capacity: c_int,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct KVPair {
    pub key: *mut ASTNode,
    pub value: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct KVList {
    pub pairs: *mut KVPair,
    pub count: c_int,
    pub capacity: c_int,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct Param {
    pub name: Token,
    pub type_: *mut MgTypeRef,
}

// AST Node union members
#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTNumber {
    pub value: f64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTString {
    pub value: *mut c_char,
    pub length: c_int,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTBoolean {
    pub value: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTIdentifier {
    pub name: Token,
    pub is_global: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTUnary {
    pub op: MgTokenType,
    pub operand: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTBinary {
    pub op: MgTokenType,
    pub left: *mut ASTNode,
    pub right: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTLogical {
    pub op: MgTokenType,
    pub left: *mut ASTNode,
    pub right: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTAssign {
    pub target: *mut ASTNode,
    pub value: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTCall {
    pub callee: *mut ASTNode,
    pub args: NodeList,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTIndex {
    pub object: *mut ASTNode,
    pub index: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTTry {
    pub expr: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTFieldGet {
    pub object: *mut ASTNode,
    pub name: Token,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTFieldSet {
    pub object: *mut ASTNode,
    pub name: Token,
    pub value: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTArrayLiteral {
    pub items: NodeList,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTDictLiteral {
    pub entries: KVList,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTStructLiteral {
    pub name: Token,
    pub fields: KVList,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTExprStmt {
    pub expr: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTVarDecl {
    pub name: Token,
    pub extra_names: *mut Token,
    pub type_annotation: *mut MgTypeRef,
    pub extra_type_annotations: *mut *mut MgTypeRef,
    pub name_count: c_int,
    pub is_global: bool,
    pub is_const: bool,
    pub initializer: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTBlock {
    pub stmts: NodeList,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTIf {
    pub condition: *mut ASTNode,
    pub then_branch: *mut ASTNode,
    pub else_branch: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTLoop {
    pub body: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTForRange {
    pub var_: Token,
    pub start: *mut ASTNode,
    pub end: *mut ASTNode,
    pub inclusive: bool,
    pub body: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTForIn {
    pub var_: Token,
    pub var2: Token,
    pub has_var2: bool,
    pub iterable: *mut ASTNode,
    pub body: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTReturn {
    pub values: NodeList,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTTryBlock {
    pub body: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTTryCatch {
    pub body: *mut ASTNode,
    pub err_name: Token,
    pub catch_body: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTDefer {
    pub call: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTFnDecl {
    pub name: Token,
    pub method_struct: Token,
    pub is_method: bool,
    pub params: *mut Param,
    pub param_count: c_int,
    pub return_type: *mut MgTypeRef,
    pub body: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTStructDecl {
    pub name: Token,
    pub fields: *mut Param,
    pub field_count: c_int,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTEnumDecl {
    pub name: Token,
    pub variants: *mut Token,
    pub variant_count: c_int,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTImport {
    pub path: Token,
    pub alias: Token,
    pub has_alias: bool,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTExport {
    pub declaration: *mut ASTNode,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTDirective {
    pub kind: MgDirectiveKind,
    pub name: Token,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTTypeAlias {
    pub name: Token,
    pub type_: *mut MgTypeRef,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct ASTExternDecl {
    pub is_function: bool,
    pub name: Token,
    pub params: *mut Param,
    pub param_count: c_int,
    pub type_: *mut MgTypeRef,
}

// AST Node
// The `as` field is a C union. We use #[repr(C)] union in Rust.
// Access requires unsafe since only one variant is active at a time.

#[repr(C)]
pub union ASTNodeAs {
    pub number: ASTNumber,
    pub string: ASTString,
    pub boolean: ASTBoolean,
    pub identifier: ASTIdentifier,
    pub unary: ASTUnary,
    pub binary: ASTBinary,
    pub logical: ASTLogical,
    pub assign: ASTAssign,
    pub call: ASTCall,
    pub index_expr: ASTIndex,
    pub try_expr: ASTTry,
    pub field_get: ASTFieldGet,
    pub field_set: ASTFieldSet,
    pub array_literal: ASTArrayLiteral,
    pub dict_literal: ASTDictLiteral,
    pub struct_literal: ASTStructLiteral,
    pub expr_stmt: ASTExprStmt,
    pub var_decl: ASTVarDecl,
    pub block: ASTBlock,
    pub if_stmt: ASTIf,
    pub loop_stmt: ASTLoop,
    pub for_range: ASTForRange,
    pub for_in: ASTForIn,
    pub return_stmt: ASTReturn,
    pub try_block: ASTTryBlock,
    pub try_catch: ASTTryCatch,
    pub defer_stmt: ASTDefer,
    pub fn_decl: ASTFnDecl,
    pub struct_decl: ASTStructDecl,
    pub enum_decl: ASTEnumDecl,
    pub import_stmt: ASTImport,
    pub export_stmt: ASTExport,
    pub directive: ASTDirective,
    pub type_alias: ASTTypeAlias,
    pub extern_decl: ASTExternDecl,
}

#[repr(C)]
pub struct ASTNode {
    pub type_: NodeType,
    pub line: c_int,
    pub as_: ASTNodeAs,
}
