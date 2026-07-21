#![allow(clippy::missing_safety_doc)]
pub mod array;
pub mod ast;
pub mod chunk;
pub mod dict;
pub mod error;
pub mod gc;
pub mod lexer;
pub mod native;
pub mod objects;
pub mod string;
pub mod value;
pub mod vm;

pub use array::MgArray;
pub use ast::AstNode;
pub use chunk::MgChunk;
pub use dict::MgDict;
pub use error::{HostError, InterpretError, InterpretResult, MgError};
pub use gc::GcRoot;
pub use lexer::{Scanner, Token};
pub use native::NativeContext;
pub use objects::{MgClosure, MgCoroutine, MgFfi, MgFunction, MgInstance, MgStruct};
pub use string::MgString;
pub use value::Value;
pub use vm::{Ffi1, Ffi2, FunctionHandle, Vm};
