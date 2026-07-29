use libc::{c_char, c_int, c_void, size_t};

use crate::types::*;

extern "C" {
    // VM Lifecycle
    pub fn vm_new() -> *mut VM;
    pub fn vm_delete(vm: *mut VM);
    pub fn vm_init(vm: *mut VM);
    pub fn vm_free(vm: *mut VM);
    pub fn vm_register_native(
        vm: *mut VM,
        name: *const c_char,
        function: NativeFn,
        arity: c_int,
        userdata: *mut c_void,
        userdata_finalizer: Option<unsafe extern "C" fn(data: *mut c_void)>,
    );
    pub fn vm_register_ffi(
        vm: *mut VM,
        name: *const c_char,
        c_func: *mut libc::c_void,
        arity: c_int,
    ) -> *mut ObjFFI;
    pub fn vm_new_native_handle(
        vm: *mut VM,
        type_name: *const c_char,
        data: *mut libc::c_void,
        finalizer: NativeHandleFinalizer,
    ) -> *mut ObjNativeHandle;
    pub fn vm_native_handle_set_method(
        vm: *mut VM,
        handle: *mut ObjNativeHandle,
        name: *const c_char,
        function: NativeFn,
        arity: c_int,
        userdata: *mut c_void,
        userdata_finalizer: Option<unsafe extern "C" fn(data: *mut c_void)>,
    ) -> bool;
    pub fn vm_native_handle_data(value: Value, type_name: *const c_char) -> *mut libc::c_void;
    pub fn vm_native_handle_value(handle: *mut ObjNativeHandle) -> Value;
    pub fn vm_set_global_value(vm: *mut VM, name: *const c_char, value: Value) -> bool;
    pub fn vm_get_global_value(vm: *mut VM, name: *const c_char, out: *mut Value) -> bool;
    pub fn vm_last_error(vm: *mut VM) -> *const c_char;
    pub fn vm_last_error_trace(vm: *mut VM) -> *const c_char;
    pub fn vm_last_error_line(vm: *mut VM) -> c_int;
    pub fn vm_last_error_file(vm: *mut VM) -> *const c_char;
    pub fn vm_last_error_function(vm: *mut VM) -> *const c_char;
    pub fn vm_clear_error(vm: *mut VM);
    pub fn vm_frame_count(vm: *mut VM) -> c_int;
    pub fn vm_stack_top(vm: *mut VM) -> c_int;
    pub fn vm_bytes_allocated(vm: *mut VM) -> size_t;
    pub fn vm_last_error_value(vm: *mut VM, out: *mut Value) -> bool;
    pub fn vm_interpret(vm: *mut VM, source: *const c_char) -> InterpretResult;
    pub fn vm_interpret_named(
        vm: *mut VM,
        source: *const c_char,
        name: *const c_char,
    ) -> InterpretResult;
    pub fn vm_run_function(vm: *mut VM, function: *mut ObjFunction) -> InterpretResult;
    pub fn vm_compile(vm: *mut VM, source: *const c_char) -> *mut ObjFunction;
    pub fn vm_compile_named(
        vm: *mut VM,
        source: *const c_char,
        name: *const c_char,
    ) -> *mut ObjFunction;
    pub fn vm_save_bytecode(vm: *mut VM, function: *mut ObjFunction, path: *const c_char) -> bool;
    pub fn vm_load_bytecode(vm: *mut VM, path: *const c_char) -> *mut ObjFunction;
    pub fn vm_push(vm: *mut VM, value: Value);
    pub fn vm_pop(vm: *mut VM) -> Value;
    pub fn vm_root_value(vm: *mut VM, value: Value) -> *mut VMRoot;
    pub fn vm_root_set(vm: *mut VM, root: *mut VMRoot, value: Value) -> bool;
    pub fn vm_root_get(root: *const VMRoot) -> Value;
    pub fn vm_unroot_value(vm: *mut VM, root: *mut VMRoot);
    pub fn mg_runtime_error_simple(vm: *mut VM, message: *const c_char);
    pub fn mg_get_native_userdata(vm: *mut VM) -> *mut c_void;
    pub fn vm_string_chars(value: Value) -> *const c_char;
    pub fn vm_string_length(value: Value) -> c_int;
    pub fn vm_string_chars_resolved(vm: *mut VM, value: Value) -> *const c_char;
    pub fn vm_string_copy(
        vm: *mut VM,
        value: Value,
        buffer: *mut c_char,
        capacity: size_t,
        required: *mut size_t,
    ) -> bool;

    // Compiler (AST -> Bytecode)
    pub fn compile_named(
        vm: *mut VM,
        ast: *mut ASTNode,
        name: *const c_char,
        name_len: c_int,
    ) -> *mut ObjFunction;

    // Object Allocation
    pub fn allocate_object(vm: *mut VM, size: size_t, obj_type: ObjType) -> *mut Obj;
    pub fn copy_string(vm: *mut VM, chars: *const c_char, length: c_int) -> *mut ObjString;
    pub fn take_string(vm: *mut VM, chars: *mut c_char, length: c_int) -> *mut ObjString;
    pub fn concat_strings(vm: *mut VM, a: *mut ObjString, b: *mut ObjString) -> *mut ObjString;
    pub fn string_chars(vm: *mut VM, string: *mut ObjString) -> *const c_char;
    pub fn string_char_at(string: *mut ObjString, index: c_int) -> c_char;
    pub fn new_function(vm: *mut VM) -> *mut ObjFunction;
    pub fn new_closure(vm: *mut VM, function: *mut ObjFunction) -> *mut ObjClosure;
    pub fn new_upvalue(vm: *mut VM, slot: *mut Value) -> *mut ObjUpvalue;
    pub fn new_native(
        vm: *mut VM,
        function: NativeFn,
        name: *mut ObjString,
        arity: c_int,
    ) -> *mut ObjNative;
    pub fn new_ffi(
        vm: *mut VM,
        c_func: *mut libc::c_void,
        name: *mut ObjString,
        arity: c_int,
    ) -> *mut ObjFFI;
    pub fn new_array(vm: *mut VM) -> *mut ObjArray;
    pub fn new_dict(vm: *mut VM) -> *mut ObjDict;
    pub fn new_struct(vm: *mut VM, name: *mut ObjString) -> *mut ObjStruct;
    pub fn new_instance(vm: *mut VM, klass: *mut ObjStruct) -> *mut ObjInstance;
    pub fn new_error(
        vm: *mut VM,
        kind: *mut ObjString,
        message: *mut ObjString,
        file: *mut ObjString,
        line: c_int,
        function: *mut ObjString,
        hint: *mut ObjString,
    ) -> *mut ObjError;
    pub fn new_vm_task(vm: *mut VM, path: *const c_char) -> *mut ObjVMTask;
    pub fn new_coroutine(vm: *mut VM, closure: *mut ObjClosure) -> *mut ObjCoroutine;
    pub fn vm_task_destroy(task: *mut ObjVMTask);

    // Array Operations
    pub fn array_push(vm: *mut VM, array: *mut ObjArray, value: Value);
    pub fn array_get(array: *mut ObjArray, index: c_int) -> Value;
    pub fn array_set(vm: *mut VM, array: *mut ObjArray, index: c_int, value: Value);

    // Dict Operations
    pub fn dict_get(dict: *mut ObjDict, key: *mut ObjString, value: *mut Value) -> bool;
    pub fn dict_set(vm: *mut VM, dict: *mut ObjDict, key: *mut ObjString, value: Value) -> bool;
    pub fn dict_delete(dict: *mut ObjDict, key: *mut ObjString) -> bool;
    pub fn dict_adjust_capacity(vm: *mut VM, dict: *mut ObjDict, capacity: c_int);

    // Table API
    pub fn table_init(table: *mut Table);
    pub fn table_free(table: *mut Table);
    pub fn table_get(table: *mut Table, key: *mut ObjString, value: *mut Value) -> bool;
    pub fn table_set(table: *mut Table, key: *mut ObjString, value: Value) -> bool;
    pub fn table_delete(table: *mut Table, key: *mut ObjString) -> bool;
    pub fn table_find_string(
        table: *mut Table,
        chars: *const c_char,
        length: c_int,
        hash: u32,
    ) -> *mut ObjString;

    // Chunk API
    pub fn chunk_init(chunk: *mut Chunk);
    pub fn chunk_free(chunk: *mut Chunk);
    pub fn chunk_write(chunk: *mut Chunk, inst: Instruction, line: c_int);
    pub fn chunk_add_constant(chunk: *mut Chunk, value: Value) -> c_int;

    // GC
    pub fn gc_collect(vm: *mut VM);
    pub fn remembered_set_add(vm: *mut VM, old_obj: *mut Obj);
    pub fn gc_mark_value(vm: *mut VM, value: Value);
    pub fn gc_mark_object(vm: *mut VM, object: *mut Obj);

    // Value Helpers
    pub fn values_equal(a: Value, b: Value) -> bool;
    pub fn print_value(value: Value);

    // Lexer
    pub fn scanner_init(scanner: *mut Scanner, source: *const c_char);
    pub fn scan_token(scanner: *mut Scanner) -> Token;

    // Parser
    pub fn parse(source: *const c_char, had_error: *mut bool) -> *mut ASTNode;

    // Optimizer
    pub fn optimize_ast(node: *mut ASTNode);

    // Compiler (full pipeline)
    pub fn compile(vm: *mut VM, ast: *mut ASTNode) -> *mut ObjFunction;

    // AST Helpers
    pub fn ast_alloc(node_type: NodeType, line: c_int) -> *mut ASTNode;
    pub fn ast_free(node: *mut ASTNode);
    pub fn type_ref_alloc(kind: MgTypeKind, line: c_int) -> *mut MgTypeRef;
    pub fn type_ref_free(type_ref: *mut MgTypeRef);
    pub fn node_list_init(list: *mut NodeList);
    pub fn node_list_write(list: *mut NodeList, node: *mut ASTNode);
    pub fn node_list_free(list: *mut NodeList);
    pub fn kv_list_init(list: *mut KVList);
    pub fn kv_list_write(list: *mut KVList, key: *mut ASTNode, value: *mut ASTNode);
    pub fn kv_list_free(list: *mut KVList);

    // Debug
    pub fn disassemble_chunk(chunk: *mut Chunk, name: *const c_char);
    pub fn disassemble_instruction(chunk: *mut Chunk, offset: c_int);

    // Static Checker
    pub fn mg_ast_has_nocheck(ast: *mut ASTNode) -> bool;
    pub fn mg_ast_has_strict(ast: *mut ASTNode) -> bool;
    pub fn mg_typecheck_ast(ast: *mut ASTNode, force_check: bool) -> bool;
}
