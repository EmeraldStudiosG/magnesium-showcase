use libc::c_int;

pub const MG_VERSION_MAJOR: c_int = 1;
pub const MG_VERSION_MINOR: c_int = 1;
pub const MG_VERSION_PATCH: c_int = 1;

pub const MAX_REGISTERS: c_int = 256;
pub const MAX_CONSTANTS: c_int = 65536;
pub const MAX_LOCALS: c_int = 256;
pub const MAX_UPVALUES: c_int = 256;
pub const MAX_CALL_FRAMES: c_int = 256;
pub const STACK_INIT_SIZE: c_int = 1024;
pub const GC_HEAP_GROW_FACTOR: usize = 2;

pub const QNAN: u64 = 0x7ffc000000000000;
pub const SIGN_BIT: u64 = 0x8000000000000000;

pub const TAG_NULL: u64 = 1;
pub const TAG_FALSE: u64 = 2;
pub const TAG_TRUE: u64 = 3;
pub const TAG_INT: u64 = 4;

pub const GLOBAL_IC_SIZE: usize = 128;
pub const FIELD_IC_SIZE: usize = 128;
pub const METHOD_IC_SIZE: usize = 128;
pub const DICT_IC_SIZE: usize = 256;
