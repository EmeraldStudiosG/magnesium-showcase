#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "magnesium.h"

#define TEST_NURSERY_LIMIT ((size_t)8 * 1024 * 1024)

static void arm_minor_after_parent_alloc(VM *vm, size_t parent_size) {
    assert(vm != NULL);
    assert(parent_size < TEST_NURSERY_LIMIT);
    assert(vm->young_bytes <= TEST_NURSERY_LIMIT - parent_size);

    /*
     * allocate_object() collects only when the next allocation would exceed
     * the nursery limit.  Put the parent exactly on that boundary so its child
     * allocation promotes the rooted parent; the collection then restores the
     * real young-generation accounting.
     */
    vm->next_gc = SIZE_MAX;
    vm->young_bytes = TEST_NURSERY_LIMIT - parent_size;
}

static Value grow_stack(VM *vm, int arg_count, Value *args) {
    assert(arg_count == 1);
    uintptr_t args_address = (uintptr_t)args;
    uintptr_t stack_begin = (uintptr_t)vm->stack;
    uintptr_t stack_end =
        stack_begin + sizeof(Value) * (size_t)vm->stack_capacity;
    assert(args_address < stack_begin || args_address >= stack_end);

    const int pushes = vm->stack_capacity + STACK_INIT_SIZE;
    for (int i = 0; i < pushes; i++) vm_push(vm, NULL_VAL);
    for (int i = 0; i < pushes; i++) (void)vm_pop(vm);
    gc_collect(vm);
    return args[0];
}

static Value assert_compact_stack(VM *vm, int arg_count, Value *args) {
    (void)arg_count;
    (void)args;
    int expected_top = 0;
    for (int i = 0; i < vm->frame_count; i++) {
        CallFrame *frame = &vm->frames[i];
        int reg_count = frame->closure->function->reg_count;
        if (reg_count <= 0) reg_count = MAX_REGISTERS;
        int frame_top = (int)(frame->slots - vm->stack) + reg_count;
        if (frame_top > expected_top) expected_top = frame_top;
    }
    assert(vm->stack_top == expected_top);
    assert(vm->stack_size == expected_top);
    return TRUE_VAL;
}

static void test_gc_barriers_and_host_roots(void) {
    VM *vm = vm_new();
    assert(vm != NULL);

    ObjArray *parent = new_array(vm);
    array_push(vm, parent, NULL_VAL);
    VMRoot *root = vm_root_value(vm, OBJ_VAL(parent));
    assert(root != NULL);

    gc_minor_collect(vm);
    assert(((Obj *)parent)->is_old);

    ObjString *first = copy_string(vm, "first", 5);
    array_set(vm, parent, 0, OBJ_VAL(first));
    gc_minor_collect(vm);

    ObjString *second = copy_string(vm, "second", 6);
    array_set(vm, parent, 0, OBJ_VAL(second));
    gc_minor_collect(vm);
    gc_minor_collect(vm);

    Value retained = array_get(parent, 0);
    assert(IS_STRING(retained));
    assert(AS_STRING(retained)->length == 6);
    assert(memcmp(string_chars(vm, AS_STRING(retained)), "second", 6) == 0);

    assert(vm_root_get(root) == OBJ_VAL(parent));
    assert(vm_root_set(vm, root, retained));
    assert(vm_root_get(root) == retained);

    ObjString *grace = copy_string(vm, "newborn-intern", 14);
    gc_minor_collect(vm);
    vm_push(vm, OBJ_VAL(grace));
    assert(copy_string(vm, "newborn-intern", 14) == grace);
    vm_pop(vm);

    vm_unroot_value(vm, root);
    vm_delete(vm);
}

static void test_dict_mono_cache_uses_owned_key(void) {
    VM *vm = vm_new();
    assert(vm != NULL);

    ObjDict *dict = new_dict(vm);
    VMRoot *dict_root = vm_root_value(vm, OBJ_VAL(dict));
    assert(dict_root != NULL);
    ObjString *canonical = copy_string(vm, "cache-key", 9);
    assert(dict_set(vm, dict, canonical, INT_VAL(7)));
    assert(dict->mono_cache_key == canonical);
    assert(vm_set_global_value(vm, "mono_cache_dict", OBJ_VAL(dict)));

    const char *lookup_source =
        "let suffix = \"key\"\n"
        "let dynamic_key = \"cache-\" + suffix\n"
        "let @mono_cache_lookup = @mono_cache_dict[dynamic_key]\n";
    assert(vm_interpret_named(vm, lookup_source, "<mono-cache-lookup-test>") ==
           INTERPRET_OK);
    Value lookup = NULL_VAL;
    assert(vm_get_global_value(vm, "mono_cache_lookup", &lookup));
    assert(IS_INT(lookup) && AS_INT(lookup) == 7);
    assert(dict->mono_cache_key == canonical);

    ObjString *left = copy_string(vm, "cache-", 6);
    vm_push(vm, OBJ_VAL(left));
    ObjString *right = copy_string(vm, "key", 3);
    vm_push(vm, OBJ_VAL(right));
    ObjString *temporary_rope = concat_strings(vm, left, right);
    vm_push(vm, OBJ_VAL(temporary_rope));
    assert(!dict_set(vm, dict, temporary_rope, INT_VAL(9)));
    assert(dict->mono_cache_key == canonical);
    vm_pop(vm);
    vm_pop(vm);
    vm_pop(vm);

    gc_minor_collect(vm);
    gc_minor_collect(vm);
    assert(dict->mono_cache_key == canonical);
    Value updated = NULL_VAL;
    assert(dict_get(dict, canonical, &updated));
    assert(IS_INT(updated) && AS_INT(updated) == 9);

    assert(dict_delete(dict, canonical));
    assert(dict->mono_cache_key == NULL);
    assert(dict->mono_cache_entry == NULL);
    assert(IS_NULL(dict->mono_cache_value));

    vm_unroot_value(vm, dict_root);
    vm_delete(vm);
}

static void test_major_collection_preempts_minor_for_newborns(void) {
    VM *vm = vm_new();
    assert(vm != NULL);

    ObjString *kind = copy_string(vm, "ThresholdOrderKind", 18);
    assert(kind != NULL);
    assert(!kind->obj.is_old);
    assert(kind->obj.gc_age == 0);

    /*
     * Make the next string allocation cross both thresholds. A direct major
     * collection protects the newborn kind string; a minor followed by a
     * major would age and then sweep it before the second string is allocated.
     */
    vm->young_bytes = TEST_NURSERY_LIMIT;
    vm->next_gc = vm->bytes_allocated;
    ObjString *message = copy_string(vm, "threshold-order-message", 23);
    assert(message != NULL);
    assert(kind != message);
    assert(kind->obj.is_old);
    assert(kind->length == 18);
    assert(memcmp(string_chars(vm, kind), "ThresholdOrderKind", 18) == 0);

    ObjError *error = new_error(vm, kind, message, NULL, 17, NULL, NULL);
    VMRoot *root = vm_root_value(vm, OBJ_VAL(error));
    assert(root != NULL);
    gc_major_collect(vm);
    assert(error->kind == kind);
    assert(error->message == message);
    assert(memcmp(string_chars(vm, error->kind),
                  "ThresholdOrderKind", 18) == 0);
    assert(memcmp(string_chars(vm, error->message),
                  "threshold-order-message", 23) == 0);

    vm_unroot_value(vm, root);
    vm_delete(vm);
}

static void test_constructor_barriers_after_forced_promotion(void) {
    VM *vm = vm_new();
    assert(vm != NULL);

    ObjString *type_name = copy_string(vm, "BarrierHandle", 13);
    vm_push(vm, OBJ_VAL(type_name));
    arm_minor_after_parent_alloc(vm, sizeof(ObjNativeHandle));
    ObjNativeHandle *handle = new_native_handle(vm, type_name, NULL, NULL);
    vm_pop(vm);

    assert(handle->obj.is_old);
    assert(handle->methods != NULL);
    assert(!handle->methods->obj.is_old);
    VMRoot *handle_root = vm_root_value(vm, OBJ_VAL(handle));
    assert(handle_root != NULL);
    gc_minor_collect(vm);
    assert(handle->methods->obj.is_old);
    vm_unroot_value(vm, handle_root);
    vm_delete(vm);

    vm = vm_new();
    assert(vm != NULL);

    ObjString *struct_name = copy_string(vm, "BarrierStruct", 13);
    vm_push(vm, OBJ_VAL(struct_name));
    arm_minor_after_parent_alloc(vm, sizeof(ObjStruct));
    ObjStruct *structure = new_struct(vm, struct_name);
    vm_pop(vm);

    assert(structure->obj.is_old);
    assert(structure->methods != NULL);
    assert(!structure->methods->obj.is_old);
    VMRoot *struct_root = vm_root_value(vm, OBJ_VAL(structure));
    assert(struct_root != NULL);
    gc_minor_collect(vm);
    assert(structure->methods->obj.is_old);
    vm_unroot_value(vm, struct_root);
    vm_delete(vm);
}

static Value arm_closure_capture_gc(VM *vm, int arg_count, Value *args) {
    (void)args;
    assert(arg_count == 0);
    arm_minor_after_parent_alloc(
        vm, sizeof(ObjClosure) + sizeof(ObjUpvalue *));
    return NULL_VAL;
}

static void test_closure_capture_barrier_after_forced_promotion(void) {
    VM *vm = vm_new();
    assert(vm != NULL);
    vm_register_native(vm, "arm_closure_capture_gc",
                       arm_closure_capture_gc, 0, NULL, NULL);

    const char *source =
        "fn make_closure_barrier()\n"
        "    let captured = \"kept\"\n"
        "    arm_closure_capture_gc()\n"
        "    fn read_captured()\n"
        "        return captured\n"
        "    end\n"
        "    return read_captured\n"
        "end\n"
        "let @closure_barrier_saved = make_closure_barrier()\n";
    assert(vm_interpret_named(vm, source, "<closure-barrier-test>") ==
           INTERPRET_OK);

    Value saved = NULL_VAL;
    assert(vm_get_global_value(vm, "closure_barrier_saved", &saved));
    assert(IS_CLOSURE(saved));
    ObjClosure *closure = AS_CLOSURE(saved);
    assert(closure->obj.is_old);
    assert(closure->upvalue_count == 1);
    assert(closure->upvalues[0] != NULL);
    assert(!closure->upvalues[0]->obj.is_old);

    gc_minor_collect(vm);
    assert(closure->upvalues[0]->obj.is_old);
    vm_delete(vm);
}

typedef enum {
    SERIALIZED_NAME_EDGE,
    SERIALIZED_SOURCE_EDGE,
    SERIALIZED_CONSTANT_EDGE
} SerializedFunctionEdge;

static ObjString *set_serialized_edge(VM *vm, ObjFunction *function,
                                      SerializedFunctionEdge edge) {
    const char *text = NULL;
    switch (edge) {
        case SERIALIZED_NAME_EDGE:
            text = "runtime-api-serialized-name-edge";
            break;
        case SERIALIZED_SOURCE_EDGE:
            text = "runtime-api-serialized-source-edge";
            break;
        case SERIALIZED_CONSTANT_EDGE:
            text = "runtime-api-serialized-constant-edge";
            break;
    }

    ObjString *string = copy_string(vm, text, (int)strlen(text));
    switch (edge) {
        case SERIALIZED_NAME_EDGE:
            function->name = string;
            break;
        case SERIALIZED_SOURCE_EDGE:
            function->source_name = string;
            break;
        case SERIALIZED_CONSTANT_EDGE:
            assert(chunk_add_constant(&function->chunk, OBJ_VAL(string)) == 0);
            break;
    }
    gc_write_barrier(vm, (Obj *)function, OBJ_VAL(string));
    return string;
}

static void assert_serialized_edge_barrier(const char *path,
                                           SerializedFunctionEdge edge) {
    VM *writer = vm_new();
    assert(writer != NULL);
    ObjFunction *function = new_function(writer);
    VMRoot *writer_root = vm_root_value(writer, OBJ_VAL(function));
    assert(writer_root != NULL);
    function->reg_count = 1;
    chunk_write(&function->chunk, ENCODE_ABC(OP_RETURN, 0, 0, 0), 1);
    (void)set_serialized_edge(writer, function, edge);
    assert(vm_save_bytecode(writer, function, path));
    vm_unroot_value(writer, writer_root);
    vm_delete(writer);

    VM *reader = vm_new();
    assert(reader != NULL);
    arm_minor_after_parent_alloc(reader, sizeof(ObjFunction));
    ObjFunction *loaded = vm_load_bytecode(reader, path);
    assert(loaded != NULL);
    assert(loaded->obj.is_old);

    ObjString *child = NULL;
    switch (edge) {
        case SERIALIZED_NAME_EDGE:
            child = loaded->name;
            break;
        case SERIALIZED_SOURCE_EDGE:
            child = loaded->source_name;
            break;
        case SERIALIZED_CONSTANT_EDGE:
            assert(loaded->chunk.const_count == 1);
            assert(IS_STRING(loaded->chunk.constants[0]));
            child = AS_STRING(loaded->chunk.constants[0]);
            break;
    }
    assert(child != NULL);
    assert(!child->obj.is_old);

    VMRoot *reader_root = vm_root_value(reader, OBJ_VAL(loaded));
    assert(reader_root != NULL);
    gc_minor_collect(reader);
    assert(child->obj.is_old);
    vm_unroot_value(reader, reader_root);
    vm_delete(reader);
}

static void test_deserialized_function_barriers_after_forced_promotion(void) {
    const char *path = "tests/.runtime_api_gc_barrier.mgc";
    assert_serialized_edge_barrier(path, SERIALIZED_NAME_EDGE);
    assert_serialized_edge_barrier(path, SERIALIZED_SOURCE_EDGE);
    assert_serialized_edge_barrier(path, SERIALIZED_CONSTANT_EDGE);
    assert(remove(path) == 0);
}

static ObjFunction *new_bytecode_test_function(VM *vm, int reg_count,
                                                const Instruction *code,
                                                int count) {
    ObjFunction *function = new_function(vm);
    function->reg_count = reg_count;
    for (int i = 0; i < count; i++) {
        chunk_write(&function->chunk, code[i], 1);
    }
    return function;
}

static void assert_file_starts_with(const char *path, const char *prefix) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    size_t length = strlen(prefix);
    char buffer[16];
    assert(length <= sizeof(buffer));
    assert(fread(buffer, 1, length, file) == length);
    assert(memcmp(buffer, prefix, length) == 0);
    assert(fclose(file) == 0);
}

static void test_bytecode_save_preflight_rejects_invalid_graphs(void) {
    const char *path = "tests/.runtime_api_invalid_save.mgc";
    const Instruction return_code[] = {
        ENCODE_ABC(OP_RETURN, 0, 0, 0),
    };
    VM *vm = vm_new();
    assert(vm != NULL);

    ObjFunction *function =
        new_bytecode_test_function(vm, 1, return_code, 1);
    VMRoot *root = vm_root_value(vm, OBJ_VAL(function));
    assert(root != NULL);

    FILE *sentinel = fopen(path, "wb");
    assert(sentinel != NULL);
    assert(fwrite("keep", 1, 4, sentinel) == 4);
    assert(fclose(sentinel) == 0);

    int saved_count = function->chunk.count;
    function->chunk.count = -1;
    assert(!vm_save_bytecode(vm, function, path));
    function->chunk.count = saved_count;

    Instruction *saved_code = function->chunk.code;
    function->chunk.code = NULL;
    assert(!vm_save_bytecode(vm, function, path));
    function->chunk.code = saved_code;

    int *saved_lines = function->chunk.lines;
    function->chunk.lines = NULL;
    assert(!vm_save_bytecode(vm, function, path));
    function->chunk.lines = saved_lines;

    assert(chunk_add_constant(&function->chunk, OBJ_VAL(function)) == 0);
    assert(!vm_save_bytecode(vm, function, path));

    ObjArray *unsupported = new_array(vm);
    function->chunk.constants[0] = OBJ_VAL(unsupported);
    gc_write_barrier(vm, (Obj *)function, OBJ_VAL(unsupported));
    assert(!vm_save_bytecode(vm, function, path));

    Value *saved_constants = function->chunk.constants;
    function->chunk.constants = NULL;
    assert(!vm_save_bytecode(vm, function, path));
    function->chunk.constants = saved_constants;

    int saved_const_count = function->chunk.const_count;
    function->chunk.const_count = -1;
    assert(!vm_save_bytecode(vm, function, path));
    function->chunk.const_count = saved_const_count;

    assert_file_starts_with(path, "keep");
    assert(remove(path) == 0);
    vm_unroot_value(vm, root);
    vm_delete(vm);
}

#define TEST_BYTECODE_FUNCTION_DEPTH 65

static void test_bytecode_function_depth_limits(void) {
    const char *path = "tests/.runtime_api_depth.mgc";
    const Instruction return_code[] = {
        ENCODE_ABC(OP_RETURN, 0, 0, 0),
    };
    VM *vm = vm_new();
    assert(vm != NULL);

    ObjFunction *functions[TEST_BYTECODE_FUNCTION_DEPTH];
    VMRoot *roots[TEST_BYTECODE_FUNCTION_DEPTH];
    for (int i = 0; i < TEST_BYTECODE_FUNCTION_DEPTH; i++) {
        functions[i] =
            new_bytecode_test_function(vm, 1, return_code, 1);
        roots[i] = vm_root_value(vm, OBJ_VAL(functions[i]));
        assert(roots[i] != NULL);
    }
    for (int i = 0; i + 1 < TEST_BYTECODE_FUNCTION_DEPTH; i++) {
        assert(chunk_add_constant(&functions[i]->chunk,
                                  OBJ_VAL(functions[i + 1])) == 0);
        gc_write_barrier(vm, (Obj *)functions[i],
                         OBJ_VAL(functions[i + 1]));
    }

    assert(vm_save_bytecode(vm, functions[1], path));
    assert(!vm_save_bytecode(vm, functions[0], path));
    assert(vm_load_bytecode(vm, path) != NULL);
    assert(remove(path) == 0);

    for (int i = TEST_BYTECODE_FUNCTION_DEPTH - 1; i >= 0; i--) {
        vm_unroot_value(vm, roots[i]);
    }
    vm_delete(vm);
}

static void write_test_item(FILE *file, const void *item, size_t size) {
    assert(fwrite(item, size, 1, file) == 1);
}

static void write_nested_bytecode_function(FILE *file, int remaining) {
    int zero = 0;
    int one = 1;
    int32_t null_string = -1;
    Instruction return_instruction =
        ENCODE_ABC(OP_RETURN, 0, 0, 0);

    write_test_item(file, &zero, sizeof(int));
    write_test_item(file, &zero, sizeof(int));
    write_test_item(file, &one, sizeof(int));
    write_test_item(file, &null_string, sizeof(int32_t));
    write_test_item(file, &null_string, sizeof(int32_t));
    write_test_item(file, &one, sizeof(int));
    write_test_item(file, &return_instruction, sizeof(Instruction));
    write_test_item(file, &one, sizeof(int));

    int constant_count = remaining > 1 ? 1 : 0;
    write_test_item(file, &constant_count, sizeof(int));
    if (constant_count != 0) {
        uint8_t function_type = 4;
        write_test_item(file, &function_type, sizeof(uint8_t));
        write_nested_bytecode_function(file, remaining - 1);
    }
}

static void test_bytecode_load_rejects_excessive_depth(void) {
    const char *path = "tests/.runtime_api_excessive_depth.mgc";
    const uint32_t magic = 0x4D47010F;
    FILE *file = fopen(path, "wb");
    assert(file != NULL);
    write_test_item(file, &magic, sizeof(uint32_t));
    write_nested_bytecode_function(file, TEST_BYTECODE_FUNCTION_DEPTH);
    assert(fclose(file) == 0);

    VM *vm = vm_new();
    assert(vm != NULL);
    assert(vm_load_bytecode(vm, path) == NULL);
    vm_delete(vm);
    assert(remove(path) == 0);
}

static bool try_save_bytecode_shape(VM *vm, const char *path,
                                    int reg_count,
                                    const Instruction *code, int count) {
    ObjFunction *function =
        new_bytecode_test_function(vm, reg_count, code, count);
    VMRoot *root = vm_root_value(vm, OBJ_VAL(function));
    assert(root != NULL);
    bool result = vm_save_bytecode(vm, function, path);
    vm_unroot_value(vm, root);
    return result;
}

static void test_bytecode_instruction_boundaries(void) {
    const char *path = "tests/.runtime_api_instruction_shape.mgc";
    VM *vm = vm_new();
    assert(vm != NULL);

    const Instruction fallthrough[] = {
        ENCODE_ABC(OP_LOADNIL, 0, 0, 0),
    };
    assert(!try_save_bytecode_shape(vm, path, 1, fallthrough, 1));

    const Instruction skip_past_end[] = {
        ENCODE_ABC(OP_LOADBOOL, 0, 1, 1),
        ENCODE_ABC(OP_RETURN, 0, 0, 0),
    };
    assert(!try_save_bytecode_shape(vm, path, 1, skip_past_end, 2));

    const Instruction standalone_aux[] = {
        ENCODE_ABC(OP_AUX, 0, 0, 0),
        ENCODE_ABC(OP_RETURN, 0, 0, 0),
    };
    assert(!try_save_bytecode_shape(vm, path, 1, standalone_aux, 2));

    ObjFunction *inner = new_function(vm);
    VMRoot *inner_root = vm_root_value(vm, OBJ_VAL(inner));
    assert(inner_root != NULL);
    inner->reg_count = 1;
    inner->upvalue_count = 1;
    chunk_write(&inner->chunk, ENCODE_ABC(OP_RETURN, 0, 0, 0), 1);

    const Instruction metadata_jump[] = {
        ENCODE_sBx(OP_JMP, 1),
        ENCODE_ABx(OP_CLOSURE, 0, 0),
        ENCODE_ABC(1, 0, 0, 0),
        ENCODE_ABC(OP_RETURN, 0, 0, 0),
    };
    ObjFunction *outer =
        new_bytecode_test_function(vm, 1, metadata_jump, 4);
    VMRoot *outer_root = vm_root_value(vm, OBJ_VAL(outer));
    assert(outer_root != NULL);
    assert(chunk_add_constant(&outer->chunk, OBJ_VAL(inner)) == 0);
    gc_write_barrier(vm, (Obj *)outer, OBJ_VAL(inner));
    assert(!vm_save_bytecode(vm, outer, path));
    vm_unroot_value(vm, outer_root);
    vm_unroot_value(vm, inner_root);

    const Instruction indexed_field[] = {
        ENCODE_ABC(OP_SETFIELD_IDX, 0, 1, 255),
        ENCODE_ABC(OP_RETURN, 0, 0, 0),
    };
    assert(try_save_bytecode_shape(vm, path, 2, indexed_field, 2));
    assert(vm_load_bytecode(vm, path) != NULL);

    ObjFunction *upvalue_function =
        new_bytecode_test_function(vm, 1, (Instruction[]){
            ENCODE_ABC(OP_GETUPVAL, 0, 1, 0),
            ENCODE_ABC(OP_RETURN, 0, 0, 0),
        }, 2);
    VMRoot *upvalue_root =
        vm_root_value(vm, OBJ_VAL(upvalue_function));
    assert(upvalue_root != NULL);
    upvalue_function->upvalue_count = 2;
    assert(vm_save_bytecode(vm, upvalue_function, path));
    assert(vm_load_bytecode(vm, path) != NULL);
    vm_unroot_value(vm, upvalue_root);

    assert(remove(path) == 0);
    vm_delete(vm);
}

static void test_interpolated_escape_line_tracking(void) {
    const char *source =
        "_\"left\\\n"
        "right\"\n"
        "let value = 1\n";
    Scanner scanner;
    scanner_init(&scanner, source);

    Token interpolated = scan_token(&scanner);
    assert(interpolated.type == TOKEN_INTERP_STRING);
    Token let_token = scan_token(&scanner);
    assert(let_token.type == TOKEN_LET);
    assert(let_token.line == 3);
}

static void test_string_and_global_api(void) {
    VM *vm = vm_new();
    assert(vm != NULL);

    ObjString *left = copy_string(vm, "ab", 2);
    vm_push(vm, OBJ_VAL(left));
    ObjString *right = copy_string(vm, "cd", 2);
    vm_push(vm, OBJ_VAL(right));
    ObjString *rope = concat_strings(vm, left, right);
    vm_pop(vm);
    vm_pop(vm);

    char buffer[5];
    size_t required = 0;
    assert(vm_string_length(OBJ_VAL(rope)) == 4);
    assert(vm_string_copy(vm, OBJ_VAL(rope), buffer, sizeof(buffer), &required));
    assert(required == sizeof(buffer));
    assert(memcmp(buffer, "abcd", sizeof(buffer)) == 0);

    assert(vm_set_global_value(vm, "answer", INT_VAL(1)));
    assert(vm_set_global_value(vm, "answer", INT_VAL(2)));
    Value answer = NULL_VAL;
    assert(vm_get_global_value(vm, "answer", &answer));
    assert(IS_INT(answer) && AS_INT(answer) == 2);

    vm_delete(vm);
}

static void test_stack_relocation_and_error_unwind(void) {
    VM *vm = vm_new();
    assert(vm != NULL);
    vm_register_native(vm, "grow_stack", grow_stack, 1, NULL, NULL);
    ObjNativeHandle *stable_handle =
        vm_new_native_handle(vm, "StableHandle", NULL, NULL);
    assert(stable_handle != NULL);
    assert(vm_native_handle_set_method(vm, stable_handle, "grow",
                                       grow_stack, 1, NULL, NULL));
    assert(vm_set_global_value(vm, "stable_handle",
                               vm_native_handle_value(stable_handle)));

    const char *grow_source =
        "let captured = 41\n"
        "fn read_captured()\n"
        "    return captured\n"
        "end\n"
        "let @native_arg_after_growth = grow_stack(\"stable\")\n"
        "let @native_handle_after_growth = @stable_handle.grow()\n"
        "let @after_growth = read_captured()\n";
    assert(vm_interpret_named(vm, grow_source, "<api-stack-test>") == INTERPRET_OK);

    Value after_growth = NULL_VAL;
    assert(vm_get_global_value(vm, "after_growth", &after_growth));
    assert(IS_NUMBER(after_growth) && AS_NUMBER(after_growth) == 41);
    Value native_arg_after_growth = NULL_VAL;
    assert(vm_get_global_value(vm, "native_arg_after_growth",
                               &native_arg_after_growth));
    assert(IS_STRING(native_arg_after_growth));
    assert(AS_STRING(native_arg_after_growth)->length == 6);
    assert(memcmp(string_chars(vm, AS_STRING(native_arg_after_growth)),
                  "stable", 6) == 0);
    Value native_handle_after_growth = NULL_VAL;
    assert(vm_get_global_value(vm, "native_handle_after_growth",
                               &native_handle_after_growth));
    assert(IS_NATIVE_HANDLE(native_handle_after_growth));
    assert(AS_NATIVE_HANDLE(native_handle_after_growth) == stable_handle);

    const char *failing_source =
        "let captured = \"kept\"\n"
        "fn saved_reader()\n"
        "    return captured\n"
        "end\n"
        "let @saved_reader = saved_reader\n"
        "error(\"expected test failure\")\n";
    assert(vm_interpret_named(vm, failing_source, "<api-unwind-test>") ==
           INTERPRET_RUNTIME_ERROR);

    const char *reuse_source = "let @after_error = @saved_reader()\n";
    assert(vm_interpret_named(vm, reuse_source, "<api-reuse-test>") == INTERPRET_OK);
    Value after_error = NULL_VAL;
    assert(vm_get_global_value(vm, "after_error", &after_error));
    assert(IS_STRING(after_error));
    assert(AS_STRING(after_error)->length == 4);
    assert(memcmp(string_chars(vm, AS_STRING(after_error)), "kept", 4) == 0);

    vm_delete(vm);
}

static void test_return_releases_dead_stack_windows(void) {
    VM *vm = vm_new();
    assert(vm != NULL);
    vm_register_native(vm, "assert_compact_stack", assert_compact_stack,
                       0, NULL, NULL);

    const char *source =
        "fn wide_frame()\n"
        "    let a00 = 0\n"
        "    let a01 = 1\n"
        "    let a02 = 2\n"
        "    let a03 = 3\n"
        "    let a04 = 4\n"
        "    let a05 = 5\n"
        "    let a06 = 6\n"
        "    let a07 = 7\n"
        "    let a08 = 8\n"
        "    let a09 = 9\n"
        "    let a10 = 10\n"
        "    let a11 = 11\n"
        "    let a12 = 12\n"
        "    let a13 = 13\n"
        "    let a14 = 14\n"
        "    let a15 = 15\n"
        "    return a00 + a15\n"
        "end\n"
        "fn caller()\n"
        "    let value = wide_frame()\n"
        "    assert_compact_stack()\n"
        "    return value\n"
        "end\n"
        "let @compact_result = caller()\n";
    assert(vm_interpret_named(vm, source, "<api-return-stack-test>") ==
           INTERPRET_OK);
    Value result = NULL_VAL;
    assert(vm_get_global_value(vm, "compact_result", &result));
    assert(IS_NUMBER(result) && AS_NUMBER(result) == 15);
    vm_delete(vm);
}

static void test_ffi_glue_null_inputs(void) {
    mg_runtime_error_simple(NULL, NULL);
    assert(mg_get_native_userdata(NULL) == NULL);
}

static char *make_large_nested_compile_source(size_t literal_size) {
    static const char *prefix =
        "let @compiler_gc_before = 11\n"
        "fn compiler_gc_nested()\n";
    static const char *line_prefixes[] = {
        "    let first = \"",
        "    let second = \"",
        "    let third = \"",
        "    let fourth = \"",
        "    let fifth = \"",
        "    let last = \"",
    };
    static const char *suffix =
        "    return len(first) + len(last)\n"
        "end\n"
        "let @compiler_gc_result = compiler_gc_nested()\n"
        "let @compiler_gc_after = 22\n";
    const size_t literal_count = sizeof(line_prefixes) / sizeof(line_prefixes[0]);
    size_t total = strlen(prefix) + strlen(suffix) + 1;
    for (size_t i = 0; i < literal_count; i++) {
        size_t overhead = strlen(line_prefixes[i]) + 2; /* closing quote + newline */
        assert(literal_size <= SIZE_MAX - total - overhead);
        total += literal_size + overhead;
    }

    char *source = (char *)malloc(total);
    assert(source != NULL);
    char *cursor = source;
    size_t prefix_len = strlen(prefix);
    memcpy(cursor, prefix, prefix_len);
    cursor += prefix_len;
    for (size_t i = 0; i < literal_count; i++) {
        size_t line_prefix_len = strlen(line_prefixes[i]);
        memcpy(cursor, line_prefixes[i], line_prefix_len);
        cursor += line_prefix_len;
        memset(cursor, (int)('a' + i), literal_size);
        cursor += literal_size;
        *cursor++ = '"';
        *cursor++ = '\n';
    }
    size_t suffix_len = strlen(suffix);
    memcpy(cursor, suffix, suffix_len);
    cursor += suffix_len;
    *cursor = '\0';
    assert((size_t)(cursor - source) + 1 == total);
    return source;
}

static void preintern_compiler_builtins(VM *vm) {
    static const char *builtins[] = {
        "print", "len", "push", "type", "tostring", "tonumber",
        "assert", "error", "input", "Err", "Error", "math", "string",
        "array", "dict", "io", "fs", "path", "os", "process", "task",
        "vm", "coroutine", "__builtin_print", "__builtin_len",
        "__builtin_push", "__builtin_type", "__builtin_tostring",
        "__builtin_tonumber", "__ffi_bind"
    };
    for (size_t i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++) {
        (void)copy_string(vm, builtins[i], (int)strlen(builtins[i]));
    }
}

static char *make_compiler_global_root_source(size_t literal_size) {
    static const char *prefix =
        "extern const compiler_gc_host_value: number\n"
        "let filler = \"";
    static const char *suffix =
        "\"\n"
        "let second_filler = \"force-major-collection\"\n"
        "let @compiler_gc_host_copy = compiler_gc_host_value\n";
    size_t prefix_len = strlen(prefix);
    size_t suffix_len = strlen(suffix);
    assert(literal_size <= SIZE_MAX - prefix_len - suffix_len - 1);
    size_t total = prefix_len + literal_size + suffix_len + 1;
    char *source = (char *)malloc(total);
    assert(source != NULL);
    memcpy(source, prefix, prefix_len);
    memset(source + prefix_len, 'g', literal_size);
    memcpy(source + prefix_len + literal_size, suffix, suffix_len + 1);
    return source;
}

static void test_compiler_global_names_are_gc_roots(void) {
    VM *vm = vm_new();
    assert(vm != NULL);
    assert(vm->stack_size == 0);
    assert(vm->stack_top == 0);
    preintern_compiler_builtins(vm);

    /*
     * compiler_init allocates one function and one unique name string, then
     * the extern declaration allocates its analysis-only name. Put those three
     * allocations exactly on the nursery boundary. The large following
     * literal triggers a minor collection, and its external character storage
     * makes the next string allocation trigger a major collection.
     */
    size_t setup_size = sizeof(ObjFunction) + 2 * sizeof(ObjString);
    assert(setup_size < TEST_NURSERY_LIMIT);
    vm->young_bytes = TEST_NURSERY_LIMIT - setup_size;
    assert(vm->next_gc < SIZE_MAX - 65536);
    size_t literal_size = vm->next_gc + 65536;
    char *source = make_compiler_global_root_source(literal_size);
    ObjFunction *function = vm_compile_named(
        vm, source, "<compiler-global-root-test>");
    free(source);

    assert(function != NULL);
    assert(vm->host_roots == NULL);
    assert(vm->stack_size == 0);
    assert(vm->stack_top == 0);
    vm_delete(vm);
}

static void test_active_compiler_functions_are_gc_roots(void) {
    VM *vm = vm_new();
    assert(vm != NULL);
    assert(vm->host_roots == NULL);

    /*
     * Size literals from the current major-GC threshold so compilation
     * deterministically crosses multiple major collections without relying on
     * a platform-specific VM initialization footprint.
     */
    assert(vm->next_gc < SIZE_MAX - 65536);
    size_t literal_size = vm->next_gc + 65536;
    char *source = make_large_nested_compile_source(literal_size);
    assert(vm_interpret_named(vm, source, "<compiler-gc-root-test>") ==
           INTERPRET_OK);
    free(source);

    Value value = NULL_VAL;
    assert(vm_get_global_value(vm, "compiler_gc_before", &value));
    assert(IS_NUMERIC(value) && AS_NUMBER(value) == 11);
    assert(vm_get_global_value(vm, "compiler_gc_result", &value));
    assert(IS_NUMERIC(value));
    assert(AS_NUMBER(value) == (double)(literal_size * 2));
    assert(vm_get_global_value(vm, "compiler_gc_after", &value));
    assert(IS_NUMERIC(value) && AS_NUMBER(value) == 22);
    assert(vm->host_roots == NULL);
    assert(vm->stack_size == 0);
    assert(vm->stack_top == 0);

    /* Compiler failures must release the active-function root as well. */
    assert(vm_compile(vm, "break\n") == NULL);
    assert(vm->host_roots == NULL);
    assert(vm->stack_size == 0);
    assert(vm->stack_top == 0);

    vm_delete(vm);
}

static bool is_disabled_batch_opcode(OpCode opcode) {
    switch (opcode) {
        case OP_FORADDLOCAL_FIELD_PROP:
        case OP_FORADDLOCAL_FIELD_PROP_INC:
        case OP_FORADDGLOBAL_FIELD_PROP:
        case OP_FORADDGLOBAL_FIELD_PROP_INC:
        case OP_FOR_MODI_ACCUM:
        case OP_FOR_MODI_ACCUM_INC:
        case OP_FOR_FIELD2_ACCUM:
        case OP_FOR_FIELD2_ACCUM_INC:
        case OP_ARRAY_MARK_FALSE_STRIDE:
            return true;
        default:
            return false;
    }
}

static bool function_contains_disabled_batch_opcode(ObjFunction *function) {
    for (int i = 0; i < function->chunk.count; i++) {
        if (is_disabled_batch_opcode(
                (OpCode)GET_OPCODE(function->chunk.code[i]))) {
            return true;
        }
    }
    for (int i = 0; i < function->chunk.const_count; i++) {
        Value constant = function->chunk.constants[i];
        if (IS_FUNCTION(constant) &&
            function_contains_disabled_batch_opcode(AS_FUNCTION(constant))) {
            return true;
        }
    }
    return false;
}

static bool function_contains_opcode_pair(ObjFunction *function,
                                          OpCode first, OpCode second) {
    for (int i = 0; i + 1 < function->chunk.count; i++) {
        if ((OpCode)GET_OPCODE(function->chunk.code[i]) == first &&
            (OpCode)GET_OPCODE(function->chunk.code[i + 1]) == second) {
            return true;
        }
    }
    for (int i = 0; i < function->chunk.const_count; i++) {
        Value constant = function->chunk.constants[i];
        if (IS_FUNCTION(constant) &&
            function_contains_opcode_pair(AS_FUNCTION(constant),
                                          first, second)) {
            return true;
        }
    }
    return false;
}

static void test_local_update_loop_execution(void) {
    static const char *source =
        "let add_value = 0\n"
        "let add_step = 2\n"
        "loop\n"
        "    if add_value >= 6 then break end\n"
        "    add_value = add_value + add_step\n"
        "end\n"
        "let @fused_add = add_value\n"
        "let sub_value = 6\n"
        "let sub_step = 2\n"
        "loop\n"
        "    if sub_value <= 0 then break end\n"
        "    sub_value = sub_value - sub_step\n"
        "end\n"
        "let @fused_sub = sub_value\n"
        "let double_value = 0.5\n"
        "let double_step = 0.25\n"
        "loop\n"
        "    if double_value >= 1.5 then break end\n"
        "    double_value = double_value + double_step\n"
        "end\n"
        "let @fused_double = double_value\n"
        "let overflow_value = 2147483646\n"
        "let overflow_step = 2\n"
        "loop\n"
        "    if overflow_value > 2147483646 then break end\n"
        "    overflow_value = overflow_value + overflow_step\n"
        "end\n"
        "let @fused_overflow = overflow_value\n"
        "let text = \"\"\n"
        "let piece = \"x\"\n"
        "let repeats = 0\n"
        "loop\n"
        "    if repeats >= 3 then break end\n"
        "    repeats = repeats + 1\n"
        "    text = text + piece\n"
        "end\n"
        "let @fused_text = text\n";

    VM *vm = vm_new();
    assert(vm != NULL);
    ObjFunction *function =
        vm_compile_named(vm, source, "<local-update-loop-test>");
    assert(function != NULL);
    assert(function_contains_opcode_pair(function, OP_ADDLOCAL, OP_LOOP));
    assert(function_contains_opcode_pair(function, OP_SUBLOCAL, OP_LOOP));
    assert(vm_run_function(vm, function) == INTERPRET_OK);

    Value value = NULL_VAL;
    assert(vm_get_global_value(vm, "fused_add", &value));
    assert(IS_NUMERIC(value) && AS_NUMBER(value) == 6.0);
    assert(vm_get_global_value(vm, "fused_sub", &value));
    assert(IS_NUMERIC(value) && AS_NUMBER(value) == 0.0);
    assert(vm_get_global_value(vm, "fused_double", &value));
    assert(IS_NUMBER(value) && AS_DOUBLE(value) == 1.5);
    assert(vm_get_global_value(vm, "fused_overflow", &value));
    assert(IS_NUMBER(value) && AS_DOUBLE(value) == 2147483648.0);
    assert(vm_get_global_value(vm, "fused_text", &value));
    assert(IS_STRING(value));
    assert(vm_string_length(value) == 3);
    assert(memcmp(vm_string_chars_resolved(vm, value), "xxx", 3) == 0);

    ObjFunction *infinite = vm_compile_named(
        vm,
        "let value = 0\n"
        "let step = 1\n"
        "loop\n"
        "    value = value + step\n"
        "end\n",
        "<local-update-loop-cancel-test>");
    assert(infinite != NULL);
    assert(function_contains_opcode_pair(infinite, OP_ADDLOCAL, OP_LOOP));
    vm->suppress_error_output = true;
    vm->loop_cancel_counter = 0;
    MG_ATOMIC_STORE_BOOL(vm->cancel_requested, true);
    assert(vm_run_function(vm, infinite) == INTERPRET_RUNTIME_ERROR);
    assert(strstr(vm_last_error(vm), "cancelled") != NULL);

    vm_delete(vm);
}

static void test_range_loop_entry_cancellation(void) {
    static const char *source =
        "for i in 0..10\n"
        "    let value = i\n"
        "end\n";
    static const char *branching_source =
        "let total = 0\n"
        "for i in 0..2\n"
        "    if i % 3 == 0 then\n"
        "        total = total + 1\n"
        "    elseif i % 3 == 1 then\n"
        "        total = total + 2\n"
        "    else\n"
        "        total = total + 3\n"
        "    end\n"
        "end\n";

    VM *vm = vm_new();
    assert(vm != NULL);
    ObjFunction *function =
        vm_compile_named(vm, source, "<range-loop-cancel-test>");
    assert(function != NULL);

    vm->suppress_error_output = true;
    vm->loop_cancel_counter = 0;
    MG_ATOMIC_STORE_BOOL(vm->cancel_requested, true);
    assert(vm_run_function(vm, function) == INTERPRET_RUNTIME_ERROR);
    assert(strstr(vm_last_error(vm), "cancelled") != NULL);
    vm_delete(vm);

    vm = vm_new();
    assert(vm != NULL);
    function = vm_compile_named(
        vm, branching_source, "<range-branch-cancel-test>");
    assert(function != NULL);

    vm->suppress_error_output = true;
    vm->loop_cancel_counter = 1;
    MG_ATOMIC_STORE_BOOL(vm->cancel_requested, true);
    assert(vm_run_function(vm, function) == INTERPRET_RUNTIME_ERROR);
    assert(strstr(vm_last_error(vm), "cancelled") != NULL);
    vm_delete(vm);
}

static void test_wide_field_constant_local_update(void) {
    size_t capacity = 32768;
    char *source = (char *)malloc(capacity);
    assert(source != NULL);

    size_t length = (size_t)snprintf(source, capacity, "if false then\n");
    assert(length < capacity);
    for (int i = 0; i < 300; i++) {
        int written = snprintf(source + length, capacity - length,
                               "    print(\"filler-%03d\")\n", i);
        assert(written > 0);
        assert((size_t)written < capacity - length);
        length += (size_t)written;
    }
    int written = snprintf(
        source + length, capacity - length,
        "end\n"
        "let config = &< value = 7 >\n"
        "let total = 1\n"
        "total = total + config<\"value\">?\n"
        "let @wide_field_total = total\n");
    assert(written > 0);
    assert((size_t)written < capacity - length);

    VM *vm = vm_new();
    assert(vm != NULL);
    ObjFunction *function =
        vm_compile_named(vm, source, "<wide-field-constant-test>");
    free(source);
    assert(function != NULL);
    assert(function->chunk.const_count > UINT8_MAX);
    assert(!function_contains_opcode_pair(function,
                                          OP_ADDLOCAL_FIELD_PROP, OP_LOOP));
    assert(vm_run_function(vm, function) == INTERPRET_OK);

    Value value = NULL_VAL;
    assert(vm_get_global_value(vm, "wide_field_total", &value));
    assert(IS_NUMERIC(value) && AS_NUMBER(value) == 8.0);
    vm_delete(vm);
}

static char *read_test_source(const char *path) {
    FILE *file = fopen(path, "rb");
    assert(file != NULL);
    assert(fseek(file, 0, SEEK_END) == 0);
    long length = ftell(file);
    assert(length >= 0);
    assert(fseek(file, 0, SEEK_SET) == 0);

    char *source = (char *)malloc((size_t)length + 1);
    assert(source != NULL);
    assert(fread(source, 1, (size_t)length, file) == (size_t)length);
    source[length] = '\0';
    assert(fclose(file) == 0);
    return source;
}

static void test_compiler_disables_exact_shape_batch_opcodes(void) {
    static const char *benchmarks[] = {
        "benchmark/control_flow.mg",
        "benchmark/object_fields.mg",
        "benchmark/fallible_lookup.mg",
        "benchmark/sieve.mg",
    };

    VM *vm = vm_new();
    assert(vm != NULL);
    for (size_t i = 0; i < sizeof(benchmarks) / sizeof(benchmarks[0]); i++) {
        char *source = read_test_source(benchmarks[i]);
        ObjFunction *function = vm_compile_named(vm, source, benchmarks[i]);
        free(source);
        assert(function != NULL);

        VMRoot *root = vm_root_value(vm, OBJ_VAL(function));
        assert(root != NULL);
        assert(!function_contains_disabled_batch_opcode(function));
        vm_unroot_value(vm, root);
    }
    vm_delete(vm);
}

int main(void) {
    test_gc_barriers_and_host_roots();
    test_dict_mono_cache_uses_owned_key();
    test_major_collection_preempts_minor_for_newborns();
    test_constructor_barriers_after_forced_promotion();
    test_closure_capture_barrier_after_forced_promotion();
    test_deserialized_function_barriers_after_forced_promotion();
    test_bytecode_save_preflight_rejects_invalid_graphs();
    test_bytecode_function_depth_limits();
    test_bytecode_load_rejects_excessive_depth();
    test_bytecode_instruction_boundaries();
    test_interpolated_escape_line_tracking();
    test_string_and_global_api();
    test_stack_relocation_and_error_unwind();
    test_return_releases_dead_stack_windows();
    test_ffi_glue_null_inputs();
    test_compiler_global_names_are_gc_roots();
    test_active_compiler_functions_are_gc_roots();
    test_local_update_loop_execution();
    test_range_loop_entry_cancellation();
    test_wide_field_constant_local_update();
    test_compiler_disables_exact_shape_batch_opcodes();
    return 0;
}
