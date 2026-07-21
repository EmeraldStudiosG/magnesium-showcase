#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifdef _WIN32
#include <stdlib.h>
#else
#include <unistd.h>
#endif
#include "magnesium.h"

/* Compiler: AST walker producing bytecode */

/* ========================================================================
 * Chunk Management
 * ======================================================================== */
void chunk_init(Chunk *chunk) {
    chunk->count = 0;
    chunk->capacity = 0;
    chunk->code = NULL;
    chunk->lines = NULL;
    chunk->const_count = 0;
    chunk->const_capacity = 0;
    chunk->constants = NULL;
}

void chunk_free(Chunk *chunk) {
    free(chunk->code);
    free(chunk->lines);
    free(chunk->constants);
    chunk_init(chunk);
}

void chunk_write(Chunk *chunk, Instruction inst, int line) {
    if (chunk->capacity < chunk->count + 1) {
        chunk->capacity = chunk->capacity < 8 ? 8 : chunk->capacity * 2;
        chunk->code = realloc(chunk->code, sizeof(Instruction) * chunk->capacity);
        chunk->lines = realloc(chunk->lines, sizeof(int) * chunk->capacity);
    }
    chunk->code[chunk->count] = inst;
    chunk->lines[chunk->count] = line;
    chunk->count++;
}

int chunk_add_constant(Chunk *chunk, Value value) {
    for (int i = 0; i < chunk->const_count; i++) {
        Value existing = chunk->constants[i];
        if (IS_NUMERIC(existing) && IS_NUMERIC(value) &&
            AS_NUMBER(existing) == AS_NUMBER(value)) {
            return i;
        }
        if (existing == value) return i;
    }
    if (chunk->const_capacity < chunk->const_count + 1) {
        chunk->const_capacity = chunk->const_capacity < 8 ? 8 : chunk->const_capacity * 2;
        chunk->constants = realloc(chunk->constants, sizeof(Value) * chunk->const_capacity);
    }
    chunk->constants[chunk->const_count] = value;
    return chunk->const_count++;
}

/* ========================================================================
 * Compiler State
 * ======================================================================== */
typedef struct {
    Token name;
    int reg;         /* register slot for this local */
    int depth;       /* scope depth */
    bool is_const;
    bool is_captured; /* captured by a closure? */
    bool has_known_struct;
    Token known_struct;
} Local;

typedef struct {
    int index;
    bool is_local;
} UpvalueInfo;

typedef enum {
    FUNC_SCRIPT,
    FUNC_FUNCTION,
    FUNC_METHOD
} FunctionType;

typedef struct {
    ASTNode *expr;   /* the deferred call expression */
    int depth;       /* scope depth when defer was declared */
} DeferEntry;

typedef struct {
    Token name;
    ASTNode *fn_decl;
    int binding_reg;
} InlineCandidate;

typedef struct {
    Token struct_name;
    Token method_name;
    Token field_names[2];
    int param_indices[2];
    int update_count;
} FieldLoopCandidate;

typedef struct Compiler Compiler;
typedef struct {
    int offset;
    bool is_plain_jump;
} JumpPatch;

struct Compiler {
    Compiler *enclosing;
    ObjFunction *function;
    FunctionType type;
    VM *vm;

    Local locals[MAX_LOCALS];
    int local_count;
    int scope_depth;
    int next_reg;        /* next free register */
    int max_reg;         /* high watermark */

    UpvalueInfo upvalues[MAX_UPVALUES];

    /* Global tracking for static analysis */
    ObjString** globals;
    int global_count;
    int global_capacity;
    bool push_shadowed;
    bool tostring_shadowed;
    bool len_shadowed;
    bool string_shadowed;
    bool array_shadowed;
    bool dict_shadowed;
    InlineCandidate *inline_candidates;
    int inline_count;
    int inline_capacity;
    FieldLoopCandidate *field_loop_candidates;
    int field_loop_count;
    int field_loop_capacity;
    char **owned_strings;
    int owned_string_count;
    int owned_string_capacity;
    bool self_call_safe;
    bool module_mode;
    int module_exports_reg;

    /* Loop state for break/continue */
    int loop_start;      /* instruction offset of loop top */
    int *break_jumps;    /* patch list for break statements */
    int break_count;
    int break_capacity;

    int error_handler_reg;
    int *error_jumps;
    int error_jump_count;
    int error_jump_capacity;

    /* Defer tracking */
    DeferEntry *defers;
    int defer_count;
    int defer_capacity;

    bool had_error;
};

static MG_THREAD_LOCAL Compiler *current = NULL;

/* Helpers */
static Chunk *current_chunk(void) {
    return &current->function->chunk;
}

static int alloc_reg(void) {
    int r = current->next_reg++;
    if (current->next_reg > current->max_reg)
        current->max_reg = current->next_reg;
    if (r >= MAX_REGISTERS) {
        fprintf(stderr, "Too many registers needed.\n");
        exit(1);
    }
    return r;
}

static bool is_temp_reg(int reg) {
    for (int i = 0; i < current->local_count; i++) {
        if (current->locals[i].reg == reg) return false;
    }
    return true;
}

static void free_temp(int reg) {
    if (reg >= 0 && reg == current->next_reg - 1 && is_temp_reg(reg)) {
        current->next_reg--;
    }
}

static void reserve_reg_index(int reg) {
    if (reg >= MAX_REGISTERS) {
        fprintf(stderr, "Too many registers needed.\n");
        exit(1);
    }
    if (reg + 1 > current->max_reg) {
        current->max_reg = reg + 1;
    }
}

static void emit(Instruction inst, int line) {
    chunk_write(current_chunk(), inst, line);
}

static void error_at(Token *token, const char *message) {
    current->had_error = true;
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
}

static int emit_jump(OpCode op, int line) {
    /* Emit a jump with placeholder offset, return patch address */
    emit(ENCODE_sBx(op, 0), line);
    return current_chunk()->count - 1;
}

static void patch_jump(int offset) {
    int jump = current_chunk()->count - offset - 1;
    current_chunk()->code[offset] = ENCODE_sBx(OP_JMP, jump);
}

static int emit_test_jump(int cond_reg, int line) {
    emit(ENCODE_AsBx(OP_TESTJMP, cond_reg, 0), line);
    return current_chunk()->count - 1;
}

static int emit_error_test_jump(int err_reg, int line) {
    emit(ENCODE_AsBx(OP_TESTERRJMP, err_reg, 0), line);
    return current_chunk()->count - 1;
}

static int emit_asbx_jump(OpCode op, int a, int line) {
    emit(ENCODE_AsBx(op, a, 0), line);
    return current_chunk()->count - 1;
}

static int emit_immediate_test_jump(OpCode op, int reg, int imm, int line) {
    emit(ENCODE_ABC(op, reg, imm & 0xff, 0), line);
    return emit_jump(OP_JMP, line);
}

static void patch_test_jump(int offset) {
    int jump = current_chunk()->count - offset - 1;
    current_chunk()->code[offset] &= 0x0000FFFF;
    current_chunk()->code[offset] |= ((uint32_t)(jump + 0x8000)) << 16;
}

static void patch_asbx_jump(int offset) {
    int jump = current_chunk()->count - offset - 1;
    Instruction inst = current_chunk()->code[offset];
    current_chunk()->code[offset] = ENCODE_AsBx(GET_OPCODE(inst), GET_AsBx_A(inst), jump);
}

static int make_constant(Value value) {
    return chunk_add_constant(current_chunk(), value);
}

static int string_constant(VM *vm, const char *chars, int length) {
    ObjString *str = copy_string(vm, chars, length);
    return make_constant(OBJ_VAL(str));
}

static int identifier_constant(VM *vm, Token *name) {
    return string_constant(vm, name->start, name->length);
}

/* Forward declarations */
static int compile_node(ASTNode *node);
static void compile_stmt(ASTNode *node);
static void compile_block(ASTNode *node);
static int compile_try_block(ASTNode *node);
static void compile_try_catch(ASTNode *node);
static void add_error_jump(int offset);
static ObjFunction *compile_with_options(VM *vm, ASTNode *ast,
                                         bool module_mode,
                                         const char *name,
                                         int name_len);

/* ========================================================================
 * Scope & Local Variable Management
 * ======================================================================== */
static void begin_scope(void) {
    current->scope_depth++;
}

static void end_scope(int line) {
    /* Close upvalues BEFORE executing defers so that defer closures
       see closed upvalue values */
    int close_from = current->local_count;
    while (close_from > 0 &&
           current->locals[close_from - 1].depth > current->scope_depth) {
        Local *local = &current->locals[close_from - 1];
        if (local->is_captured) {
            emit(ENCODE_ABx(OP_CLOSE_UPVAL, local->reg, 0), line);
        }
        close_from--;
    }

    /* Execute defers for this scope in LIFO order */
    for (int i = current->defer_count - 1; i >= 0; i--) {
        if (current->defers[i].depth == current->scope_depth) {
            compile_node(current->defers[i].expr);
        }
    }
    /* Remove all defers at this depth */
    {
        int write = 0;
        for (int i = 0; i < current->defer_count; i++) {
            if (current->defers[i].depth != current->scope_depth) {
                current->defers[write++] = current->defers[i];
            }
        }
        current->defer_count = write;
    }

    current->scope_depth--;
    while (current->local_count > 0 &&
           current->locals[current->local_count - 1].depth > current->scope_depth) {
        Local *local = &current->locals[current->local_count - 1];
        current->local_count--;
        if (local->reg == current->next_reg - 1) {
            current->next_reg--;
        }
    }
}

static Local *add_local(Token name, int reg, bool is_const) {
    if (current->local_count >= MAX_LOCALS) {
        fprintf(stderr, "Too many local variables.\n");
        return NULL;
    }
    Local *local = &current->locals[current->local_count++];
    local->name = name;
    local->reg = reg;
    local->depth = current->scope_depth;
    local->is_const = is_const;
    local->is_captured = false;
    local->has_known_struct = false;
    local->known_struct = (Token){0};
    return local;
}

static bool identifiers_equal(Token *a, Token *b) {
    return a->length == b->length && memcmp(a->start, b->start, a->length) == 0;
}

static Compiler *get_root_compiler(void) {
    Compiler *c = current;
    while (c->enclosing != NULL) c = c->enclosing;
    return c;
}

static void add_global_str(ObjString* name) {
    Compiler *root = get_root_compiler();
    if (root->global_count >= MAX_CONSTANTS) return;

    for (int i = 0; i < root->global_count; i++) {
        if (name == root->globals[i]) return;
    }

    if (root->global_count >= root->global_capacity) {
        root->global_capacity = root->global_capacity < 16 ? 16 : root->global_capacity * 2;
        root->globals = realloc(root->globals, sizeof(ObjString*) * root->global_capacity);
    }
    root->globals[root->global_count++] = name;
}

static void add_global(Token name) {
    ObjString* str = copy_string(current->vm, name.start, name.length);
    add_global_str(str);
    if (name.length == 4 && memcmp(name.start, "push", 4) == 0) {
        get_root_compiler()->push_shadowed = true;
    } else if (name.length == 8 && memcmp(name.start, "tostring", 8) == 0) {
        get_root_compiler()->tostring_shadowed = true;
    } else if (name.length == 3 && memcmp(name.start, "len", 3) == 0) {
        get_root_compiler()->len_shadowed = true;
    } else if (name.length == 6 && memcmp(name.start, "string", 6) == 0) {
        get_root_compiler()->string_shadowed = true;
    } else if (name.length == 5 && memcmp(name.start, "array", 5) == 0) {
        get_root_compiler()->array_shadowed = true;
    } else if (name.length == 4 && memcmp(name.start, "dict", 4) == 0) {
        get_root_compiler()->dict_shadowed = true;
    }
}

static bool resolve_global(Token *name) {
    Compiler *root = get_root_compiler();
    ObjString* str = copy_string(current->vm, name->start, name->length);
    for (int i = 0; i < root->global_count; i++) {
        if (str == root->globals[i]) return true;
    }
    return false;
}

static void register_builtins(void) {
    const char *builtins[] = {
        "print", "len", "push", "type", "tostring", "tonumber",
        "assert", "error", "input",
        "Err", "Error",
        "math", "string", "array", "dict", "io", "fs", "path",
        "os", "process", "task", "vm", "coroutine",
        "__builtin_print", "__builtin_len", "__builtin_push", "__builtin_type",
        "__builtin_tostring", "__builtin_tonumber",
        "__ffi_bind"
    };
    int count = sizeof(builtins) / sizeof(builtins[0]);
    for (int i = 0; i < count; i++) {
        ObjString* str = copy_string(current->vm, builtins[i], (int)strlen(builtins[i]));
        add_global_str(str);
    }
}

static void register_vm_globals(void) {
    Table *globals = &current->vm->globals;
    for (int i = 0; i < globals->capacity; i++) {
        ObjString *key = globals->entries[i].key;
        if (key != NULL && key != TOMBSTONE_KEY) {
            add_global_str(key);
        }
    }
}

static int resolve_local(Compiler *compiler, Token *name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        if (identifiers_equal(name, &compiler->locals[i].name)) {
            return compiler->locals[i].reg;
        }
    }
    return -1;
}

static Local *resolve_local_entry(Compiler *compiler, Token *name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        if (identifiers_equal(name, &compiler->locals[i].name)) {
            return &compiler->locals[i];
        }
    }
    return NULL;
}

static void update_known_struct_for_local(Token *name, ASTNode *value) {
    Local *local = resolve_local_entry(current, name);
    if (!local || local->reg < 0) return;
    local->has_known_struct = false;
    local->known_struct = (Token){0};
    if (value && value->type == NODE_STRUCT_LITERAL) {
        local->has_known_struct = true;
        local->known_struct = value->as.struct_literal.name;
    }
}

static bool is_push_name(Token *name) {
    return name->length == 4 && memcmp(name->start, "push", 4) == 0;
}

static bool is_tostring_name(Token *name) {
    return name->length == 8 && memcmp(name->start, "tostring", 8) == 0;
}

static bool is_len_name(Token *name) {
    return name->length == 3 && memcmp(name->start, "len", 3) == 0;
}

static bool is_string_name(Token *name) {
    return name->length == 6 && memcmp(name->start, "string", 6) == 0;
}

static bool is_array_name(Token *name) {
    return name->length == 5 && memcmp(name->start, "array", 5) == 0;
}

static bool is_dict_name(Token *name) {
    return name->length == 4 && memcmp(name->start, "dict", 4) == 0;
}

static bool resolve_local_in_chain(Compiler *compiler, Token *name) {
    for (Compiler *c = compiler; c != NULL; c = c->enclosing) {
        if (resolve_local(c, name) != -1) return true;
    }
    return false;
}

static int add_upvalue(Compiler *compiler, int index, bool is_local) {
    int count = compiler->function->upvalue_count;
    /* Check if already captured */
    for (int i = 0; i < count; i++) {
        if (compiler->upvalues[i].index == index &&
            compiler->upvalues[i].is_local == is_local) {
            return i;
        }
    }
    if (count >= MAX_UPVALUES) {
        fprintf(stderr, "Too many upvalues in function.\n");
        return 0;
    }
    compiler->upvalues[count].index = index;
    compiler->upvalues[count].is_local = is_local;
    return compiler->function->upvalue_count++;
}

static int resolve_upvalue(Compiler *compiler, Token *name) {
    if (!compiler->enclosing) return -1;

    /* Check enclosing locals */
    int local = resolve_local(compiler->enclosing, name);
    if (local != -1) {
        compiler->enclosing->locals[local].is_captured = true;
        return add_upvalue(compiler, local, true);
    }

    /* Check enclosing upvalues (recursive) */
    int upvalue = resolve_upvalue(compiler->enclosing, name);
    if (upvalue != -1) {
        return add_upvalue(compiler, upvalue, false);
    }

    return -1;
}

/* ========================================================================
 * Compiler Init
 * ======================================================================== */
static void compiler_init(Compiler *compiler, VM *vm, FunctionType type, Token *name) {
    compiler->enclosing = current;
    compiler->vm = vm;
    compiler->type = type;
    compiler->function = new_function(vm);
    compiler->local_count = 0;
    compiler->scope_depth = 0;
    compiler->next_reg = 0;
    compiler->max_reg = 0;
    compiler->loop_start = -1;
    compiler->break_jumps = NULL;
    compiler->break_count = 0;
    compiler->break_capacity = 0;
    compiler->error_handler_reg = -1;
    compiler->error_jumps = NULL;
    compiler->error_jump_count = 0;
    compiler->error_jump_capacity = 0;
    compiler->defers = NULL;
    compiler->defer_count = 0;
    compiler->defer_capacity = 0;
    compiler->had_error = false;
    compiler->globals = NULL;
    compiler->global_count = 0;
    compiler->global_capacity = 0;
    compiler->inline_candidates = NULL;
    compiler->inline_count = 0;
    compiler->inline_capacity = 0;
    compiler->field_loop_candidates = NULL;
    compiler->field_loop_count = 0;
    compiler->field_loop_capacity = 0;
    compiler->owned_strings = NULL;
    compiler->owned_string_count = 0;
    compiler->owned_string_capacity = 0;
    compiler->self_call_safe = false;
    compiler->module_mode = false;
    compiler->module_exports_reg = -1;
    compiler->push_shadowed = compiler->enclosing ? compiler->enclosing->push_shadowed : false;
    compiler->tostring_shadowed = compiler->enclosing ? compiler->enclosing->tostring_shadowed : false;
    compiler->len_shadowed = compiler->enclosing ? compiler->enclosing->len_shadowed : false;
    compiler->string_shadowed = compiler->enclosing ? compiler->enclosing->string_shadowed : false;
    compiler->array_shadowed = compiler->enclosing ? compiler->enclosing->array_shadowed : false;
    compiler->dict_shadowed = compiler->enclosing ? compiler->enclosing->dict_shadowed : false;

    if (name && name->length > 0) {
        compiler->function->name = copy_string(vm, name->start, name->length);
    }
    if (compiler->enclosing && compiler->enclosing->function->source_name) {
        compiler->function->source_name = compiler->enclosing->function->source_name;
    } else if (name && name->length > 0) {
        compiler->function->source_name = copy_string(vm, name->start, name->length);
    }

    current = compiler;

    /* Reserve register 0 for the function itself in non-script contexts */
    if (type != FUNC_SCRIPT) {
        Token self_token = {TOKEN_IDENTIFIER, "", 0, 0};
        add_local(self_token, alloc_reg(), false);
    }
}

/* ========================================================================
 * Expression Compilation - each returns the register holding the result
 * ======================================================================== */
static int compile_node(ASTNode *node);
static void compile_stmt(ASTNode *node);
static void compile_block(ASTNode *node);

static int compile_number(ASTNode *node) {
    int reg = alloc_reg();
    int k = make_constant(NUMBER_VAL(node->as.number.value));
    emit(ENCODE_ABx(OP_LOADK, reg, k), node->line);
    return reg;
}

static int compile_string(ASTNode *node) {
    int reg = alloc_reg();
    int k = string_constant(current->vm, node->as.string.value, node->as.string.length);
    emit(ENCODE_ABx(OP_LOADK, reg, k), node->line);
    return reg;
}

static int compile_bool(ASTNode *node) {
    int reg = alloc_reg();
    emit(ENCODE_ABC(OP_LOADBOOL, reg, node->as.boolean.value ? 1 : 0, 0), node->line);
    return reg;
}

static int compile_null(ASTNode *node) {
    int reg = alloc_reg();
    emit(ENCODE_ABC(OP_LOADNIL, reg, 0, 0), node->line);
    return reg;
}

static int compile_identifier(ASTNode *node) {
    Token *name = &node->as.identifier.name;

    if (node->as.identifier.is_global) {
        int reg = alloc_reg();
        int k = identifier_constant(current->vm, name);
        emit(ENCODE_ABx(OP_GETGLOBAL, reg, k), node->line);
        return reg;
    }

    /* Try local */
    int local = resolve_local(current, name);
    if (local != -1) return local;

    /* Try upvalue */
    int upval = resolve_upvalue(current, name);
    if (upval != -1) {
        int reg = alloc_reg();
        emit(ENCODE_ABC(OP_GETUPVAL, reg, upval, 0), node->line);
        return reg;
    }

    /* Check if it's a known global */
    if (resolve_global(name)) {
        int reg = alloc_reg();
        int k = identifier_constant(current->vm, name);
        emit(ENCODE_ABx(OP_GETGLOBAL, reg, k), node->line);
        return reg;
    }

    /* Compile error for undefined local identifier */
    error_at(name, "Undefined variable.");
    int reg = alloc_reg();
    int k = identifier_constant(current->vm, name);
    emit(ENCODE_ABx(OP_GETGLOBAL, reg, k), node->line);
    return reg;
}

static bool compile_identifier_into(ASTNode *node, int dest) {
    Token *name = &node->as.identifier.name;

    if (node->as.identifier.is_global) {
        int k = identifier_constant(current->vm, name);
        emit(ENCODE_ABx(OP_GETGLOBAL, dest, k), node->line);
        return true;
    }

    int local = resolve_local(current, name);
    if (local != -1) {
        if (local != dest) {
            emit(ENCODE_ABC(OP_MOVE, dest, local, 0), node->line);
        }
        return true;
    }

    int upval = resolve_upvalue(current, name);
    if (upval != -1) {
        emit(ENCODE_ABC(OP_GETUPVAL, dest, upval, 0), node->line);
        return true;
    }

    if (resolve_global(name)) {
        int k = identifier_constant(current->vm, name);
        emit(ENCODE_ABx(OP_GETGLOBAL, dest, k), node->line);
        return true;
    }

    error_at(name, "Undefined variable.");
    int k = identifier_constant(current->vm, name);
    emit(ENCODE_ABx(OP_GETGLOBAL, dest, k), node->line);
    return true;
}

static bool compile_simple_into(ASTNode *node, int dest) {
    switch (node->type) {
        case NODE_NUMBER: {
            int k = make_constant(NUMBER_VAL(node->as.number.value));
            emit(ENCODE_ABx(OP_LOADK, dest, k), node->line);
            return true;
        }
        case NODE_STRING: {
            int k = string_constant(current->vm, node->as.string.value, node->as.string.length);
            emit(ENCODE_ABx(OP_LOADK, dest, k), node->line);
            return true;
        }
        case NODE_BOOL:
            emit(ENCODE_ABC(OP_LOADBOOL, dest, node->as.boolean.value ? 1 : 0, 0), node->line);
            return true;
        case NODE_NULL:
            emit(ENCODE_ABC(OP_LOADNIL, dest, 0, 0), node->line);
            return true;
        case NODE_IDENTIFIER:
            return compile_identifier_into(node, dest);
        default:
            return false;
    }
}

static bool number_fits_i8(ASTNode *node, int *out) {
    if (node->type != NODE_NUMBER) return false;
    double value = node->as.number.value;
    int int_value = (int)value;
    if ((double)int_value != value || int_value < -128 || int_value > 127) {
        return false;
    }
    *out = int_value;
    return true;
}

static int compile_unary(ASTNode *node) {
    int operand = compile_node(node->as.unary.operand);
    int dest = alloc_reg();

    switch (node->as.unary.op) {
        case TOKEN_MINUS:
            emit(ENCODE_ABC(OP_NEG, dest, operand, 0), node->line);
            break;
        case TOKEN_BANG:
            emit(ENCODE_ABC(OP_NOT, dest, operand, 0), node->line);
            break;
        default:
            break;
    }
    free_temp(operand);
    return dest;
}

static int compile_binary(ASTNode *node) {
    if (node->as.binary.left->type == NODE_NUMBER &&
        node->as.binary.right->type == NODE_NUMBER) {
        double a = node->as.binary.left->as.number.value;
        double b = node->as.binary.right->as.number.value;
        double result;
        switch (node->as.binary.op) {
            case TOKEN_PLUS:    result = a + b; break;
            case TOKEN_MINUS:   result = a - b; break;
            case TOKEN_STAR:    result = a * b; break;
            case TOKEN_SLASH:   result = b != 0.0 ? a / b : 0.0; break;
            case TOKEN_PERCENT: result = b != 0.0 ? fmod(a, b) : 0.0; break;
            default: goto no_fold;
        }
        int reg = alloc_reg();
        int k = make_constant(NUMBER_AUTO_VAL(result));
        emit(ENCODE_ABx(OP_LOADK, reg, k), node->line);
        return reg;
    }
no_fold:;
    int imm_left;
    if (number_fits_i8(node->as.binary.left, &imm_left) &&
        (node->as.binary.op == TOKEN_PLUS || node->as.binary.op == TOKEN_STAR)) {
        int right = compile_node(node->as.binary.right);
        int dest = alloc_reg();
        int encoded = imm_left & 0xff;
        emit(ENCODE_ABC(node->as.binary.op == TOKEN_PLUS ? OP_ADDI : OP_MULI,
                        dest, right, encoded), node->line);
        free_temp(right);
        return dest;
    }

    int left = compile_node(node->as.binary.left);
    int dest = alloc_reg();

    if (node->as.binary.right->type == NODE_NUMBER) {
        int imm;
        if (number_fits_i8(node->as.binary.right, &imm)) {
            int encoded = imm & 0xff;
            switch (node->as.binary.op) {
                case TOKEN_PLUS:        emit(ENCODE_ABC(OP_ADDI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_MINUS:       emit(ENCODE_ABC(OP_SUBI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_STAR:        emit(ENCODE_ABC(OP_MULI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_SLASH:
                    if (imm == 2 || imm == -2) {
                        int k = make_constant(NUMBER_VAL(1.0 / (double)imm));
                        if (k <= 255) {
                            emit(ENCODE_ABC(OP_MULK, dest, left, k), node->line);
                            free_temp(left);
                            return dest;
                        }
                    }
                    emit(ENCODE_ABC(OP_DIVI, dest, left, encoded), node->line);
                    free_temp(left);
                    return dest;
                case TOKEN_PERCENT:     emit(ENCODE_ABC(OP_MODI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_EQUAL_EQUAL: emit(ENCODE_ABC(OP_EQI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_BANG_EQUAL:  emit(ENCODE_ABC(OP_NEQI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_LESS:        emit(ENCODE_ABC(OP_LTI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_LESS_EQUAL:  emit(ENCODE_ABC(OP_LEI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_GREATER:     emit(ENCODE_ABC(OP_GTI, dest, left, encoded), node->line); free_temp(left); return dest;
                case TOKEN_GREATER_EQUAL: emit(ENCODE_ABC(OP_GEI, dest, left, encoded), node->line); free_temp(left); return dest;
                default: break;
            }
        }
        int k = make_constant(NUMBER_VAL(node->as.binary.right->as.number.value));
        if (k > 255) {
            goto compile_right_register;
        }
        switch (node->as.binary.op) {
            case TOKEN_PLUS:    emit(ENCODE_ABC(OP_ADDK, dest, left, k), node->line); free_temp(left); return dest;
            case TOKEN_MINUS:   emit(ENCODE_ABC(OP_SUBK, dest, left, k), node->line); free_temp(left); return dest;
            case TOKEN_STAR:    emit(ENCODE_ABC(OP_MULK, dest, left, k), node->line); free_temp(left); return dest;
            case TOKEN_SLASH:   emit(ENCODE_ABC(OP_DIVK, dest, left, k), node->line); free_temp(left); return dest;
            case TOKEN_PERCENT: emit(ENCODE_ABC(OP_MODK, dest, left, k), node->line); free_temp(left); return dest;
            default: break;
        }
    }

compile_right_register:
    int right = compile_node(node->as.binary.right);
    switch (node->as.binary.op) {
        case TOKEN_PLUS:
            emit(ENCODE_ABC(OP_ADD, dest, left, right), node->line);
            break;
        case TOKEN_MINUS:
            emit(ENCODE_ABC(OP_SUB, dest, left, right), node->line);
            break;
        case TOKEN_STAR:
            emit(ENCODE_ABC(OP_MUL, dest, left, right), node->line);
            break;
        case TOKEN_SLASH:
            emit(ENCODE_ABC(OP_DIV, dest, left, right), node->line);
            break;
        case TOKEN_PERCENT:
            emit(ENCODE_ABC(OP_MOD, dest, left, right), node->line);
            break;
        case TOKEN_EQUAL_EQUAL:
            emit(ENCODE_ABC(OP_EQ, dest, left, right), node->line);
            break;
        case TOKEN_BANG_EQUAL:
            emit(ENCODE_ABC(OP_NEQ, dest, left, right), node->line);
            break;
        case TOKEN_LESS:
            emit(ENCODE_ABC(OP_LT, dest, left, right), node->line);
            break;
        case TOKEN_LESS_EQUAL:
            emit(ENCODE_ABC(OP_LE, dest, left, right), node->line);
            break;
        case TOKEN_GREATER:
            emit(ENCODE_ABC(OP_LT, dest, right, left), node->line);
            break;
        case TOKEN_GREATER_EQUAL:
            emit(ENCODE_ABC(OP_LE, dest, right, left), node->line);
            break;
        case TOKEN_DOT_DOT:
        case TOKEN_DOT_DOT_EQUAL:
            break;
        default:
            break;
    }
    free_temp(right);
    free_temp(left);
    return dest;
}

static bool compile_binary_into(ASTNode *node, int dest) {
    int imm_left;
    if (number_fits_i8(node->as.binary.left, &imm_left) &&
        (node->as.binary.op == TOKEN_PLUS || node->as.binary.op == TOKEN_STAR)) {
        int right = compile_node(node->as.binary.right);
        emit(ENCODE_ABC(node->as.binary.op == TOKEN_PLUS ? OP_ADDI : OP_MULI,
                        dest, right, imm_left & 0xff), node->line);
        free_temp(right);
        return true;
    }

    /* Fuse (a - b) + c → SUBADD or (a + b) - c → ADDSUB */
    if (current->error_handler_reg < 0 &&
        node->as.binary.op == TOKEN_PLUS &&
        node->as.binary.left->type == NODE_BINARY &&
        node->as.binary.left->as.binary.op == TOKEN_MINUS) {
        int a_reg = compile_node(node->as.binary.left->as.binary.left);
        int b_reg = compile_node(node->as.binary.left->as.binary.right);
        int c_reg = compile_node(node->as.binary.right);
        emit(ENCODE_ABC(OP_SUBADD, dest, a_reg, b_reg), node->line);
        emit(ENCODE_ABx(OP_AUX, 0, c_reg), node->line);
        free_temp(c_reg);
        free_temp(b_reg);
        free_temp(a_reg);
        return true;
    }
    if (current->error_handler_reg < 0 &&
        node->as.binary.op == TOKEN_MINUS &&
        node->as.binary.left->type == NODE_BINARY &&
        node->as.binary.left->as.binary.op == TOKEN_PLUS) {
        int a_reg = compile_node(node->as.binary.left->as.binary.left);
        int b_reg = compile_node(node->as.binary.left->as.binary.right);
        int c_reg = compile_node(node->as.binary.right);
        emit(ENCODE_ABC(OP_ADDSUB, dest, a_reg, b_reg), node->line);
        emit(ENCODE_ABx(OP_AUX, 0, c_reg), node->line);
        free_temp(c_reg);
        free_temp(b_reg);
        free_temp(a_reg);
        return true;
    }

    /* Fuse (a * b) + c → MULADD or (a * b) - c → MULSUB */
    if (current->error_handler_reg < 0 &&
        (node->as.binary.op == TOKEN_PLUS || node->as.binary.op == TOKEN_MINUS) &&
        node->as.binary.left->type == NODE_BINARY &&
        node->as.binary.left->as.binary.op == TOKEN_STAR) {
        int a_reg = compile_node(node->as.binary.left->as.binary.left);
        int b_reg = compile_node(node->as.binary.left->as.binary.right);
        int c_reg = compile_node(node->as.binary.right);
        emit(ENCODE_ABC(node->as.binary.op == TOKEN_PLUS ? OP_MULADD : OP_MULSUB,
                        dest, a_reg, b_reg), node->line);
        emit(ENCODE_ABx(OP_AUX, 0, c_reg), node->line);
        free_temp(c_reg);
        free_temp(b_reg);
        free_temp(a_reg);
        return true;
    }

    int left = compile_node(node->as.binary.left);

    if (node->as.binary.right->type == NODE_NUMBER) {
        int imm;
        if (number_fits_i8(node->as.binary.right, &imm)) {
            int encoded = imm & 0xff;
            switch (node->as.binary.op) {
                case TOKEN_PLUS:        emit(ENCODE_ABC(OP_ADDI, dest, left, encoded), node->line); free_temp(left); return true;
                case TOKEN_MINUS:       emit(ENCODE_ABC(OP_SUBI, dest, left, encoded), node->line); free_temp(left); return true;
                case TOKEN_STAR:        emit(ENCODE_ABC(OP_MULI, dest, left, encoded), node->line); free_temp(left); return true;
                case TOKEN_SLASH:
                    if (imm == 2 || imm == -2) {
                        int k = make_constant(NUMBER_VAL(1.0 / (double)imm));
                        if (k <= 255) {
                            emit(ENCODE_ABC(OP_MULK, dest, left, k), node->line);
                            free_temp(left);
                            return true;
                        }
                    }
                    emit(ENCODE_ABC(OP_DIVI, dest, left, encoded), node->line);
                    free_temp(left);
                    return true;
                case TOKEN_PERCENT:     emit(ENCODE_ABC(OP_MODI, dest, left, encoded), node->line); free_temp(left); return true;
                default: break;
            }
        }

        int k = make_constant(NUMBER_VAL(node->as.binary.right->as.number.value));
        if (k <= 255) {
            switch (node->as.binary.op) {
                case TOKEN_PLUS:    emit(ENCODE_ABC(OP_ADDK, dest, left, k), node->line); free_temp(left); return true;
                case TOKEN_MINUS:   emit(ENCODE_ABC(OP_SUBK, dest, left, k), node->line); free_temp(left); return true;
                case TOKEN_STAR:    emit(ENCODE_ABC(OP_MULK, dest, left, k), node->line); free_temp(left); return true;
                case TOKEN_SLASH:   emit(ENCODE_ABC(OP_DIVK, dest, left, k), node->line); free_temp(left); return true;
                case TOKEN_PERCENT: emit(ENCODE_ABC(OP_MODK, dest, left, k), node->line); free_temp(left); return true;
                default: break;
            }
        }
    }

    int right = compile_node(node->as.binary.right);

    switch (node->as.binary.op) {
        case TOKEN_PLUS:          emit(ENCODE_ABC(OP_ADD, dest, left, right), node->line); break;
        case TOKEN_MINUS:         emit(ENCODE_ABC(OP_SUB, dest, left, right), node->line); break;
        case TOKEN_STAR:          emit(ENCODE_ABC(OP_MUL, dest, left, right), node->line); break;
        case TOKEN_SLASH:         emit(ENCODE_ABC(OP_DIV, dest, left, right), node->line); break;
        case TOKEN_PERCENT:       emit(ENCODE_ABC(OP_MOD, dest, left, right), node->line); break;
        case TOKEN_EQUAL_EQUAL:   emit(ENCODE_ABC(OP_EQ, dest, left, right), node->line); break;
        case TOKEN_BANG_EQUAL:    emit(ENCODE_ABC(OP_NEQ, dest, left, right), node->line); break;
        case TOKEN_LESS:          emit(ENCODE_ABC(OP_LT, dest, left, right), node->line); break;
        case TOKEN_LESS_EQUAL:    emit(ENCODE_ABC(OP_LE, dest, left, right), node->line); break;
        case TOKEN_GREATER:       emit(ENCODE_ABC(OP_LT, dest, right, left), node->line); break;
        case TOKEN_GREATER_EQUAL: emit(ENCODE_ABC(OP_LE, dest, right, left), node->line); break;
        default:
            free_temp(right);
            free_temp(left);
            return false;
    }
    free_temp(right);
    free_temp(left);
    return true;
}

static bool compile_node_into_reg(ASTNode *node, int dest) {
    switch (node->type) {
        case NODE_NUMBER:
        case NODE_STRING:
        case NODE_BOOL:
        case NODE_NULL:
        case NODE_IDENTIFIER:
            return compile_simple_into(node, dest);
        case NODE_UNARY: {
            int operand = compile_node(node->as.unary.operand);
            switch (node->as.unary.op) {
                case TOKEN_MINUS:
                    emit(ENCODE_ABC(OP_NEG, dest, operand, 0), node->line);
                    break;
                case TOKEN_BANG:
                    emit(ENCODE_ABC(OP_NOT, dest, operand, 0), node->line);
                    break;
                default:
                    free_temp(operand);
                    return false;
            }
            free_temp(operand);
            return true;
        }
        case NODE_BINARY:
            return compile_binary_into(node, dest);
        default:
            return false;
    }
}

static int compile_logical(ASTNode *node) {
    int left = compile_node(node->as.logical.left);
    int dest = alloc_reg();

    /* Use OP_TESTSET: if test passes, copies left to dest and skips JMP;
       otherwise falls through to compile the right operand */
    if (node->as.logical.op == TOKEN_OR) {
        /* OR: if left is truthy, result = left (skip right) */
        emit(ENCODE_ABC(OP_TESTSET, dest, left, 1), node->line);
    } else {
        /* AND: if left is falsey, result = left (skip right) */
        emit(ENCODE_ABC(OP_TESTSET, dest, left, 0), node->line);
    }
    int jump = emit_jump(OP_JMP, node->line);

    int right = compile_node(node->as.logical.right);
    emit(ENCODE_ABC(OP_MOVE, dest, right, 0), node->line);

    patch_jump(jump);
    return dest;
}

static JumpPatch compile_condition_jump(ASTNode *condition, int line) {
    if (condition->type == NODE_BINARY &&
        (condition->as.binary.op == TOKEN_EQUAL_EQUAL ||
         condition->as.binary.op == TOKEN_BANG_EQUAL) &&
        condition->as.binary.left->type == NODE_BINARY &&
        condition->as.binary.left->as.binary.op == TOKEN_PERCENT) {
        int divisor;
        int expected;
        ASTNode *mod_expr = condition->as.binary.left;
        if (number_fits_i8(mod_expr->as.binary.right, &divisor) &&
            number_fits_i8(condition->as.binary.right, &expected)) {
            int value = compile_node(mod_expr->as.binary.left);
            int jump = emit_immediate_test_jump(
                condition->as.binary.op == TOKEN_EQUAL_EQUAL ? OP_MODI_EQI_TEST : OP_MODI_NEQI_TEST,
                value,
                divisor,
                line);
            current_chunk()->code[current_chunk()->count - 2] |= ((uint32_t)(expected & 0xff)) << 24;
            free_temp(value);
            return (JumpPatch){jump, true};
        }
    }

    /* Fuse (a + b) > N or (a - b) > N into single test instruction */
    if (condition->type == NODE_BINARY &&
        condition->as.binary.left->type == NODE_BINARY &&
        (condition->as.binary.left->as.binary.op == TOKEN_PLUS ||
         condition->as.binary.left->as.binary.op == TOKEN_MINUS) &&
        condition->as.binary.right->type == NODE_NUMBER &&
        current->error_handler_reg < 0) {
        int imm;
        if (number_fits_i8(condition->as.binary.right, &imm)) {
            bool is_add = (condition->as.binary.left->as.binary.op == TOKEN_PLUS);
            OpCode op = OP_COUNT;
            if (is_add && condition->as.binary.op == TOKEN_GREATER)       op = OP_ADD_GT_TEST;
            else if (is_add && condition->as.binary.op == TOKEN_GREATER_EQUAL) op = OP_ADD_GE_TEST;
            else if (!is_add && condition->as.binary.op == TOKEN_GREATER) op = OP_SUB_GT_TEST;
            else if (!is_add && condition->as.binary.op == TOKEN_GREATER_EQUAL) op = OP_SUB_GE_TEST;
            if (op != OP_COUNT) {
                int a = compile_node(condition->as.binary.left->as.binary.left);
                int b = compile_node(condition->as.binary.left->as.binary.right);
                emit(ENCODE_ABC(op, a, b, imm & 0xff), line);
                int jump = emit_jump(OP_JMP, line);
                free_temp(b);
                free_temp(a);
                return (JumpPatch){jump, true};
            }
        }
    }

    if (condition->type == NODE_BINARY &&
        condition->as.binary.right->type == NODE_NUMBER) {
        int imm;
        if (number_fits_i8(condition->as.binary.right, &imm)) {
            OpCode op;
            switch (condition->as.binary.op) {
                case TOKEN_EQUAL_EQUAL:   op = OP_EQI_TEST; break;
                case TOKEN_BANG_EQUAL:    op = OP_NEQI_TEST; break;
                case TOKEN_LESS:          op = OP_LTI_TEST; break;
                case TOKEN_LESS_EQUAL:    op = OP_LEI_TEST; break;
                case TOKEN_GREATER:       op = OP_GTI_TEST; break;
                case TOKEN_GREATER_EQUAL: op = OP_GEI_TEST; break;
                default:                  op = OP_COUNT; break;
            }
            if (op != OP_COUNT) {
                int left = compile_node(condition->as.binary.left);
                int jump = emit_immediate_test_jump(op, left, imm, line);
                free_temp(left);
                return (JumpPatch){jump, true};
            }
        }
    }

    int cond = compile_node(condition);
    return (JumpPatch){emit_test_jump(cond, line), false};
}

static void patch_condition_jump(JumpPatch jump) {
    if (jump.is_plain_jump) {
        patch_jump(jump.offset);
    } else {
        patch_test_jump(jump.offset);
    }
}

static bool inline_name_is_param(ASTNode *fn_decl, Token *name) {
    for (int i = 0; i < fn_decl->as.fn_decl.param_count; i++) {
        if (identifiers_equal(name, &fn_decl->as.fn_decl.params[i].name)) {
            return true;
        }
    }
    return false;
}

static bool inline_expr_is_safe(ASTNode *fn_decl, ASTNode *expr) {
    if (!expr) return false;
    switch (expr->type) {
        case NODE_NUMBER:
        case NODE_STRING:
        case NODE_BOOL:
        case NODE_NULL:
            return true;
        case NODE_IDENTIFIER:
            return inline_name_is_param(fn_decl, &expr->as.identifier.name);
        case NODE_UNARY:
            return inline_expr_is_safe(fn_decl, expr->as.unary.operand);
        case NODE_BINARY:
            return inline_expr_is_safe(fn_decl, expr->as.binary.left) &&
                   inline_expr_is_safe(fn_decl, expr->as.binary.right);
        case NODE_LOGICAL:
            return inline_expr_is_safe(fn_decl, expr->as.logical.left) &&
                   inline_expr_is_safe(fn_decl, expr->as.logical.right);
        default:
            return false;
    }
}

static ASTNode *inline_return_expr(ASTNode *fn_decl) {
    if (!fn_decl || fn_decl->type != NODE_FN_DECL || fn_decl->as.fn_decl.is_method) return NULL;
    if (fn_decl->as.fn_decl.param_count < 0 || fn_decl->as.fn_decl.param_count > 8) return NULL;

    ASTNode *body = fn_decl->as.fn_decl.body;
    if (!body || body->type != NODE_BLOCK || body->as.block.stmts.count != 1) return NULL;

    ASTNode *stmt = body->as.block.stmts.nodes[0];
    if (!stmt || stmt->type != NODE_RETURN || stmt->as.return_stmt.values.count != 1) return NULL;

    ASTNode *expr = stmt->as.return_stmt.values.nodes[0];
    return inline_expr_is_safe(fn_decl, expr) ? expr : NULL;
}

static void register_inline_candidate(ASTNode *fn_decl, int binding_reg) {
    if (current != get_root_compiler() || current->scope_depth != 0) return;
    if (binding_reg < 0 || !inline_return_expr(fn_decl)) return;

    Compiler *root = get_root_compiler();
    if (root->inline_count >= root->inline_capacity) {
        root->inline_capacity = root->inline_capacity < 8 ? 8 : root->inline_capacity * 2;
        root->inline_candidates = realloc(root->inline_candidates,
            sizeof(InlineCandidate) * root->inline_capacity);
    }

    root->inline_candidates[root->inline_count++] = (InlineCandidate){
        fn_decl->as.fn_decl.name,
        fn_decl,
        binding_reg
    };
}

static InlineCandidate *find_inline_candidate(Token *name) {
    Compiler *root = get_root_compiler();
    if (current != root) return NULL;

    int resolved = resolve_local(current, name);
    for (int i = root->inline_count - 1; i >= 0; i--) {
        InlineCandidate *candidate = &root->inline_candidates[i];
        if (resolved == candidate->binding_reg &&
            identifiers_equal(name, &candidate->name)) {
            return candidate;
        }
    }
    return NULL;
}

static bool compiler_own_string(char *value) {
    if (!value) return false;
    if (current->owned_string_count >= current->owned_string_capacity) {
        int old_capacity = current->owned_string_capacity;
        int new_capacity = old_capacity < 4 ? 4 : old_capacity * 2;
        char **items = (char **)realloc(current->owned_strings, sizeof(char *) * (size_t)new_capacity);
        if (!items) return false;
        current->owned_strings = items;
        current->owned_string_capacity = new_capacity;
    }
    current->owned_strings[current->owned_string_count++] = value;
    return true;
}

static void free_compiler_owned_strings(Compiler *compiler) {
    for (int i = 0; i < compiler->owned_string_count; i++) {
        free(compiler->owned_strings[i]);
    }
    free(compiler->owned_strings);
}

static int method_param_arg_index(ASTNode *fn_decl, Token *name) {
    for (int i = 1; i < fn_decl->as.fn_decl.param_count; i++) {
        if (identifiers_equal(name, &fn_decl->as.fn_decl.params[i].name)) {
            return i - 1;
        }
    }
    return -1;
}

static bool match_field_increment_stmt(ASTNode *fn_decl, ASTNode *stmt,
                                       Token *self_name, Token *field_name,
                                       int *arg_index) {
    if (stmt && stmt->type == NODE_EXPRESSION_STMT) {
        stmt = stmt->as.expr_stmt.expr;
    }
    if (!stmt || stmt->type != NODE_FIELD_SET) return false;

    ASTNode *object = stmt->as.field_set.object;
    if (!object || object->type != NODE_IDENTIFIER ||
        object->as.identifier.is_global ||
        !identifiers_equal(&object->as.identifier.name, self_name)) {
        return false;
    }
    *field_name = stmt->as.field_set.name;

    ASTNode *value = stmt->as.field_set.value;
    if (!value || value->type != NODE_BINARY || value->as.binary.op != TOKEN_PLUS) {
        return false;
    }
    ASTNode *left = value->as.binary.left;
    ASTNode *right = value->as.binary.right;
    if (!left || left->type != NODE_FIELD_GET ||
        !identifiers_equal(&left->as.field_get.name, field_name) ||
        left->as.field_get.object->type != NODE_IDENTIFIER ||
        left->as.field_get.object->as.identifier.is_global ||
        !identifiers_equal(&left->as.field_get.object->as.identifier.name, self_name)) {
        return false;
    }
    if (!right || right->type != NODE_IDENTIFIER || right->as.identifier.is_global) {
        return false;
    }

    *arg_index = method_param_arg_index(fn_decl, &right->as.identifier.name);
    return *arg_index >= 0;
}

static void register_field_loop_candidate(ASTNode *fn_decl) {
    if (current != get_root_compiler() || current->scope_depth != 0 ||
        !fn_decl->as.fn_decl.is_method ||
        fn_decl->as.fn_decl.param_count < 2) {
        return;
    }

    ASTNode *body = fn_decl->as.fn_decl.body;
    if (!body || body->type != NODE_BLOCK || body->as.block.stmts.count != 2) return;

    FieldLoopCandidate candidate;
    memset(&candidate, 0, sizeof(candidate));
    candidate.struct_name = fn_decl->as.fn_decl.method_struct;
    candidate.method_name = fn_decl->as.fn_decl.name;
    candidate.update_count = 2;

    Token *self_name = &fn_decl->as.fn_decl.params[0].name;
    if (!match_field_increment_stmt(fn_decl, body->as.block.stmts.nodes[0],
                                    self_name, &candidate.field_names[0],
                                    &candidate.param_indices[0]) ||
        !match_field_increment_stmt(fn_decl, body->as.block.stmts.nodes[1],
                                    self_name, &candidate.field_names[1],
                                    &candidate.param_indices[1])) {
        return;
    }

    Compiler *root = get_root_compiler();
    if (root->field_loop_count >= root->field_loop_capacity) {
        root->field_loop_capacity = root->field_loop_capacity < 4 ? 4 : root->field_loop_capacity * 2;
        root->field_loop_candidates = realloc(root->field_loop_candidates,
            sizeof(FieldLoopCandidate) * root->field_loop_capacity);
    }
    root->field_loop_candidates[root->field_loop_count++] = candidate;
}

static FieldLoopCandidate *find_field_loop_candidate(Token *struct_name, Token *method_name) {
    Compiler *root = get_root_compiler();
    if (current != root) return NULL;
    for (int i = root->field_loop_count - 1; i >= 0; i--) {
        FieldLoopCandidate *candidate = &root->field_loop_candidates[i];
        if (identifiers_equal(struct_name, &candidate->struct_name) &&
            identifiers_equal(method_name, &candidate->method_name)) {
            return candidate;
        }
    }
    return NULL;
}

static int compile_inline_call(ASTNode *node, InlineCandidate *candidate) {
    ASTNode *fn_decl = candidate->fn_decl;
    int arg_count = node->as.call.args.count;
    if (arg_count != fn_decl->as.fn_decl.param_count) return -1;

    int arg_regs[8];
    for (int i = 0; i < arg_count; i++) {
        arg_regs[i] = compile_node(node->as.call.args.nodes[i]);
    }

    int saved_local_count = current->local_count;
    for (int i = 0; i < arg_count; i++) {
        add_local(fn_decl->as.fn_decl.params[i].name, arg_regs[i], true);
    }

    int result = compile_node(inline_return_expr(fn_decl));
    current->local_count = saved_local_count;
    return result;
}

static bool token_matches_string(Token *token, ObjString *string) {
    return string &&
           token->length == string->length &&
           memcmp(token->start, string->chars, token->length) == 0;
}

static bool call_args_are_safe(NodeList *args);

static bool call_arg_is_safe(ASTNode *expr) {
    if (!expr) return false;
    switch (expr->type) {
        case NODE_NUMBER:
        case NODE_STRING:
        case NODE_BOOL:
        case NODE_NULL:
        case NODE_IDENTIFIER:
            return true;
        case NODE_UNARY:
            return call_arg_is_safe(expr->as.unary.operand);
        case NODE_BINARY:
            return call_arg_is_safe(expr->as.binary.left) &&
                   call_arg_is_safe(expr->as.binary.right);
        case NODE_LOGICAL:
            return call_arg_is_safe(expr->as.logical.left) &&
                   call_arg_is_safe(expr->as.logical.right);
        case NODE_INDEX:
            return call_arg_is_safe(expr->as.index_expr.object) &&
                   call_arg_is_safe(expr->as.index_expr.index);
        case NODE_TRY:
            return false;
        case NODE_TRY_BLOCK:
            return false;
        case NODE_FIELD_GET:
            return call_arg_is_safe(expr->as.field_get.object);
        case NODE_CALL:
            return false;
        default:
            return false;
    }
}

static bool call_args_are_safe(NodeList *args) {
    for (int i = 0; i < args->count; i++) {
        if (!call_arg_is_safe(args->nodes[i])) return false;
    }
    return true;
}

static bool expr_references_name(ASTNode *expr, Token *name) {
    if (!expr) return false;
    switch (expr->type) {
        case NODE_IDENTIFIER:
            return !expr->as.identifier.is_global &&
                   identifiers_equal(&expr->as.identifier.name, name);
        case NODE_UNARY:
            return expr_references_name(expr->as.unary.operand, name);
        case NODE_BINARY:
            return expr_references_name(expr->as.binary.left, name) ||
                   expr_references_name(expr->as.binary.right, name);
        case NODE_LOGICAL:
            return expr_references_name(expr->as.logical.left, name) ||
                   expr_references_name(expr->as.logical.right, name);
        case NODE_INDEX:
            return expr_references_name(expr->as.index_expr.object, name) ||
                   expr_references_name(expr->as.index_expr.index, name);
        case NODE_TRY:
            return expr_references_name(expr->as.try_expr.expr, name);
        case NODE_TRY_BLOCK:
            return expr_references_name(expr->as.try_block.body, name);
        case NODE_TRY_CATCH:
            return expr_references_name(expr->as.try_catch.body, name) ||
                   expr_references_name(expr->as.try_catch.catch_body, name);
        case NODE_FIELD_GET:
            return expr_references_name(expr->as.field_get.object, name);
        default:
            return false;
    }
}

static bool compile_local_update_fused_term(ASTNode *expr, MgTokenType update_op,
                                            int target_reg) {
    if (expr && expr->type == NODE_CALL && current->error_handler_reg < 0 &&
        (update_op == TOKEN_PLUS || update_op == TOKEN_MINUS) &&
        expr->as.call.args.count == 1 &&
        expr->as.call.callee->type == NODE_IDENTIFIER &&
        is_len_name(&expr->as.call.callee->as.identifier.name) &&
        !get_root_compiler()->len_shadowed &&
        !resolve_local_in_chain(current, &expr->as.call.callee->as.identifier.name)) {
        int value_reg = compile_node(expr->as.call.args.nodes[0]);
        emit(ENCODE_ABC(update_op == TOKEN_PLUS ? OP_ADDLOCAL_LEN : OP_SUBLOCAL_LEN,
                        target_reg, value_reg, 0), expr->line);
        free_temp(value_reg);
        return true;
    }

    if (expr && expr->type == NODE_TRY && current->error_handler_reg < 0 &&
        (update_op == TOKEN_PLUS || update_op == TOKEN_MINUS)) {
        ASTNode *inner = expr->as.try_expr.expr;
        if (inner->type == NODE_INDEX &&
            inner->as.index_expr.index->type == NODE_STRING) {
            int obj = compile_node(inner->as.index_expr.object);
            ASTNode *key = inner->as.index_expr.index;
            int field_k = string_constant(current->vm, key->as.string.value, key->as.string.length);
            reserve_reg_index(target_reg + 1);
            emit(ENCODE_ABC(update_op == TOKEN_PLUS ? OP_ADDLOCAL_FIELD_PROP
                                                    : OP_SUBLOCAL_FIELD_PROP,
                            target_reg, obj, field_k), expr->line);
            free_temp(obj);
            return true;
        }
    }

    if (!expr || expr->type != NODE_BINARY || expr->as.binary.right->type != NODE_NUMBER) {
        if (expr && expr->type == NODE_NUMBER &&
            (update_op == TOKEN_PLUS || update_op == TOKEN_MINUS)) {
            int imm;
            if (number_fits_i8(expr, &imm)) {
                emit(ENCODE_ABC(update_op == TOKEN_PLUS ? OP_ADDI : OP_SUBI,
                                target_reg, target_reg, imm & 0xff), expr->line);
                return true;
            }
            int k = make_constant(NUMBER_VAL(expr->as.number.value));
            if (k <= 255) {
                emit(ENCODE_ABC(update_op == TOKEN_PLUS ? OP_ADDK : OP_SUBK,
                                target_reg, target_reg, k), expr->line);
                return true;
            }
        }
        return false;
    }

    OpCode op = OP_COUNT;
    int c = 0;
    int imm;
    MgTokenType term_op = expr->as.binary.op;

    if (number_fits_i8(expr->as.binary.right, &imm)) {
        int encoded = imm & 0xff;
        if (term_op == TOKEN_STAR) {
            op = update_op == TOKEN_PLUS ? OP_ADDLOCAL_MULI : OP_SUBLOCAL_MULI;
            c = encoded;
        } else if (term_op == TOKEN_PERCENT) {
            op = update_op == TOKEN_PLUS ? OP_ADDLOCAL_MODI : OP_SUBLOCAL_MODI;
            c = encoded;
        } else if (term_op == TOKEN_SLASH) {
            if (imm == 2 || imm == -2) {
                int k = make_constant(NUMBER_VAL(1.0 / (double)imm));
                if (k > 255) return false;
                op = update_op == TOKEN_PLUS ? OP_ADDLOCAL_MULK : OP_SUBLOCAL_MULK;
                c = k;
            } else {
                op = update_op == TOKEN_PLUS ? OP_ADDLOCAL_DIVI : OP_SUBLOCAL_DIVI;
                c = encoded;
            }
        }
    } else if (term_op == TOKEN_STAR) {
        int k = make_constant(NUMBER_VAL(expr->as.binary.right->as.number.value));
        if (k > 255) return false;
        op = update_op == TOKEN_PLUS ? OP_ADDLOCAL_MULK : OP_SUBLOCAL_MULK;
        c = k;
    }

    if (op == OP_COUNT) {
        /* Handle local += reg * reg (both operands non-constant) */
        if ((update_op == TOKEN_PLUS || update_op == TOKEN_MINUS) &&
            term_op == TOKEN_STAR &&
            current->error_handler_reg < 0) {
            int a = compile_node(expr->as.binary.left);
            int b = compile_node(expr->as.binary.right);
            emit(ENCODE_ABC(update_op == TOKEN_PLUS ? OP_MULLOCAL_ADD : OP_MULLOCAL_SUB,
                            target_reg, a, b), expr->line);
            free_temp(b);
            free_temp(a);
            return true;
        }
        return false;
    }

    int source = compile_node(expr->as.binary.left);
    emit(ENCODE_ABC(op, target_reg, source, c), expr->line);
    free_temp(source);
    return true;
}

static bool local_update_rhs_is_safe(ASTNode *expr, Token *target) {
    if (!expr_references_name(expr, target) && call_arg_is_safe(expr)) {
        return true;
    }
    if (expr && expr->type == NODE_CALL &&
        expr->as.call.args.count == 1 &&
        expr->as.call.callee->type == NODE_IDENTIFIER &&
        is_len_name(&expr->as.call.callee->as.identifier.name) &&
        !get_root_compiler()->len_shadowed &&
        !resolve_local_in_chain(current, &expr->as.call.callee->as.identifier.name) &&
        !expr_references_name(expr->as.call.args.nodes[0], target) &&
        call_arg_is_safe(expr->as.call.args.nodes[0])) {
        return true;
    }
    if (!expr || expr->type != NODE_TRY || current->error_handler_reg >= 0) {
        return false;
    }
    ASTNode *inner = expr->as.try_expr.expr;
    return inner->type == NODE_INDEX &&
           inner->as.index_expr.index->type == NODE_STRING &&
           !expr_references_name(inner, target) &&
           call_arg_is_safe(inner->as.index_expr.object);
}

static bool compile_local_update_chain(ASTNode *expr, Token *target, int target_reg, bool dry_run) {
    if (!expr) return false;
    if (expr->type == NODE_IDENTIFIER &&
        !expr->as.identifier.is_global &&
        identifiers_equal(&expr->as.identifier.name, target)) {
        return true;
    }

    if (expr->type != NODE_BINARY ||
        (expr->as.binary.op != TOKEN_PLUS && expr->as.binary.op != TOKEN_MINUS)) {
        return false;
    }

    if (!compile_local_update_chain(expr->as.binary.left, target, target_reg, dry_run)) {
        return false;
    }
    if (!local_update_rhs_is_safe(expr->as.binary.right, target)) {
        return false;
    }

    if (!dry_run) {
        if (!compile_local_update_fused_term(expr->as.binary.right,
                                             expr->as.binary.op,
                                             target_reg)) {
            int rhs = compile_node(expr->as.binary.right);
            emit(ENCODE_ABC(expr->as.binary.op == TOKEN_PLUS ? OP_ADDLOCAL : OP_SUBLOCAL,
                            target_reg, rhs, 0), expr->line);
            free_temp(rhs);
        }
    }
    return true;
}

static bool local_update_term_can_fuse(ASTNode *expr) {
    if (!expr || expr->type != NODE_BINARY || expr->as.binary.right->type != NODE_NUMBER) {
        return false;
    }

    int imm;
    switch (expr->as.binary.op) {
        case TOKEN_STAR:
            return true;
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            return number_fits_i8(expr->as.binary.right, &imm);
        default:
            return false;
    }
}

static bool local_update_chain_has_fused_term(ASTNode *expr, Token *target) {
    if (!expr) return false;
    if (expr->type == NODE_IDENTIFIER &&
        !expr->as.identifier.is_global &&
        identifiers_equal(&expr->as.identifier.name, target)) {
        return false;
    }
    if (expr->type != NODE_BINARY ||
        (expr->as.binary.op != TOKEN_PLUS && expr->as.binary.op != TOKEN_MINUS)) {
        return false;
    }
    return local_update_chain_has_fused_term(expr->as.binary.left, target) ||
           local_update_term_can_fuse(expr->as.binary.right);
}

static ASTNode *single_block_statement(ASTNode *body) {
    if (!body || body->type != NODE_BLOCK || body->as.block.stmts.count != 1) return NULL;
    return body->as.block.stmts.nodes[0];
}

static bool match_accum_delta_branch(ASTNode *branch, Token **target_name, int *delta) {
    ASTNode *stmt = single_block_statement(branch);
    if (!stmt) return false;
    if (stmt->type == NODE_EXPRESSION_STMT) {
        stmt = stmt->as.expr_stmt.expr;
    }
    if (!stmt || stmt->type != NODE_ASSIGN ||
        stmt->as.assign.target->type != NODE_IDENTIFIER ||
        stmt->as.assign.target->as.identifier.is_global) {
        return false;
    }

    Token *name = &stmt->as.assign.target->as.identifier.name;
    if (*target_name && !identifiers_equal(*target_name, name)) return false;

    ASTNode *value = stmt->as.assign.value;
    if (!value || value->type != NODE_BINARY || value->as.binary.op != TOKEN_PLUS) {
        return false;
    }
    if (value->as.binary.left->type != NODE_IDENTIFIER ||
        value->as.binary.left->as.identifier.is_global ||
        !identifiers_equal(&value->as.binary.left->as.identifier.name, name)) {
        return false;
    }
    if (value->as.binary.right->type != NODE_NUMBER ||
        !number_fits_i8(value->as.binary.right, delta)) {
        return false;
    }

    *target_name = name;
    return true;
}

static bool match_mod_eq_condition(ASTNode *condition, Token *iter_name,
                                   int *divisor, int *residue) {
    if (!condition || condition->type != NODE_BINARY ||
        condition->as.binary.op != TOKEN_EQUAL_EQUAL ||
        condition->as.binary.left->type != NODE_BINARY ||
        condition->as.binary.left->as.binary.op != TOKEN_PERCENT ||
        condition->as.binary.right->type != NODE_NUMBER) {
        return false;
    }

    ASTNode *mod = condition->as.binary.left;
    if (mod->as.binary.left->type != NODE_IDENTIFIER ||
        mod->as.binary.left->as.identifier.is_global ||
        !identifiers_equal(&mod->as.binary.left->as.identifier.name, iter_name) ||
        mod->as.binary.right->type != NODE_NUMBER) {
        return false;
    }

    if (!number_fits_i8(mod->as.binary.right, divisor) ||
        !number_fits_i8(condition->as.binary.right, residue) ||
        *divisor == 0) {
        return false;
    }
    return true;
}

static bool compile_for_range_mod_accumulator(ASTNode *node, int iter_reg, int end_reg) {
    ASTNode *stmt = single_block_statement(node->as.for_range.body);
    if (!stmt || stmt->type != NODE_IF) return false;

    ASTNode *elseif_node = stmt->as.if_stmt.else_branch;
    if (!elseif_node || elseif_node->type != NODE_IF ||
        !elseif_node->as.if_stmt.else_branch) {
        return false;
    }

    int divisor0, divisor1, residue0, residue1;
    if (!match_mod_eq_condition(stmt->as.if_stmt.condition, &node->as.for_range.var,
                                &divisor0, &residue0) ||
        !match_mod_eq_condition(elseif_node->as.if_stmt.condition, &node->as.for_range.var,
                                &divisor1, &residue1) ||
        divisor0 != divisor1) {
        return false;
    }

    Token *target_name = NULL;
    int delta0, delta1, delta_else;
    if (!match_accum_delta_branch(stmt->as.if_stmt.then_branch, &target_name, &delta0) ||
        !match_accum_delta_branch(elseif_node->as.if_stmt.then_branch, &target_name, &delta1) ||
        !match_accum_delta_branch(elseif_node->as.if_stmt.else_branch, &target_name, &delta_else)) {
        return false;
    }

    int target_reg = -1;
    bool target_const = false;
    for (int i = current->local_count - 1; i >= 0; i--) {
        if (identifiers_equal(target_name, &current->locals[i].name)) {
            target_reg = current->locals[i].reg;
            target_const = current->locals[i].is_const;
            break;
        }
    }
    if (target_reg < 0 || target_const || target_reg == iter_reg || target_reg == end_reg) {
        return false;
    }

    emit(ENCODE_ABC(node->as.for_range.inclusive ? OP_FOR_MODI_ACCUM_INC : OP_FOR_MODI_ACCUM,
                    iter_reg, target_reg, divisor0 & 0xff), node->line);
    emit(ENCODE_ABC(OP_AUX, residue0 & 0xff, delta0 & 0xff, residue1 & 0xff), node->line);
    emit(ENCODE_ABC(OP_AUX, delta1 & 0xff, delta_else & 0xff, 0), node->line);
    return true;
}

static bool compile_for_range_field2_accumulator(ASTNode *node, int iter_reg, int end_reg) {
    ASTNode *stmt = single_block_statement(node->as.for_range.body);
    if (!stmt) return false;
    if (stmt->type == NODE_EXPRESSION_STMT) {
        stmt = stmt->as.expr_stmt.expr;
    }
    if (!stmt || stmt->type != NODE_CALL ||
        stmt->as.call.callee->type != NODE_FIELD_GET) {
        return false;
    }

    ASTNode *callee_obj = stmt->as.call.callee->as.field_get.object;
    if (!callee_obj || callee_obj->type != NODE_IDENTIFIER ||
        callee_obj->as.identifier.is_global) {
        return false;
    }

    Local *receiver = resolve_local_entry(current, &callee_obj->as.identifier.name);
    if (!receiver || !receiver->has_known_struct || receiver->reg < 0 ||
        receiver->reg == iter_reg || receiver->reg == end_reg) {
        return false;
    }

    FieldLoopCandidate *candidate = find_field_loop_candidate(
        &receiver->known_struct,
        &stmt->as.call.callee->as.field_get.name);
    if (!candidate || candidate->update_count != 2) return false;

    int deltas[2];
    for (int i = 0; i < 2; i++) {
        int arg_index = candidate->param_indices[i];
        if (arg_index < 0 || arg_index >= stmt->as.call.args.count ||
            stmt->as.call.args.nodes[arg_index]->type != NODE_NUMBER ||
            !number_fits_i8(stmt->as.call.args.nodes[arg_index], &deltas[i])) {
            return false;
        }
    }

    int struct_k = identifier_constant(current->vm, &candidate->struct_name);
    int field0_k = identifier_constant(current->vm, &candidate->field_names[0]);
    int field1_k = identifier_constant(current->vm, &candidate->field_names[1]);
    if (struct_k > 255 || field0_k > 255 || field1_k > 255) return false;

    emit(ENCODE_ABC(node->as.for_range.inclusive ? OP_FOR_FIELD2_ACCUM_INC : OP_FOR_FIELD2_ACCUM,
                    iter_reg, receiver->reg, struct_k), node->line);
    emit(ENCODE_ABC(OP_AUX, field0_k, deltas[0] & 0xff, field1_k), node->line);
    emit(ENCODE_ABC(OP_AUX, deltas[1] & 0xff, 0, 0), node->line);
    return true;
}

static bool compile_for_range_field_accumulator(ASTNode *node, int iter_reg, int end_reg) {
    ASTNode *stmt = single_block_statement(node->as.for_range.body);
    if (!stmt) return false;

    ASTNode *assign = stmt;
    if (stmt->type == NODE_EXPRESSION_STMT) {
        assign = stmt->as.expr_stmt.expr;
    }
    if (!assign || assign->type != NODE_ASSIGN ||
        assign->as.assign.target->type != NODE_IDENTIFIER) {
        return false;
    }

    Token *target_name = &assign->as.assign.target->as.identifier.name;
    bool target_is_global = assign->as.assign.target->as.identifier.is_global ||
                            resolve_global(target_name);
    int target_reg = -1;
    int target_k = -1;
    if (!target_is_global) {
        target_reg = resolve_local(current, target_name);
        if (target_reg < 0 || target_reg == iter_reg || target_reg == end_reg) return false;
    }

    ASTNode *value = assign->as.assign.value;
    if (!value || value->type != NODE_BINARY || value->as.binary.op != TOKEN_PLUS) return false;
    ASTNode *left = value->as.binary.left;
    ASTNode *right = value->as.binary.right;
    if (!left || left->type != NODE_IDENTIFIER ||
        !identifiers_equal(&left->as.identifier.name, target_name)) {
        return false;
    }
    if (!target_is_global && left->as.identifier.is_global) return false;

    if (!right || right->type != NODE_TRY) return false;
    ASTNode *inner = right->as.try_expr.expr;
    if (!inner || inner->type != NODE_INDEX ||
        inner->as.index_expr.index->type != NODE_STRING) {
        return false;
    }
    if (expr_references_name(inner, target_name) ||
        expr_references_name(inner, &node->as.for_range.var) ||
        !call_arg_is_safe(inner->as.index_expr.object)) {
        return false;
    }

    if (target_is_global) {
        target_reg = alloc_reg();
        reserve_reg_index(target_reg + 1);
        target_k = identifier_constant(current->vm, target_name);
    }

    int obj_reg = compile_node(inner->as.index_expr.object);
    ASTNode *key = inner->as.index_expr.index;
    int field_k = string_constant(current->vm, key->as.string.value, key->as.string.length);
    if (target_is_global) {
        emit(ENCODE_ABC(node->as.for_range.inclusive
                            ? OP_FORADDGLOBAL_FIELD_PROP_INC
                            : OP_FORADDGLOBAL_FIELD_PROP,
                        iter_reg, target_reg, obj_reg), node->line);
        emit(ENCODE_ABx(OP_AUX, 0, target_k), node->line);
        emit(ENCODE_ABx(OP_AUX, 0, field_k), node->line);
        free_temp(target_reg);
    } else {
        emit(ENCODE_ABC(node->as.for_range.inclusive
                            ? OP_FORADDLOCAL_FIELD_PROP_INC
                            : OP_FORADDLOCAL_FIELD_PROP,
                        iter_reg, target_reg, obj_reg), node->line);
        emit(ENCODE_ABx(OP_AUX, 0, field_k), node->line);
    }
    free_temp(obj_reg);
    return true;
}

static bool branch_is_single_break(ASTNode *branch) {
    ASTNode *stmt = single_block_statement(branch);
    return stmt && stmt->type == NODE_BREAK;
}

static bool compile_array_mark_false_stride_loop(ASTNode *node) {
    ASTNode *body = node->as.loop_stmt.body;
    if (!body || body->type != NODE_BLOCK || body->as.block.stmts.count != 3) return false;

    ASTNode *guard = body->as.block.stmts.nodes[0];
    ASTNode *set_stmt = body->as.block.stmts.nodes[1];
    ASTNode *inc_stmt = body->as.block.stmts.nodes[2];
    if (!guard || guard->type != NODE_IF || guard->as.if_stmt.else_branch ||
        !branch_is_single_break(guard->as.if_stmt.then_branch)) {
        return false;
    }

    ASTNode *cond = guard->as.if_stmt.condition;
    if (!cond || cond->type != NODE_BINARY || cond->as.binary.op != TOKEN_GREATER ||
        cond->as.binary.left->type != NODE_IDENTIFIER ||
        cond->as.binary.left->as.identifier.is_global ||
        cond->as.binary.right->type != NODE_IDENTIFIER ||
        cond->as.binary.right->as.identifier.is_global) {
        return false;
    }
    Token *index_name = &cond->as.binary.left->as.identifier.name;
    Token *limit_name = &cond->as.binary.right->as.identifier.name;

    if (set_stmt->type == NODE_EXPRESSION_STMT) set_stmt = set_stmt->as.expr_stmt.expr;
    if (!set_stmt || set_stmt->type != NODE_ASSIGN ||
        set_stmt->as.assign.target->type != NODE_INDEX ||
        set_stmt->as.assign.value->type != NODE_BOOL ||
        set_stmt->as.assign.value->as.boolean.value) {
        return false;
    }
    ASTNode *target = set_stmt->as.assign.target;
    if (target->as.index_expr.object->type != NODE_IDENTIFIER ||
        target->as.index_expr.object->as.identifier.is_global ||
        target->as.index_expr.index->type != NODE_IDENTIFIER ||
        target->as.index_expr.index->as.identifier.is_global ||
        !identifiers_equal(index_name, &target->as.index_expr.index->as.identifier.name)) {
        return false;
    }
    Token *array_name = &target->as.index_expr.object->as.identifier.name;

    if (inc_stmt->type == NODE_EXPRESSION_STMT) inc_stmt = inc_stmt->as.expr_stmt.expr;
    if (!inc_stmt || inc_stmt->type != NODE_ASSIGN ||
        inc_stmt->as.assign.target->type != NODE_IDENTIFIER ||
        inc_stmt->as.assign.target->as.identifier.is_global ||
        !identifiers_equal(index_name, &inc_stmt->as.assign.target->as.identifier.name)) {
        return false;
    }
    ASTNode *value = inc_stmt->as.assign.value;
    if (!value || value->type != NODE_BINARY || value->as.binary.op != TOKEN_PLUS ||
        value->as.binary.left->type != NODE_IDENTIFIER ||
        value->as.binary.left->as.identifier.is_global ||
        !identifiers_equal(index_name, &value->as.binary.left->as.identifier.name) ||
        value->as.binary.right->type != NODE_IDENTIFIER ||
        value->as.binary.right->as.identifier.is_global) {
        return false;
    }
    Token *step_name = &value->as.binary.right->as.identifier.name;

    int array_reg = resolve_local(current, array_name);
    int index_reg = resolve_local(current, index_name);
    int limit_reg = resolve_local(current, limit_name);
    int step_reg = resolve_local(current, step_name);
    if (array_reg < 0 || index_reg < 0 || limit_reg < 0 || step_reg < 0) return false;

    emit(ENCODE_ABC(OP_ARRAY_MARK_FALSE_STRIDE, array_reg, index_reg, limit_reg), node->line);
    emit(ENCODE_ABx(OP_AUX, 0, step_reg), node->line);
    return true;
}

static int compile_call(ASTNode *node) {
    int arg_count = node->as.call.args.count;
    bool is_method_call = (node->as.call.callee->type == NODE_FIELD_GET);

    if (is_method_call &&
        arg_count == 1 &&
        node->as.call.callee->as.field_get.object->type == NODE_IDENTIFIER &&
        is_string_name(&node->as.call.callee->as.field_get.object->as.identifier.name) &&
        is_len_name(&node->as.call.callee->as.field_get.name) &&
        !get_root_compiler()->string_shadowed &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.field_get.object->as.identifier.name)) {
        int value_reg = compile_node(node->as.call.args.nodes[0]);
        int dest = alloc_reg();
        emit(ENCODE_ABC(OP_LEN, dest, value_reg, 0), node->line);
        free_temp(value_reg);
        return dest;
    }

    if (is_method_call &&
        arg_count == 1 &&
        node->as.call.callee->as.field_get.object->type == NODE_IDENTIFIER &&
        is_array_name(&node->as.call.callee->as.field_get.object->as.identifier.name) &&
        is_len_name(&node->as.call.callee->as.field_get.name) &&
        !get_root_compiler()->array_shadowed &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.field_get.object->as.identifier.name)) {
        int value_reg = compile_node(node->as.call.args.nodes[0]);
        int dest = alloc_reg();
        emit(ENCODE_ABC(OP_LEN, dest, value_reg, 0), node->line);
        free_temp(value_reg);
        return dest;
    }

    if (is_method_call &&
        arg_count == 1 &&
        node->as.call.callee->as.field_get.object->type == NODE_IDENTIFIER &&
        is_dict_name(&node->as.call.callee->as.field_get.object->as.identifier.name) &&
        is_len_name(&node->as.call.callee->as.field_get.name) &&
        !get_root_compiler()->dict_shadowed &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.field_get.object->as.identifier.name)) {
        int value_reg = compile_node(node->as.call.args.nodes[0]);
        int dest = alloc_reg();
        emit(ENCODE_ABC(OP_LEN, dest, value_reg, 0), node->line);
        free_temp(value_reg);
        return dest;
    }

    if (!is_method_call &&
        arg_count == 1 &&
        node->as.call.callee->type == NODE_IDENTIFIER &&
        is_tostring_name(&node->as.call.callee->as.identifier.name) &&
        !get_root_compiler()->tostring_shadowed &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.identifier.name)) {
        int value_reg = compile_node(node->as.call.args.nodes[0]);
        int dest = alloc_reg();
        emit(ENCODE_ABC(OP_TOSTRING, dest, value_reg, 0), node->line);
        free_temp(value_reg);
        return dest;
    }

    if (!is_method_call &&
        arg_count == 1 &&
        node->as.call.callee->type == NODE_IDENTIFIER &&
        is_len_name(&node->as.call.callee->as.identifier.name) &&
        !get_root_compiler()->len_shadowed &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.identifier.name)) {
        int value_reg = compile_node(node->as.call.args.nodes[0]);
        int dest = alloc_reg();
        emit(ENCODE_ABC(OP_LEN, dest, value_reg, 0), node->line);
        free_temp(value_reg);
        return dest;
    }

    if (!is_method_call &&
        arg_count == 2 &&
        node->as.call.callee->type == NODE_IDENTIFIER &&
        is_push_name(&node->as.call.callee->as.identifier.name) &&
        !get_root_compiler()->push_shadowed &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.identifier.name)) {
        int array_reg = compile_node(node->as.call.args.nodes[0]);
        int value_reg = compile_node(node->as.call.args.nodes[1]);
        emit(ENCODE_ABC(OP_ARRAY_PUSH, array_reg, value_reg, 0), node->line);
        free_temp(value_reg);
        free_temp(array_reg);
        int dest = alloc_reg();
        emit(ENCODE_ABC(OP_LOADNIL, dest, 0, 0), node->line);
        return dest;
    }

    if (!is_method_call &&
        node->as.call.callee->type == NODE_IDENTIFIER) {
        InlineCandidate *candidate = find_inline_candidate(&node->as.call.callee->as.identifier.name);
        if (candidate) {
            int result = compile_inline_call(node, candidate);
            if (result != -1) return result;
        }
    }

    int total_args = arg_count + (is_method_call ? 1 : 0);
    int global_call_k = -1;
    bool direct_self_call = false;
    int direct_local_callee = -1;
    bool use_mcallfield = false;
    int mcallfield_k = -1;

    if (!is_method_call &&
        node->as.call.callee->type == NODE_IDENTIFIER) {
        Token *callee_name = &node->as.call.callee->as.identifier.name;
        if (current->self_call_safe &&
            token_matches_string(callee_name, current->function->name) &&
            resolve_local(current, callee_name) == -1) {
            direct_self_call = true;
        } else if (call_args_are_safe(&node->as.call.args)) {
            direct_local_callee = resolve_local(current, callee_name);
        }
    }

    if (!is_method_call &&
        node->as.call.callee->type == NODE_IDENTIFIER &&
        !direct_self_call &&
        direct_local_callee == -1 &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.identifier.name) &&
        resolve_global(&node->as.call.callee->as.identifier.name)) {
        int k = identifier_constant(current->vm, &node->as.call.callee->as.identifier.name);
        if (k <= 255) {
            global_call_k = k;
        }
    }

    int base = alloc_reg();
    for (int i = 0; i < total_args; i++) {
        alloc_reg();
    }
    int saved_next_reg = current->next_reg;
    current->next_reg = base + total_args + 1;

    int obj_reg = -1;
    int callee;
    if (direct_self_call || direct_local_callee != -1 || global_call_k != -1) {
        callee = base;
    } else if (is_method_call) {
        ASTNode *fg = node->as.call.callee;
        mcallfield_k = identifier_constant(current->vm, &fg->as.field_get.name);
        use_mcallfield = (mcallfield_k <= 255);
        obj_reg = compile_node(fg->as.field_get.object);
        if (use_mcallfield) {
            callee = base;
        } else {
            callee = alloc_reg();
            emit(ENCODE_ABC(OP_GETFIELD, callee, obj_reg, mcallfield_k), node->line);
        }
    } else {
        if (compile_simple_into(node->as.call.callee, base)) {
            callee = base;
        } else {
            callee = compile_node(node->as.call.callee);
        }
    }

    int arg_regs[256];
    for (int i = 0; i < arg_count; i++) {
        int expected = base + 1 + i + (is_method_call ? 1 : 0);
        if (compile_simple_into(node->as.call.args.nodes[i], expected)) {
            arg_regs[i] = expected;
        } else {
            arg_regs[i] = compile_node(node->as.call.args.nodes[i]);
        }
    }

    current->next_reg = saved_next_reg;

    if (callee != base) {
        emit(ENCODE_ABC(OP_MOVE, base, callee, 0), node->line);
    }

    if (is_method_call) {
        if (obj_reg != base + 1) {
            emit(ENCODE_ABC(OP_MOVE, base + 1, obj_reg, 0), node->line);
        }
        for (int i = 0; i < arg_count; i++) {
            int expected = base + 2 + i;
            if (arg_regs[i] != expected) {
                emit(ENCODE_ABC(OP_MOVE, expected, arg_regs[i], 0), node->line);
            }
        }
    } else {
        for (int i = 0; i < arg_count; i++) {
            int expected = base + 1 + i;
            if (arg_regs[i] != expected) {
                emit(ENCODE_ABC(OP_MOVE, expected, arg_regs[i], 0), node->line);
            }
        }
    }

    if (direct_self_call) {
        emit(ENCODE_ABC(OP_CALLSELF, base, total_args, 2), node->line);
    } else if (direct_local_callee != -1) {
        emit(ENCODE_ABC(OP_CALLR, base, direct_local_callee, total_args), node->line);
    } else if (global_call_k != -1) {
        emit(ENCODE_ABC(OP_CALLG, base, total_args, global_call_k), node->line);
    } else if (use_mcallfield) {
        emit(ENCODE_ABC(OP_MCALLFIELD, base, total_args, mcallfield_k), node->line);
    } else {
        emit(ENCODE_ABC(is_method_call ? OP_MCALL : OP_CALL, base, total_args, 2), node->line);
    }
    return base;
}

static int compile_call_with_expected(ASTNode *node, int expected_returns) {
    bool is_method_call = (node->as.call.callee->type == NODE_FIELD_GET);
    int arg_count = node->as.call.args.count;
    int obj_reg = -1;
    int callee = -1;
    int mcallfield_k = -1;
    bool use_mcallfield0 = false;

    if (!is_method_call &&
        arg_count == 2 &&
        node->as.call.callee->type == NODE_IDENTIFIER &&
        is_push_name(&node->as.call.callee->as.identifier.name) &&
        !get_root_compiler()->push_shadowed &&
        !resolve_local_in_chain(current, &node->as.call.callee->as.identifier.name)) {
        int array_reg = compile_node(node->as.call.args.nodes[0]);
        int value_reg = compile_node(node->as.call.args.nodes[1]);
        emit(ENCODE_ABC(OP_ARRAY_PUSH, array_reg, value_reg, 0), node->line);
        free_temp(value_reg);
        free_temp(array_reg);
        if (expected_returns <= 0) return -1;
        int dest = alloc_reg();
        for (int i = 0; i < expected_returns; i++) {
            emit(ENCODE_ABC(OP_LOADNIL, dest + i, 0, 0), node->line);
            if (i > 0) alloc_reg();
        }
        return dest;
    }

    if (is_method_call) {
        ASTNode *fg = node->as.call.callee;
        obj_reg = compile_node(fg->as.field_get.object);
        int k = identifier_constant(current->vm, &fg->as.field_get.name);
        if (expected_returns == 0 && k <= 255) {
            use_mcallfield0 = true;
            mcallfield_k = k;
        } else {
            callee = alloc_reg();
            emit(ENCODE_ABC(OP_GETFIELD, callee, obj_reg, k), node->line);
        }
    } else {
        callee = compile_node(node->as.call.callee);
    }

    int arg_regs[256];
    for (int i = 0; i < arg_count; i++) {
        arg_regs[i] = compile_node(node->as.call.args.nodes[i]);
    }

    int total_args = arg_count + (is_method_call ? 1 : 0);
    int base = alloc_reg();
    int slots_after_base = total_args > expected_returns - 1 ? total_args : expected_returns - 1;
    for (int i = 0; i < slots_after_base; i++) alloc_reg();

    if (!use_mcallfield0 && callee != base) {
        emit(ENCODE_ABC(OP_MOVE, base, callee, 0), node->line);
    }

    if (is_method_call) {
        if (obj_reg != base + 1) {
            emit(ENCODE_ABC(OP_MOVE, base + 1, obj_reg, 0), node->line);
        }
        for (int i = 0; i < arg_count; i++) {
            int expected = base + 2 + i;
            if (arg_regs[i] != expected) {
                emit(ENCODE_ABC(OP_MOVE, expected, arg_regs[i], 0), node->line);
            }
        }
    } else {
        for (int i = 0; i < arg_count; i++) {
            int expected = base + 1 + i;
            if (arg_regs[i] != expected) {
                emit(ENCODE_ABC(OP_MOVE, expected, arg_regs[i], 0), node->line);
            }
        }
    }

    if (use_mcallfield0) {
        emit(ENCODE_ABC(OP_MCALLFIELD0, base, total_args, mcallfield_k), node->line);
    } else {
        emit(ENCODE_ABC(is_method_call ? OP_MCALL : OP_CALL,
                        base, total_args, expected_returns + 1), node->line);
    }
    return base;
}

static int compile_index_op(ASTNode *node, OpCode op) {
    int obj = compile_node(node->as.index_expr.object);
    if (node->as.index_expr.index->type == NODE_STRING &&
        (op == OP_GETINDEX || op == OP_GETINDEX_TRY)) {
        int dest = alloc_reg();
        if (op == OP_GETINDEX_TRY) {
            alloc_reg();
        }
        ASTNode *key = node->as.index_expr.index;
        int field_k = string_constant(current->vm, key->as.string.value, key->as.string.length);
        emit(ENCODE_ABC(op == OP_GETINDEX_TRY ? OP_GETFIELD_TRY : OP_GETFIELD,
                        dest, obj, field_k), node->line);
        free_temp(obj);
        return dest;
    }
    int idx = compile_node(node->as.index_expr.index);
    int dest = alloc_reg();
    if (op == OP_GETINDEX_TRY) {
        alloc_reg();
    }
    emit(ENCODE_ABC(op, dest, obj, idx), node->line);
    free_temp(idx);
    free_temp(obj);
    return dest;
}

static int compile_index(ASTNode *node) {
    return compile_index_op(node, OP_GETINDEX);
}

static int compile_propagating_field_lookup(ASTNode *node) {
    int obj = compile_node(node->as.index_expr.object);
    int dest = alloc_reg();
    int err_reg = alloc_reg();
    ASTNode *key = node->as.index_expr.index;
    int field_k = string_constant(current->vm, key->as.string.value, key->as.string.length);

    emit(ENCODE_ABC(OP_GETFIELD_PROP, dest, obj, field_k), node->line);
    free_temp(err_reg);
    free_temp(obj);
    return dest;
}

static void compile_propagate_error(int err_reg, int line) {
    int saved_next = current->next_reg;
    int no_err_jump = emit_error_test_jump(err_reg, line);

    if (current->error_handler_reg >= 0) {
        emit(ENCODE_ABC(OP_MOVE, current->error_handler_reg, err_reg, 0), line);
        add_error_jump(emit_jump(OP_JMP, line));
    } else {
        int nil_reg = alloc_reg();
        int out_err_reg = alloc_reg();
        emit(ENCODE_ABC(OP_LOADNIL, nil_reg, 0, 0), line);
        emit(ENCODE_ABC(OP_MOVE, out_err_reg, err_reg, 0), line);
        emit(ENCODE_ABC(OP_RETURN, nil_reg, 2, 0), line);
    }

    if (current->next_reg > current->max_reg) {
        current->max_reg = current->next_reg;
    }
    current->next_reg = saved_next;
    patch_test_jump(no_err_jump);
}

static int compile_try(ASTNode *node) {
    ASTNode *expr = node->as.try_expr.expr;
    if (expr->type == NODE_INDEX) {
        if (current->error_handler_reg < 0 &&
            expr->as.index_expr.index->type == NODE_STRING) {
            return compile_propagating_field_lookup(expr);
        }
        int value_reg = compile_index_op(expr, OP_GETINDEX_TRY);
        compile_propagate_error(value_reg + 1, node->line);
        free_temp(value_reg + 1);
        return value_reg;
    }
    if (expr->type == NODE_CALL) {
        int value_reg = compile_call_with_expected(expr, 2);
        compile_propagate_error(value_reg + 1, node->line);
        free_temp(value_reg + 1);
        return value_reg;
    }

    Token token = {TOKEN_QUESTION, "?", 1, node->line};
    error_at(&token, "'?' supports fallible lookups and calls.");
    return compile_node(expr);
}

static int compile_field_get(ASTNode *node) {
    int obj = compile_node(node->as.field_get.object);
    int dest = alloc_reg();
    Token *name = &node->as.field_get.name;
    int k = identifier_constant(current->vm, name);

    if (node->as.field_get.object->type == NODE_IDENTIFIER) {
        int local = resolve_local(current, &node->as.field_get.object->as.identifier.name);
        if (local != -1) {
            for (int i = current->local_count - 1; i >= 0; i--) {
                if (current->locals[i].reg == local && current->locals[i].name.length > 0) {
                    Token var_name = current->locals[i].name;
                    ObjString *var_str = copy_string(current->vm, var_name.start, var_name.length);
                    Value val;
                    if (table_get(&current->vm->globals, var_str, &val)) {
                        if (IS_STRUCT(val)) {
                            ObjStruct *s = AS_STRUCT_OBJ(val);
                            for (int fi = 0; fi < s->field_count; fi++) {
                                if (s->field_names[fi] != NULL &&
                                    s->field_names[fi]->length == name->length &&
                                    memcmp(s->field_names[fi]->chars, name->start, name->length) == 0) {
                                    emit(ENCODE_ABC(OP_GETFIELD_IDX, dest, obj, fi), node->line);
                                    return dest;
                                }
                            }
                        }
                    }
                    break;
                }
            }
        }
    }

    emit(ENCODE_ABC(OP_GETFIELD, dest, obj, k), node->line);
    return dest;
}

static int compile_array_literal(ASTNode *node) {
    int count = node->as.array_literal.items.count;
    int dest = alloc_reg();
    emit(ENCODE_ABC(OP_NEWARRAY, dest, count, 0), node->line);

    for (int i = 0; i < count; i++) {
        int val = compile_node(node->as.array_literal.items.nodes[i]);
        emit(ENCODE_ABC(OP_SETARRAY, dest, i, val), node->line);
    }
    return dest;
}

static int compile_dict_literal(ASTNode *node) {
    int dest = alloc_reg();
    emit(ENCODE_ABC(OP_NEWDICT, dest, 0, 0), node->line);

    for (int i = 0; i < node->as.dict_literal.entries.count; i++) {
        int key = compile_node(node->as.dict_literal.entries.pairs[i].key);
        int val = compile_node(node->as.dict_literal.entries.pairs[i].value);
        emit(ENCODE_ABC(OP_SETINDEX, dest, key, val), node->line);
    }
    return dest;
}

static int compile_struct_literal(ASTNode *node) {
    /* Look up the struct by name. Module-private structs are locals. */
    int struct_k = identifier_constant(current->vm, &node->as.struct_literal.name);
    int struct_reg = alloc_reg();
    int local = resolve_local(current, &node->as.struct_literal.name);
    if (local != -1) {
        emit(ENCODE_ABC(OP_MOVE, struct_reg, local, 0), node->line);
    } else {
        int upval = resolve_upvalue(current, &node->as.struct_literal.name);
        if (upval != -1) {
            emit(ENCODE_ABC(OP_GETUPVAL, struct_reg, upval, 0), node->line);
        } else {
            emit(ENCODE_ABx(OP_GETGLOBAL, struct_reg, struct_k), node->line);
        }
    }

    /* Call the struct to create an instance (OP_CALL on a struct = instantiate) */
    int base = alloc_reg();
    emit(ENCODE_ABC(OP_MOVE, base, struct_reg, 0), node->line);
    emit(ENCODE_ABC(OP_CALL, base, 0, 2), node->line);

    /* Base holds the instance. Set fields by name. */
    for (int i = 0; i < node->as.struct_literal.fields.count; i++) {
        ASTNode *key_node = node->as.struct_literal.fields.pairs[i].key;
        int field_k = string_constant(current->vm,
            key_node->as.string.value, key_node->as.string.length);
        int val = compile_node(node->as.struct_literal.fields.pairs[i].value);
        emit(ENCODE_ABC(OP_SETFIELD, base, field_k, val), node->line);
    }

    return base;
}

static int compile_interp_string(ASTNode *node) {
    fprintf(stderr, "[line %d] Error: Interpolated string not supported in compiler (should be desugared by parser).\n", node->line);
    current->had_error = true;
    int reg = alloc_reg();
    emit(ENCODE_ABC(OP_LOADNIL, reg, 0, 0), node->line);
    return reg;
}

/* Compile function body into a new ObjFunction */
static int compile_function(ASTNode *node, FunctionType type) {
    bool self_call_safe = current &&
                          current->type == FUNC_SCRIPT &&
                          current->scope_depth == 0 &&
                          type == FUNC_FUNCTION &&
                          !node->as.fn_decl.is_method &&
                          node->as.fn_decl.name.length > 0;
    Compiler compiler;
    compiler_init(&compiler, current->vm, type, &node->as.fn_decl.name);
    compiler.self_call_safe = self_call_safe;

    begin_scope();

    /* For methods, 'self' is explicitly listed in the parameter list.
       No implicit injection, the compiler handles self-injection
       at the call site (compile_call). */

    /* Compile parameters */
    for (int i = 0; i < node->as.fn_decl.param_count; i++) {
        int r = alloc_reg();
        add_local(node->as.fn_decl.params[i].name, r, false);
        compiler.function->arity++;
    }

    /* Compile body */
    compile_block(node->as.fn_decl.body);

    /* Execute remaining defers in LIFO before implicit return */
    for (int i = compiler.defer_count - 1; i >= 0; i--) {
        compile_node(compiler.defers[i].expr);
    }
    compiler.defer_count = 0;

    /* Implicit return nil */
    int nil_reg = alloc_reg();
    emit(ENCODE_ABC(OP_LOADNIL, nil_reg, 0, 0), node->line);
    emit(ENCODE_ABC(OP_RETURN, nil_reg, 1, 0), node->line);

    ObjFunction *fn = compiler.function;
    fn->reg_count = compiler.max_reg;  /* actual register count for stack sizing */
    UpvalueInfo *upvals = compiler.upvalues;
    int upval_count = fn->upvalue_count;

    /* Restore enclosing compiler */
    current = compiler.enclosing;
    if (compiler.had_error) current->had_error = true;
    free(compiler.break_jumps);
    free(compiler.error_jumps);
    free(compiler.defers);
    free(compiler.globals);
    free(compiler.inline_candidates);
    free(compiler.field_loop_candidates);
    free_compiler_owned_strings(&compiler);

    /* Emit closure instruction in the enclosing scope */
    int dest = alloc_reg();
    int k = make_constant(OBJ_VAL(fn));
    emit(ENCODE_ABx(OP_CLOSURE, dest, k), node->line);

    /* Emit upvalue capture info: one pseudo-instruction per upvalue */
    for (int i = 0; i < upval_count; i++) {
        emit(ENCODE_ABC(upvals[i].is_local ? 1 : 0, upvals[i].index, 0, 0), node->line);
    }

    return dest;
}

/* ========================================================================
 * compile_node: dispatch an expression node, return result register
 * ======================================================================== */
static int compile_node(ASTNode *node) {
    switch (node->type) {
        case NODE_NUMBER:        return compile_number(node);
        case NODE_STRING:        return compile_string(node);
        case NODE_INTERP_STRING: return compile_interp_string(node);
        case NODE_BOOL:          return compile_bool(node);
        case NODE_NULL:          return compile_null(node);
        case NODE_IDENTIFIER:    return compile_identifier(node);
        case NODE_UNARY:         return compile_unary(node);
        case NODE_BINARY:        return compile_binary(node);
        case NODE_LOGICAL:       return compile_logical(node);
        case NODE_CALL:          return compile_call(node);
        case NODE_INDEX:         return compile_index(node);
        case NODE_TRY:           return compile_try(node);
        case NODE_TRY_BLOCK:     return compile_try_block(node);
        case NODE_FIELD_GET:     return compile_field_get(node);
        case NODE_ARRAY_LITERAL: return compile_array_literal(node);
        case NODE_DICT_LITERAL:    return compile_dict_literal(node);
        case NODE_STRUCT_LITERAL:  return compile_struct_literal(node);
        case NODE_FN_DECL:         return compile_function(node, FUNC_FUNCTION);
        default:
            fprintf(stderr, "[line %d] Cannot compile node type %d as expression.\n",
                    node->line, node->type);
            return alloc_reg();
    }
}

/* ========================================================================
 * Statement Compilation
 * ======================================================================== */
static bool is_local_const(Compiler *compiler, Token *name) {
    for (int i = compiler->local_count - 1; i >= 0; i--) {
        if (identifiers_equal(name, &compiler->locals[i].name)) {
            return compiler->locals[i].is_const;
        }
    }
    return false;
}

static bool is_const_in_chain(Compiler *compiler, Token *name) {
    for (Compiler *c = compiler; c != NULL; c = c->enclosing) {
        if (is_local_const(c, name)) return true;
    }
    return false;
}

static void compile_assign(ASTNode *node) {
    ASTNode *target = node->as.assign.target;

    /* Check const enforcement before compiling the value */
    if (target->type == NODE_IDENTIFIER && is_push_name(&target->as.identifier.name) &&
        (target->as.identifier.is_global ||
         (current->scope_depth == 0 && !resolve_local_in_chain(current, &target->as.identifier.name)))) {
        get_root_compiler()->push_shadowed = true;
    }
    if (target->type == NODE_IDENTIFIER && is_tostring_name(&target->as.identifier.name) &&
        (target->as.identifier.is_global ||
         (current->scope_depth == 0 && !resolve_local_in_chain(current, &target->as.identifier.name)))) {
        get_root_compiler()->tostring_shadowed = true;
    }
    if (target->type == NODE_IDENTIFIER && is_len_name(&target->as.identifier.name) &&
        (target->as.identifier.is_global ||
         (current->scope_depth == 0 && !resolve_local_in_chain(current, &target->as.identifier.name)))) {
        get_root_compiler()->len_shadowed = true;
    }
    if (target->type == NODE_IDENTIFIER && is_string_name(&target->as.identifier.name) &&
        (target->as.identifier.is_global ||
         (current->scope_depth == 0 && !resolve_local_in_chain(current, &target->as.identifier.name)))) {
        get_root_compiler()->string_shadowed = true;
    }
    if (target->type == NODE_IDENTIFIER && is_array_name(&target->as.identifier.name) &&
        (target->as.identifier.is_global ||
         (current->scope_depth == 0 && !resolve_local_in_chain(current, &target->as.identifier.name)))) {
        get_root_compiler()->array_shadowed = true;
    }
    if (target->type == NODE_IDENTIFIER && is_dict_name(&target->as.identifier.name) &&
        (target->as.identifier.is_global ||
         (current->scope_depth == 0 && !resolve_local_in_chain(current, &target->as.identifier.name)))) {
        get_root_compiler()->dict_shadowed = true;
    }

    if (target->type == NODE_IDENTIFIER && !target->as.identifier.is_global) {
        Token *name = &target->as.identifier.name;
        if (is_const_in_chain(current, name)) {
            error_at(name, "Cannot assign to constant.");
            return;
        }
        update_known_struct_for_local(name, node->as.assign.value);
    }

    if (target->type == NODE_IDENTIFIER && !target->as.identifier.is_global) {
        Token *name = &target->as.identifier.name;
        int local = resolve_local(current, name);
        if (local != -1 &&
            compile_local_update_chain(node->as.assign.value, name, local, true)) {
            compile_local_update_chain(node->as.assign.value, name, local, false);
            return;
        }
    }

    if (target->type == NODE_IDENTIFIER && !target->as.identifier.is_global) {
        Token *name = &target->as.identifier.name;
        int local = resolve_local(current, name);
        if (local != -1 && compile_node_into_reg(node->as.assign.value, local)) {
            return;
        }
    }

    if (target->type == NODE_IDENTIFIER) {
        Token *name = &target->as.identifier.name;
        if ((target->as.identifier.is_global || resolve_global(name)) &&
            compile_local_update_chain(node->as.assign.value, name, 0, true) &&
            local_update_chain_has_fused_term(node->as.assign.value, name)) {
            int acc = alloc_reg();
            int k = identifier_constant(current->vm, name);
            emit(ENCODE_ABx(OP_GETGLOBAL, acc, k), node->line);
            compile_local_update_chain(node->as.assign.value, name, acc, false);
            emit(ENCODE_ABx(OP_SETGLOBAL, acc, k), node->line);
            free_temp(acc);
            return;
        }
    }

    if (target->type == NODE_IDENTIFIER && !target->as.identifier.is_global &&
        node->as.assign.value &&
        node->as.assign.value->type == NODE_BINARY &&
        node->as.assign.value->as.binary.op == TOKEN_PLUS &&
        node->as.assign.value->as.binary.left->type == NODE_IDENTIFIER &&
        !node->as.assign.value->as.binary.left->as.identifier.is_global &&
        identifiers_equal(&target->as.identifier.name,
                          &node->as.assign.value->as.binary.left->as.identifier.name) &&
        call_arg_is_safe(node->as.assign.value->as.binary.right) &&
        resolve_local(current, &target->as.identifier.name) == -1) {
        int upval = resolve_upvalue(current, &target->as.identifier.name);
        if (upval != -1) {
            int rhs = compile_node(node->as.assign.value->as.binary.right);
            emit(ENCODE_ABC(OP_ADDUP, upval, rhs, 0), node->line);
            free_temp(rhs);
            return;
        }
    }

    int val = compile_node(node->as.assign.value);

    if (target->type == NODE_IDENTIFIER) {
        Token *name = &target->as.identifier.name;
        if (target->as.identifier.is_global) {
            int k = identifier_constant(current->vm, name);
            emit(ENCODE_ABx(OP_SETGLOBAL, val, k), node->line);
            return;
        }
        int local = resolve_local(current, name);
        if (local != -1) {
            emit(ENCODE_ABC(OP_MOVE, local, val, 0), node->line);
            return;
        }
        int upval = resolve_upvalue(current, name);
        if (upval != -1) {
            emit(ENCODE_ABC(OP_SETUPVAL, val, upval, 0), node->line);
            return;
        }

        /* Check if it's a known global */
        if (resolve_global(name)) {
            int k = identifier_constant(current->vm, name);
            emit(ENCODE_ABx(OP_SETGLOBAL, val, k), node->line);
            return;
        }
        
        /* Error: assigning to undeclared local */
        error_at(name, "Undefined variable.");
    } else if (target->type == NODE_INDEX) {
        int obj = compile_node(target->as.index_expr.object);
        int idx = compile_node(target->as.index_expr.index);
        emit(ENCODE_ABC(OP_SETINDEX, obj, idx, val), node->line);
    }
}

static void compile_field_set(ASTNode *node) {
    int obj = compile_node(node->as.field_set.object);
    int val = compile_node(node->as.field_set.value);
    int k = identifier_constant(current->vm, &node->as.field_set.name);
    emit(ENCODE_ABC(OP_SETFIELD, obj, k, val), node->line);
}

static void compile_var_decl(ASTNode *node) {
    Token *name = &node->as.var_decl.name;
    bool is_global = node->as.var_decl.is_global;
    bool is_const = node->as.var_decl.is_const;
    int nnames = node->as.var_decl.name_count;

    if (is_global) {
        add_global(*name);
        for (int i = 0; i < nnames - 1; i++) {
            add_global(node->as.var_decl.extra_names[i]);
        }
    }

    /* Multi-return: let a, b = func() */
    if (nnames > 1 && node->as.var_decl.initializer) {
        /* The initializer must be a call expression for multi-return. */
        ASTNode *init = node->as.var_decl.initializer;

        if (init->type == NODE_TRY_BLOCK) {
            int base = compile_try_block(init);
            if (is_global) {
                int k0 = identifier_constant(current->vm, name);
                emit(ENCODE_ABx(OP_SETGLOBAL, base, k0), node->line);
                for (int i = 1; i < nnames; i++) {
                    int ki = identifier_constant(current->vm, &node->as.var_decl.extra_names[i - 1]);
                    emit(ENCODE_ABx(OP_SETGLOBAL, base + i, ki), node->line);
                }
            } else {
                add_local(*name, base, is_const);
                for (int i = 1; i < nnames; i++) {
                    int r = base + i;
                    if (current->next_reg <= r) current->next_reg = r + 1;
                    add_local(node->as.var_decl.extra_names[i - 1], r, is_const);
                }
            }
        } else if (init->type == NODE_CALL) {
            bool is_method = (init->as.call.callee->type == NODE_FIELD_GET);
            int arg_count = init->as.call.args.count;

            int obj_reg = -1;
            int callee;
            if (is_method) {
                ASTNode *fg = init->as.call.callee;
                obj_reg = compile_node(fg->as.field_get.object);
                callee = alloc_reg();
                int k = identifier_constant(current->vm, &fg->as.field_get.name);
                emit(ENCODE_ABC(OP_GETFIELD, callee, obj_reg, k), node->line);
            } else {
                callee = compile_node(init->as.call.callee);
            }

            int arg_regs[256];
            for (int i = 0; i < arg_count; i++) {
                arg_regs[i] = compile_node(init->as.call.args.nodes[i]);
            }

            int total_args = arg_count + (is_method ? 1 : 0);
            int base = alloc_reg();
            for (int i = 0; i < total_args; i++) alloc_reg();

            if (callee != base)
                emit(ENCODE_ABC(OP_MOVE, base, callee, 0), node->line);

            if (is_method) {
                if (obj_reg != base + 1)
                    emit(ENCODE_ABC(OP_MOVE, base + 1, obj_reg, 0), node->line);
                for (int i = 0; i < arg_count; i++) {
                    int expected = base + 2 + i;
                    if (arg_regs[i] != expected)
                        emit(ENCODE_ABC(OP_MOVE, expected, arg_regs[i], 0), node->line);
                }
            } else {
                for (int i = 0; i < arg_count; i++) {
                    int expected = base + 1 + i;
                    if (arg_regs[i] != expected)
                        emit(ENCODE_ABC(OP_MOVE, expected, arg_regs[i], 0), node->line);
                }
            }

            /* C = nnames + 1 means "expect nnames return values" */
            emit(ENCODE_ABC(is_method ? OP_MCALL : OP_CALL, base, total_args, nnames + 1), node->line);

            /* Results are in base, base+1, ..., base+nnames-1 */
            /* Bind each to a local (or global) */
            if (is_global) {
                int k0 = identifier_constant(current->vm, name);
                emit(ENCODE_ABx(OP_SETGLOBAL, base, k0), node->line);
                for (int i = 1; i < nnames; i++) {
                    int ki = identifier_constant(current->vm, &node->as.var_decl.extra_names[i - 1]);
                    emit(ENCODE_ABx(OP_SETGLOBAL, base + i, ki), node->line);
                }
            } else {
                add_local(*name, base, is_const);
                for (int i = 1; i < nnames; i++) {
                    /* Allocate register for each extra name. */
                    int r = base + i;
                    if (current->next_reg <= r) current->next_reg = r + 1;
                    add_local(node->as.var_decl.extra_names[i - 1], r, is_const);
                }
            }
        } else {
            /* Non-call initializer with multiple names: only first gets the value */
            int val = compile_node(init);
            int reg = alloc_reg();
            if (val != reg) emit(ENCODE_ABC(OP_MOVE, reg, val, 0), node->line);
            add_local(*name, reg, is_const);
            for (int i = 1; i < nnames; i++) {
                int r = alloc_reg();
                emit(ENCODE_ABC(OP_LOADNIL, r, 0, 0), node->line);
                add_local(node->as.var_decl.extra_names[i - 1], r, is_const);
            }
        }
        return;
    }

    /* Single variable (original path) */
    if (is_global) {
        int k = identifier_constant(current->vm, name);
        if (node->as.var_decl.initializer) {
            int val = compile_node(node->as.var_decl.initializer);
            emit(ENCODE_ABx(OP_SETGLOBAL, val, k), node->line);
        } else {
            int reg = alloc_reg();
            emit(ENCODE_ABC(OP_LOADNIL, reg, 0, 0), node->line);
            emit(ENCODE_ABx(OP_SETGLOBAL, reg, k), node->line);
        }
        /* Track const flag even for globals so reassignment is caught */
        if (is_const) {
            add_local(*name, -1, true);
        }
    } else {
        int reg = alloc_reg();
        if (node->as.var_decl.initializer) {
            if (current->error_handler_reg < 0 &&
                compile_node_into_reg(node->as.var_decl.initializer, reg)) {
                Local *local = add_local(*name, reg, is_const);
                if (local && node->as.var_decl.initializer->type == NODE_STRUCT_LITERAL) {
                    local->has_known_struct = true;
                    local->known_struct = node->as.var_decl.initializer->as.struct_literal.name;
                }
                return;
            }
            int val = compile_node(node->as.var_decl.initializer);
            if (val != reg) {
                emit(ENCODE_ABC(OP_MOVE, reg, val, 0), node->line);
            }
        } else {
            emit(ENCODE_ABC(OP_LOADNIL, reg, 0, 0), node->line);
        }
        Local *local = add_local(*name, reg, is_const);
        if (local && node->as.var_decl.initializer &&
            node->as.var_decl.initializer->type == NODE_STRUCT_LITERAL) {
            local->has_known_struct = true;
            local->known_struct = node->as.var_decl.initializer->as.struct_literal.name;
        }
    }
}

static void compile_if(ASTNode *node) {
    JumpPatch then_jump = compile_condition_jump(node->as.if_stmt.condition, node->line);

    begin_scope();
    compile_block(node->as.if_stmt.then_branch);
    end_scope(node->line);

    if (node->as.if_stmt.else_branch) {
        int else_jump = emit_jump(OP_JMP, node->line);
        patch_condition_jump(then_jump);
        begin_scope();
        if (node->as.if_stmt.else_branch->type == NODE_IF) {
            compile_if(node->as.if_stmt.else_branch);
        } else {
            compile_block(node->as.if_stmt.else_branch);
        }
        end_scope(node->line);
        patch_jump(else_jump);
    } else {
        patch_condition_jump(then_jump);
    }
}

static void add_break_jump(int offset) {
    if (current->break_count >= current->break_capacity) {
        current->break_capacity = current->break_capacity < 8 ? 8 : current->break_capacity * 2;
        current->break_jumps = realloc(current->break_jumps,
                                       sizeof(int) * current->break_capacity);
    }
    current->break_jumps[current->break_count++] = offset;
}

static void add_error_jump(int offset) {
    if (current->error_jump_count >= current->error_jump_capacity) {
        current->error_jump_capacity = current->error_jump_capacity < 4 ? 4 : current->error_jump_capacity * 2;
        current->error_jumps = realloc(current->error_jumps,
                                       sizeof(int) * current->error_jump_capacity);
    }
    current->error_jumps[current->error_jump_count++] = offset;
}

static bool node_contains_call(ASTNode *node) {
    if (!node) return false;
    switch (node->type) {
        case NODE_CALL:
            return true;
        case NODE_BINARY:
            return node_contains_call(node->as.binary.left) ||
                   node_contains_call(node->as.binary.right);
        case NODE_LOGICAL:
            return node_contains_call(node->as.logical.left) ||
                   node_contains_call(node->as.logical.right);
        case NODE_UNARY:
            return node_contains_call(node->as.unary.operand);
        case NODE_ASSIGN:
            return node_contains_call(node->as.assign.target) ||
                   node_contains_call(node->as.assign.value);
        case NODE_FIELD_SET:
            return node_contains_call(node->as.field_set.value);
        case NODE_INDEX:
            return true;
        case NODE_TRY:
            return true;
        case NODE_TRY_BLOCK:
            return node_contains_call(node->as.try_block.body);
        case NODE_TRY_CATCH:
            return node_contains_call(node->as.try_catch.body) ||
                   node_contains_call(node->as.try_catch.catch_body);
        case NODE_FIELD_GET:
            return node_contains_call(node->as.field_get.object);
        case NODE_EXPRESSION_STMT:
            return node_contains_call(node->as.expr_stmt.expr);
        case NODE_LET:
        case NODE_CONST:
            return node_contains_call(node->as.var_decl.initializer);
        case NODE_RETURN:
            for (int i = 0; i < node->as.return_stmt.values.count; i++) {
                if (node_contains_call(node->as.return_stmt.values.nodes[i])) return true;
            }
            return false;
        case NODE_IF:
            return node_contains_call(node->as.if_stmt.condition) ||
                   node_contains_call(node->as.if_stmt.then_branch) ||
                   node_contains_call(node->as.if_stmt.else_branch);
        case NODE_LOOP:
            return node_contains_call(node->as.loop_stmt.body);
        case NODE_FOR_RANGE:
            return node_contains_call(node->as.for_range.start) ||
                   node_contains_call(node->as.for_range.end) ||
                   node_contains_call(node->as.for_range.body);
        case NODE_FOR_IN:
            return node_contains_call(node->as.for_in.iterable) ||
                   node_contains_call(node->as.for_in.body);
        case NODE_BLOCK:
            for (int i = 0; i < node->as.block.stmts.count; i++) {
                if (node_contains_call(node->as.block.stmts.nodes[i])) return true;
            }
            return false;
        default:
            return false;
    }
}

static bool node_calls_tostring_of_name(ASTNode *node, Token *name) {
    if (!node) return false;
    switch (node->type) {
        case NODE_CALL: {
            if (node->as.call.callee->type == NODE_IDENTIFIER &&
                (is_tostring_name(&node->as.call.callee->as.identifier.name) ||
                 (node->as.call.callee->as.identifier.name.length == 18 &&
                  memcmp(node->as.call.callee->as.identifier.name.start, "__builtin_tostring", 18) == 0)) &&
                node->as.call.args.count == 1 &&
                node->as.call.args.nodes[0]->type == NODE_IDENTIFIER &&
                !node->as.call.args.nodes[0]->as.identifier.is_global &&
                identifiers_equal(&node->as.call.args.nodes[0]->as.identifier.name, name)) {
                return true;
            }
            if (node_calls_tostring_of_name(node->as.call.callee, name)) return true;
            for (int i = 0; i < node->as.call.args.count; i++) {
                if (node_calls_tostring_of_name(node->as.call.args.nodes[i], name)) return true;
            }
            return false;
        }
        case NODE_BINARY:
            return node_calls_tostring_of_name(node->as.binary.left, name) ||
                   node_calls_tostring_of_name(node->as.binary.right, name);
        case NODE_LOGICAL:
            return node_calls_tostring_of_name(node->as.logical.left, name) ||
                   node_calls_tostring_of_name(node->as.logical.right, name);
        case NODE_UNARY:
            return node_calls_tostring_of_name(node->as.unary.operand, name);
        case NODE_ASSIGN:
            return node_calls_tostring_of_name(node->as.assign.target, name) ||
                   node_calls_tostring_of_name(node->as.assign.value, name);
        case NODE_FIELD_SET:
            return node_calls_tostring_of_name(node->as.field_set.object, name) ||
                   node_calls_tostring_of_name(node->as.field_set.value, name);
        case NODE_INDEX:
            return node_calls_tostring_of_name(node->as.index_expr.object, name) ||
                   node_calls_tostring_of_name(node->as.index_expr.index, name);
        case NODE_TRY:
            return node_calls_tostring_of_name(node->as.try_expr.expr, name);
        case NODE_TRY_BLOCK:
            return node_calls_tostring_of_name(node->as.try_block.body, name);
        case NODE_TRY_CATCH:
            return node_calls_tostring_of_name(node->as.try_catch.body, name) ||
                   node_calls_tostring_of_name(node->as.try_catch.catch_body, name);
        case NODE_FIELD_GET:
            return node_calls_tostring_of_name(node->as.field_get.object, name);
        case NODE_EXPRESSION_STMT:
            return node_calls_tostring_of_name(node->as.expr_stmt.expr, name);
        case NODE_LET:
        case NODE_CONST:
            return node_calls_tostring_of_name(node->as.var_decl.initializer, name);
        case NODE_RETURN:
            for (int i = 0; i < node->as.return_stmt.values.count; i++) {
                if (node_calls_tostring_of_name(node->as.return_stmt.values.nodes[i], name)) return true;
            }
            return false;
        case NODE_IF:
            return node_calls_tostring_of_name(node->as.if_stmt.condition, name) ||
                   node_calls_tostring_of_name(node->as.if_stmt.then_branch, name) ||
                   node_calls_tostring_of_name(node->as.if_stmt.else_branch, name);
        case NODE_LOOP:
            return node_calls_tostring_of_name(node->as.loop_stmt.body, name);
        case NODE_FOR_RANGE:
            return node_calls_tostring_of_name(node->as.for_range.start, name) ||
                   node_calls_tostring_of_name(node->as.for_range.end, name) ||
                   node_calls_tostring_of_name(node->as.for_range.body, name);
        case NODE_FOR_IN:
            return node_calls_tostring_of_name(node->as.for_in.iterable, name) ||
                   node_calls_tostring_of_name(node->as.for_in.body, name);
        case NODE_BLOCK:
            for (int i = 0; i < node->as.block.stmts.count; i++) {
                if (node_calls_tostring_of_name(node->as.block.stmts.nodes[i], name)) return true;
            }
            return false;
        default:
            return false;
    }
}

static void compile_loop(ASTNode *node) {
    if (compile_array_mark_false_stride_loop(node)) {
        return;
    }

    /* Save outer loop state */
    int outer_start = current->loop_start;
    int *outer_breaks = current->break_jumps;
    int outer_break_count = current->break_count;
    int outer_break_cap = current->break_capacity;

    current->loop_start = current_chunk()->count;
    current->break_jumps = NULL;
    current->break_count = 0;
    current->break_capacity = 0;

    begin_scope();
    compile_block(node->as.loop_stmt.body);
    end_scope(node->line);

    /* Loop back */
    int loop_offset = current_chunk()->count - current->loop_start + 1;
    emit(ENCODE_sBx(OP_LOOP, loop_offset), node->line);

    /* Patch breaks */
    for (int i = 0; i < current->break_count; i++) {
        patch_jump(current->break_jumps[i]);
    }
    free(current->break_jumps);

    /* Restore outer loop state */
    current->loop_start = outer_start;
    current->break_jumps = outer_breaks;
    current->break_count = outer_break_count;
    current->break_capacity = outer_break_cap;
}

static void compile_for_range(ASTNode *node) {
    int outer_start = current->loop_start;
    int *outer_breaks = current->break_jumps;
    int outer_break_count = current->break_count;
    int outer_break_cap = current->break_capacity;
    current->break_jumps = NULL;
    current->break_count = 0;
    current->break_capacity = 0;

    begin_scope();

    int iter_reg = alloc_reg();
    int end_reg = alloc_reg();

    if (!compile_simple_into(node->as.for_range.start, iter_reg)) {
        int start_val = compile_node(node->as.for_range.start);
        emit(ENCODE_ABC(OP_MOVE, iter_reg, start_val, 0), node->line);
        current->next_reg = end_reg + 1;
    }
    add_local(node->as.for_range.var, iter_reg, false);

    if (!compile_simple_into(node->as.for_range.end, end_reg)) {
        int end_val = compile_node(node->as.for_range.end);
        emit(ENCODE_ABC(OP_MOVE, end_reg, end_val, 0), node->line);
        current->next_reg = end_reg + 1;
    }

    if (compile_for_range_mod_accumulator(node, iter_reg, end_reg) ||
        compile_for_range_field2_accumulator(node, iter_reg, end_reg) ||
        compile_for_range_field_accumulator(node, iter_reg, end_reg)) {
        free(current->break_jumps);
        end_scope(node->line);

        current->loop_start = outer_start;
        current->break_jumps = outer_breaks;
        current->break_count = outer_break_count;
        current->break_capacity = outer_break_cap;
        return;
    }

    bool numeric_range = node_contains_call(node->as.for_range.body) &&
                         !node_calls_tostring_of_name(node->as.for_range.body, &node->as.for_range.var);
    emit(ENCODE_ABC(numeric_range ? OP_FORPREP_NUM : OP_FORPREP,
                    iter_reg, 0, 0), node->line);
    current->loop_start = current_chunk()->count;

    OpCode for_op = node->as.for_range.inclusive
        ? (numeric_range ? OP_FORLOOP_INC_NUM : OP_FORLOOP_INC)
        : (numeric_range ? OP_FORLOOP_NUM : OP_FORLOOP);
    int exit_jump = emit_asbx_jump(for_op, iter_reg, node->line);

    compile_block(node->as.for_range.body);

    int body_end = current_chunk()->count;
    int loop_offset = body_end - current->loop_start + 1;

    bool fused = false;
    if (body_end > current->loop_start) {
        Instruction last = current_chunk()->code[body_end - 1];
        if (GET_OPCODE(last) == OP_ADDI && GET_A(last) == GET_B(last) && loop_offset <= 255) {
            for (int i = current->loop_start; i < body_end - 1; i++) {
                Instruction inst = current_chunk()->code[i];
                if (GET_OPCODE(inst) == OP_JMP) {
                    int target = i + 1 + GET_sBx(inst);
                    if (target == body_end) {
                        current_chunk()->code[i] = ENCODE_sBx(OP_JMP, current->loop_start - (i + 1));
                    }
                }
            }
            current_chunk()->code[body_end - 1] = ENCODE_ABC(OP_ADDI_LOOP, GET_A(last), GET_C(last) & 0xFF, loop_offset - 1);
            fused = true;
        }
    }

    if (!fused) {
        for (int i = current->loop_start; i < body_end; i++) {
            Instruction inst = current_chunk()->code[i];
            if (GET_OPCODE(inst) == OP_JMP) {
                int target = i + 1 + GET_sBx(inst);
                if (target == body_end) {
                    current_chunk()->code[i] = ENCODE_sBx(OP_JMP, current->loop_start - (i + 1));
                }
            }
        }
        emit(ENCODE_sBx(OP_LOOP, loop_offset), node->line);
    }

    patch_asbx_jump(exit_jump);

    for (int i = 0; i < current->break_count; i++) {
        patch_jump(current->break_jumps[i]);
    }
    free(current->break_jumps);

    end_scope(node->line);

    current->loop_start = outer_start;
    current->break_jumps = outer_breaks;
    current->break_count = outer_break_count;
    current->break_capacity = outer_break_cap;
}

static void compile_for_in(ASTNode *node) {
    int outer_start = current->loop_start;
    int *outer_breaks = current->break_jumps;
    int outer_break_count = current->break_count;
    int outer_break_cap = current->break_capacity;
    current->break_jumps = NULL;
    current->break_count = 0;
    current->break_capacity = 0;

    begin_scope();

    int iterable_src = compile_node(node->as.for_in.iterable);
    /* Reserve two contiguous registers: [iterable_copy, iter_state] */
    int iterable_reg = alloc_reg();
    int iter_state = alloc_reg();
    /* Copy iterable into the slot just before iter_state */
    if (iterable_src != iterable_reg) {
        emit(ENCODE_ABC(OP_MOVE, iterable_reg, iterable_src, 0), node->line);
    }
    emit(ENCODE_ABC(OP_ITER_PREP, iter_state, iterable_reg, 0), node->line);

    int var_reg = alloc_reg();
    add_local(node->as.for_in.var, var_reg, false);

    int var2_reg = -1;
    if (node->as.for_in.has_var2) {
        var2_reg = alloc_reg();
        add_local(node->as.for_in.var2, var2_reg, false);
    }

    current->loop_start = current_chunk()->count;

    /* Get next value*/
    int jump_dist_slot = current_chunk()->count;
    emit(ENCODE_ABC(OP_ITER_NEXT, var_reg, iter_state, 0), node->line);

    compile_block(node->as.for_in.body);

    int loop_offset = current_chunk()->count - current->loop_start + 1;
    emit(ENCODE_sBx(OP_LOOP, loop_offset), node->line);

    /* Patch the ITER_NEXT jump distance */
    int jump_over = current_chunk()->count - jump_dist_slot - 1;
    current_chunk()->code[jump_dist_slot] =
        ENCODE_ABC(OP_ITER_NEXT, var_reg, iter_state, jump_over);

    for (int i = 0; i < current->break_count; i++) {
        patch_jump(current->break_jumps[i]);
    }
    free(current->break_jumps);

    end_scope(node->line);

    current->loop_start = outer_start;
    current->break_jumps = outer_breaks;
    current->break_count = outer_break_count;
    current->break_capacity = outer_break_cap;
}

static void compile_return(ASTNode *node) {
    int count = node->as.return_stmt.values.count;
    if (count == 0) {
        int reg = alloc_reg();
        emit(ENCODE_ABC(OP_LOADNIL, reg, 0, 0), node->line);
        emit(ENCODE_ABC(OP_RETURN, reg, 1, 0), node->line);
    } else {
        int first = compile_node(node->as.return_stmt.values.nodes[0]);
        for (int i = 1; i < count; i++) {
            int r = compile_node(node->as.return_stmt.values.nodes[i]);
            int expected = first + i;
            if (r != expected) {
                emit(ENCODE_ABC(OP_MOVE, expected, r, 0), node->line);
            }
        }
        emit(ENCODE_ABC(OP_RETURN, first, count, 0), node->line);
    }
}

static void compile_block_to_value(ASTNode *block, int result_reg, int line) {
    if (!block || block->type != NODE_BLOCK || block->as.block.stmts.count == 0) {
        emit(ENCODE_ABC(OP_LOADNIL, result_reg, 0, 0), line);
        return;
    }

    int count = block->as.block.stmts.count;
    for (int i = 0; i < count - 1; i++) {
        compile_stmt(block->as.block.stmts.nodes[i]);
    }

    ASTNode *last = block->as.block.stmts.nodes[count - 1];
    if (last->type == NODE_EXPRESSION_STMT) {
        int value = compile_node(last->as.expr_stmt.expr);
        if (value != result_reg) {
            emit(ENCODE_ABC(OP_MOVE, result_reg, value, 0), last->line);
        }
    } else {
        compile_stmt(last);
        emit(ENCODE_ABC(OP_LOADNIL, result_reg, 0, 0), line);
    }
}

static int compile_try_block(ASTNode *node) {
    int result_reg = alloc_reg();
    int err_reg = alloc_reg();
    emit(ENCODE_ABC(OP_LOADNIL, result_reg, 0, 0), node->line);
    emit(ENCODE_ABC(OP_LOADNIL, err_reg, 0, 0), node->line);

    int outer_handler = current->error_handler_reg;
    int *outer_jumps = current->error_jumps;
    int outer_jump_count = current->error_jump_count;
    int outer_jump_capacity = current->error_jump_capacity;

    current->error_handler_reg = err_reg;
    current->error_jumps = NULL;
    current->error_jump_count = 0;
    current->error_jump_capacity = 0;

    begin_scope();
    compile_block_to_value(node->as.try_block.body, result_reg, node->line);
    end_scope(node->line);

    emit(ENCODE_ABC(OP_LOADNIL, err_reg, 0, 0), node->line);
    int done_jump = emit_jump(OP_JMP, node->line);

    for (int i = 0; i < current->error_jump_count; i++) {
        patch_jump(current->error_jumps[i]);
    }
    free(current->error_jumps);

    emit(ENCODE_ABC(OP_LOADNIL, result_reg, 0, 0), node->line);

    current->error_handler_reg = outer_handler;
    current->error_jumps = outer_jumps;
    current->error_jump_count = outer_jump_count;
    current->error_jump_capacity = outer_jump_capacity;

    patch_jump(done_jump);
    return result_reg;
}

static void compile_try_catch(ASTNode *node) {
    int err_reg = alloc_reg();

    int outer_handler = current->error_handler_reg;
    int *outer_jumps = current->error_jumps;
    int outer_jump_count = current->error_jump_count;
    int outer_jump_capacity = current->error_jump_capacity;

    current->error_handler_reg = err_reg;
    current->error_jumps = NULL;
    current->error_jump_count = 0;
    current->error_jump_capacity = 0;

    begin_scope();
    compile_block(node->as.try_catch.body);
    end_scope(node->line);

    int done_jump = emit_jump(OP_JMP, node->line);

    for (int i = 0; i < current->error_jump_count; i++) {
        patch_jump(current->error_jumps[i]);
    }
    free(current->error_jumps);

    current->error_handler_reg = outer_handler;
    current->error_jumps = outer_jumps;
    current->error_jump_count = outer_jump_count;
    current->error_jump_capacity = outer_jump_capacity;

    begin_scope();
    add_local(node->as.try_catch.err_name, err_reg, true);
    compile_block(node->as.try_catch.catch_body);
    end_scope(node->line);

    patch_jump(done_jump);
}

static void compile_defer(ASTNode *node) {
    /* Store the deferred call expression for emission at scope exit */
    if (current->defer_count >= current->defer_capacity) {
        current->defer_capacity = current->defer_capacity < 8 ? 8 : current->defer_capacity * 2;
        current->defers = realloc(current->defers,
            sizeof(DeferEntry) * current->defer_capacity);
    }
    current->defers[current->defer_count].expr = node->as.defer_stmt.call;
    current->defers[current->defer_count].depth = current->scope_depth;
    current->defer_count++;
}

static void compile_fn_decl(ASTNode *node) {
    int fn_binding_reg = -1;
    bool top_level_global = false;

    if (!node->as.fn_decl.is_method && node->as.fn_decl.name.length > 0) {
        /* Named function: register name early to support recursion */
        if (current->scope_depth == 0) {
            if (top_level_global) {
                add_global(node->as.fn_decl.name);
            }
            fn_binding_reg = alloc_reg();
            add_local(node->as.fn_decl.name, fn_binding_reg, true);
            emit(ENCODE_ABC(OP_LOADNIL, fn_binding_reg, 0, 0), node->line);
        } else {
            /* Local: reserve register now, but it will be populated by compile_function */
            fn_binding_reg = alloc_reg();
            add_local(node->as.fn_decl.name, fn_binding_reg, false);
            emit(ENCODE_ABC(OP_LOADNIL, fn_binding_reg, 0, 0), node->line);
        }
    }

    int fn_reg = compile_function(node, node->as.fn_decl.is_method ? FUNC_METHOD : FUNC_FUNCTION);

    if (node->as.fn_decl.is_method) {
        /* Method: store in the struct's method table. */
        int struct_k = identifier_constant(current->vm, &node->as.fn_decl.method_struct);
        int struct_reg = alloc_reg();
        int local = resolve_local(current, &node->as.fn_decl.method_struct);
        if (local != -1) {
            emit(ENCODE_ABC(OP_MOVE, struct_reg, local, 0), node->line);
        } else {
            int upval = resolve_upvalue(current, &node->as.fn_decl.method_struct);
            if (upval != -1) {
                emit(ENCODE_ABC(OP_GETUPVAL, struct_reg, upval, 0), node->line);
            } else {
                emit(ENCODE_ABx(OP_GETGLOBAL, struct_reg, struct_k), node->line);
            }
        }

        int method_k = identifier_constant(current->vm, &node->as.fn_decl.name);
        emit(ENCODE_ABC(OP_SETFIELD, struct_reg, method_k, fn_reg), node->line);
        register_field_loop_candidate(node);
    } else if (node->as.fn_decl.name.length > 0) {
        /* Named function: bind to global or local */
        if (top_level_global) {
            int k = identifier_constant(current->vm, &node->as.fn_decl.name);
            if (fn_binding_reg != -1 && fn_reg != fn_binding_reg) {
                emit(ENCODE_ABC(OP_MOVE, fn_binding_reg, fn_reg, 0), node->line);
            }
            emit(ENCODE_ABx(OP_SETGLOBAL, fn_binding_reg != -1 ? fn_binding_reg : fn_reg, k), node->line);
        } else {
            if (fn_binding_reg != -1 && fn_reg != fn_binding_reg) {
                emit(ENCODE_ABC(OP_MOVE, fn_binding_reg, fn_reg, 0), node->line);
            } else {
                add_local(node->as.fn_decl.name, fn_reg, false);
            }
        }
    }

    if (!node->as.fn_decl.is_method && node->as.fn_decl.name.length > 0 &&
        current->scope_depth == 0) {
        register_inline_candidate(node, fn_binding_reg != -1 ? fn_binding_reg : fn_reg);
    }
}

static void compile_struct_decl(ASTNode *node) {
    bool top_level_global = false;
    if (top_level_global) {
        add_global(node->as.struct_decl.name);
    }

    int name_k = identifier_constant(current->vm, &node->as.struct_decl.name);
    int dest = alloc_reg();
    if (!top_level_global) {
        add_local(node->as.struct_decl.name, dest, true);
    }

    /* OP_NEWSTRUCT A Bx: R[A] = new struct named K[Bx] */
    emit(ENCODE_ABx(OP_NEWSTRUCT, dest, name_k), node->line);

    /* Emit field count as pseudo-instruction */
    emit(ENCODE_ABC(node->as.struct_decl.field_count, 0, 0, 0), node->line);

    /* Emit field name constant indices */
    for (int i = 0; i < node->as.struct_decl.field_count; i++) {
        int field_k = identifier_constant(current->vm, &node->as.struct_decl.fields[i].name);
        emit(ENCODE_ABC(field_k, 0, 0, 0), node->line);
    }

    if (top_level_global) {
        emit(ENCODE_ABx(OP_SETGLOBAL, dest, name_k), node->line);
    }
}

static void compile_enum_decl(ASTNode *node) {
    bool top_level_global = false;
    if (top_level_global) {
        add_global(node->as.enum_decl.name);
    }

    /* Compile enum as a dict: Status = { Ok = 0, Error = 1, Pending = 2 } */
    int name_k = identifier_constant(current->vm, &node->as.enum_decl.name);
    int dest = alloc_reg();
    if (!top_level_global) {
        add_local(node->as.enum_decl.name, dest, true);
    }
    emit(ENCODE_ABC(OP_NEWDICT, dest, 0, 0), node->line);

    for (int i = 0; i < node->as.enum_decl.variant_count; i++) {
        /* Key: variant name as string in a register */
        int vk = string_constant(current->vm,
            node->as.enum_decl.variants[i].start,
            node->as.enum_decl.variants[i].length);
        int key_reg = alloc_reg();
        emit(ENCODE_ABx(OP_LOADK, key_reg, vk), node->line);

        /* Value: integer index */
        int val_reg = alloc_reg();
        int ik = make_constant(NUMBER_VAL((double)i));
        emit(ENCODE_ABx(OP_LOADK, val_reg, ik), node->line);

        emit(ENCODE_ABC(OP_SETINDEX, dest, key_reg, val_reg), node->line);
    }

    if (top_level_global) {
        emit(ENCODE_ABx(OP_SETGLOBAL, dest, name_k), node->line);
    }
}

static void canonicalize_import_path(char *path, int capacity) {
    char full[4096];
#ifdef _WIN32
    if (_fullpath(full, path, sizeof(full)) != NULL) {
        size_t len = strlen(full);
        if (len < (size_t)capacity) memcpy(path, full, len + 1);
    }
#else
    if (realpath(path, full) != NULL) {
        size_t len = strlen(full);
        if (len < (size_t)capacity) memcpy(path, full, len + 1);
    }
#endif
}

static bool read_import_file(Token *path, char *resolved_path, int resolved_capacity,
                             char **source_out) {
    char mod_path[1024];
    int mod_len = path->length - 2;
    if (mod_len < 0 || mod_len >= (int)sizeof(mod_path)) {
        error_at(path, "Module path is too long.");
        return false;
    }

    memcpy(mod_path, path->start + 1, mod_len);
    mod_path[mod_len] = '\0';

    FILE *f = fopen(mod_path, "rb");
    if (f) {
        snprintf(resolved_path, resolved_capacity, "%s", mod_path);
    } else {
        char full[1024];
        if (mod_len > (int)sizeof(full) - 4) {
            error_at(path, "Module path is too long.");
            return false;
        }
        memcpy(full, mod_path, mod_len);
        memcpy(full + mod_len, ".mg", 4);
        f = fopen(full, "rb");
        if (f) {
            snprintf(resolved_path, resolved_capacity, "%s", full);
        }
    }

    if (!f) {
        error_at(path, "Cannot open module file.");
        return false;
    }
    if (fseek(f, 0L, SEEK_END) != 0) {
        fclose(f);
        error_at(path, "Cannot read module file.");
        return false;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        error_at(path, "Cannot read module file.");
        return false;
    }
    rewind(f);

    char *source = (char *)malloc((size_t)sz + 1);
    if (!source) {
        fclose(f);
        error_at(path, "Out of memory reading module file.");
        return false;
    }
    size_t read = fread(source, 1, (size_t)sz, f);
    fclose(f);
    if (read != (size_t)sz) {
        free(source);
        error_at(path, "Cannot read module file.");
        return false;
    }
    source[sz] = '\0';
    *source_out = source;
    canonicalize_import_path(resolved_path, resolved_capacity);
    return true;
}

static int reserve_import_alias(Token *alias, bool *is_global) {
    *is_global = false;

    int reg = alloc_reg();
    emit(ENCODE_ABC(OP_LOADNIL, reg, 0, 0), alias->line);
    add_local(*alias, reg, true);
    return reg;
}

static void bind_import_alias(Token *alias, bool is_global, int alias_reg,
                              int value_reg, int line) {
    if (is_global) {
        int alias_k = identifier_constant(current->vm, alias);
        emit(ENCODE_ABx(OP_SETGLOBAL, value_reg, alias_k), line);
    } else if (alias_reg != value_reg) {
        emit(ENCODE_ABC(OP_MOVE, alias_reg, value_reg, 0), line);
    }
}

/* ========================================================================
 * compile_stmt / compile_block / compile entry point
 * ======================================================================== */
static void compile_stmt(ASTNode *node) {
    /* Save register watermark. temps from statements can be reclaimed */
    int saved_reg = current->next_reg;

    switch (node->type) {
        case NODE_EXPRESSION_STMT:
            /* If the inner expression is an assignment or field_set, compile as statement */
            if (node->as.expr_stmt.expr->type == NODE_ASSIGN) {
                compile_assign(node->as.expr_stmt.expr);
            } else if (node->as.expr_stmt.expr->type == NODE_FIELD_SET) {
                compile_field_set(node->as.expr_stmt.expr);
            } else if (node->as.expr_stmt.expr->type == NODE_CALL) {
                compile_call_with_expected(node->as.expr_stmt.expr, 0);
            } else {
                compile_node(node->as.expr_stmt.expr);
            }
            /* Reclaim temp registers used by expression statement */
            current->next_reg = saved_reg;
            break;
        case NODE_LET:
        case NODE_CONST:
            compile_var_decl(node);
            break;
        case NODE_ASSIGN:
            compile_assign(node);
            break;
        case NODE_FIELD_SET:
            compile_field_set(node);
            break;
        case NODE_IF:
            compile_if(node);
            break;
        case NODE_LOOP:
            compile_loop(node);
            break;
        case NODE_FOR_RANGE:
            compile_for_range(node);
            break;
        case NODE_FOR_IN:
            compile_for_in(node);
            break;
        case NODE_RETURN:
            compile_return(node);
            break;
        case NODE_TRY_CATCH:
            compile_try_catch(node);
            break;
        case NODE_DEFER:
            compile_defer(node);
            break;
        case NODE_FN_DECL:
            compile_fn_decl(node);
            break;
        case NODE_STRUCT_DECL:
            compile_struct_decl(node);
            break;
        case NODE_ENUM_DECL:
            compile_enum_decl(node);
            break;
        case NODE_BREAK:
            if (current->loop_start == -1) {
                Token err_tok = {TOKEN_BREAK, "break", 5, node->line};
                error_at(&err_tok, "'break' outside of loop.");
            } else {
                int jump = emit_jump(OP_JMP, node->line);
                add_break_jump(jump);
            }
            break;
        case NODE_CONTINUE:
            if (current->loop_start == -1) {
                Token err_tok = {TOKEN_CONTINUE, "continue", 8, node->line};
                error_at(&err_tok, "'continue' outside of loop.");
            } else {
                int loop_offset = current_chunk()->count - current->loop_start + 1;
                emit(ENCODE_sBx(OP_LOOP, loop_offset), node->line);
            }
            break;
        case NODE_BLOCK:
            begin_scope();
            compile_block(node);
            end_scope(node->line);
            break;
        case NODE_IMPORT: {
            /* import "path" [as alias]
               Compile the module now, but load/cache its returned export object at runtime. */
            Token *path = &node->as.import_stmt.path;
            Token alias_token;

            if (node->as.import_stmt.has_alias) {
                alias_token = node->as.import_stmt.alias;
            } else {
                char mod_path_raw[1024];
                snprintf(mod_path_raw, sizeof(mod_path_raw), "%.*s", path->length - 2, path->start + 1);
                char *last_slash = strrchr(mod_path_raw, '/');
                char *alias_start = last_slash ? last_slash + 1 : mod_path_raw;
                int alias_len = (int)strlen(alias_start);
                char *dot = strrchr(alias_start, '.');
                if (dot) alias_len = (int)(dot - alias_start);
                char *alias_buf = (char *)malloc(alias_len + 1);
                if (!alias_buf) {
                    error_at(path, "Out of memory creating import alias.");
                    break;
                }
                memcpy(alias_buf, alias_start, alias_len);
                alias_buf[alias_len] = '\0';
                if (!compiler_own_string(alias_buf)) {
                    free(alias_buf);
                    error_at(path, "Out of memory creating import alias.");
                    break;
                }
                alias_token.start = alias_buf;
                alias_token.length = alias_len;
                alias_token.line = path->line;
            }
            Token *alias = &alias_token;
            bool alias_is_global = false;
            int alias_reg = reserve_import_alias(alias, &alias_is_global);

            char resolved_path[1024];
            char *source = NULL;
            if (!read_import_file(path, resolved_path, (int)sizeof(resolved_path), &source)) {
                break;
            }

            int cache_k = string_constant(current->vm, "@modules", 8);
            int cache_reg = alloc_reg();
            emit(ENCODE_ABx(OP_GETGLOBAL, cache_reg, cache_k), node->line);

            int path_k = string_constant(current->vm, resolved_path, (int)strlen(resolved_path));
            int path_reg = alloc_reg();
            emit(ENCODE_ABx(OP_LOADK, path_reg, path_k), node->line);

            int result_reg = alloc_reg();
            emit(ENCODE_ABC(OP_GETINDEX, result_reg, cache_reg, path_reg), node->line);
            int cache_miss_jump = emit_test_jump(result_reg, node->line);
            int cache_hit_done_jump = emit_jump(OP_JMP, node->line);
            patch_test_jump(cache_miss_jump);

            int len = (int)strlen(resolved_path);
            bool is_header = (len > 2 && resolved_path[len - 2] == '.' && resolved_path[len - 1] == 'h');

            if (is_header) {
                emit(ENCODE_ABC(OP_NEWDICT, result_reg, 0, 0), node->line);

                char *p = source;
                while (*p) {
                    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') p++;
                    if (!*p) break;
                    if (strncmp(p, "double ", 7) == 0) {
                        p += 7;
                        while (*p == ' ') p++;
                        char *name_start = p;
                        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') p++;
                        int name_len = p - name_start;
                        if (name_len > 0) {
                            while (*p == ' ') p++;
                            if (*p == '(') {
                                p++;
                                int arity = 0;
                                while (*p && *p != ')') {
                                    if (strncmp(p, "double", 6) == 0) {
                                        arity++;
                                        p += 6;
                                    } else {
                                        p++;
                                    }
                                }
                                /* Call __ffi_bind */
                                int bind_k = string_constant(current->vm, "__ffi_bind", 10);
                                int bind_reg = alloc_reg();
                                emit(ENCODE_ABx(OP_GETGLOBAL, bind_reg, bind_k), node->line);

                                int name_k = string_constant(current->vm, name_start, name_len);
                                int arg1_reg = alloc_reg();
                                emit(ENCODE_ABx(OP_LOADK, arg1_reg, name_k), node->line);

                                int arity_k = make_constant(NUMBER_VAL(arity));
                                int arg2_reg = alloc_reg();
                                emit(ENCODE_ABx(OP_LOADK, arg2_reg, arity_k), node->line);

                                /* __ffi_bind is native arity 2 */
                                emit(ENCODE_ABC(OP_CALL, bind_reg, 2, 2), node->line);

                                /* bind_reg now holds the OBJ_FFI. Add to the module export dict. */
                                int name_reg_dyn = alloc_reg();
                                emit(ENCODE_ABx(OP_LOADK, name_reg_dyn, name_k), node->line);
                                emit(ENCODE_ABC(OP_SETINDEX, result_reg, name_reg_dyn, bind_reg), node->line);
                            }
                        }
                    } else {
                        while (*p && *p != '\n') p++;
                    }
                }
                emit(ENCODE_ABC(OP_SETINDEX, cache_reg, path_reg, result_reg), node->line);
                patch_jump(cache_hit_done_jump);
                bind_import_alias(alias, alias_is_global, alias_reg, result_reg, node->line);
                current->next_reg = alias_is_global ? saved_reg : alias_reg + 1;
                free(source);
                break;
            }

            /* Parse and compile the module into an ObjFunction */
            bool mod_error = false;
            ASTNode *mod_ast = parse(source, &mod_error);
            if (mod_error) {
                error_at(path, "Errors in module.");
                ast_free(mod_ast);
                free(source);
                break;
            }
            if (!mg_typecheck_ast(mod_ast, false)) {
                error_at(path, "Type errors in module.");
                ast_free(mod_ast);
                free(source);
                break;
            }
            optimize_ast(mod_ast);
            Compiler *saved_compiler = current;
            current = NULL;
            ObjFunction *mod_fn = compile_with_options(saved_compiler->vm, mod_ast, true,
                                                       resolved_path, (int)strlen(resolved_path));
            current = saved_compiler;
            ast_free(mod_ast);
            free(source);
            if (!mod_fn) {
                error_at(path, "Module compilation failed.");
                break;
            }

            /* Emit: create closure from the compiled module function */
            int mod_k = make_constant(OBJ_VAL(mod_fn));
            int closure_reg = alloc_reg();
            emit(ENCODE_ABx(OP_CLOSURE, closure_reg, mod_k), node->line);

            /* Emit: call the module closure (no args, 1 result) */
            emit(ENCODE_ABC(OP_MOVE, result_reg, closure_reg, 0), node->line);
            emit(ENCODE_ABC(OP_CALL, result_reg, 0, 2), node->line);
            emit(ENCODE_ABC(OP_SETINDEX, cache_reg, path_reg, result_reg), node->line);

            patch_jump(cache_hit_done_jump);
            bind_import_alias(alias, alias_is_global, alias_reg, result_reg, node->line);
            current->next_reg = alias_is_global ? saved_reg : alias_reg + 1;
            break;
        }
        case NODE_EXPORT: {
            /* export <declaration>
               Module chunks keep exports in a hidden register and return it. */
            if (current->module_exports_reg < 0) {
                Token err_tok = {TOKEN_EXPORT, "export", 6, node->line};
                error_at(&err_tok, "'export' is only valid inside an imported module.");
                break;
            }

            ASTNode *decl = node->as.export_stmt.declaration;
            compile_stmt(decl);

            /* Get the name of what was exported */
            Token *name = NULL;
            if (decl->type == NODE_FN_DECL) name = &decl->as.fn_decl.name;
            else if (decl->type == NODE_LET || decl->type == NODE_CONST) name = &decl->as.var_decl.name;
            else if (decl->type == NODE_STRUCT_DECL) name = &decl->as.struct_decl.name;
            else if (decl->type == NODE_ENUM_DECL) name = &decl->as.enum_decl.name;

            if (name) {
                int val_reg = resolve_local(current, name);
                int val_k = identifier_constant(current->vm, name);
                if (val_reg == -1) {
                    int upval = resolve_upvalue(current, name);
                    if (upval != -1) {
                        val_reg = alloc_reg();
                        emit(ENCODE_ABC(OP_GETUPVAL, val_reg, upval, 0), node->line);
                    } else {
                        val_reg = alloc_reg();
                        emit(ENCODE_ABx(OP_GETGLOBAL, val_reg, val_k), node->line);
                    }
                }

                int name_reg = alloc_reg();
                emit(ENCODE_ABx(OP_LOADK, name_reg, val_k), node->line);
                emit(ENCODE_ABC(OP_SETINDEX, current->module_exports_reg, name_reg, val_reg), node->line);
            }
            break;
        }
        case NODE_EXTERN_DECL:
            add_global(node->as.extern_decl.name);
            break;
        case NODE_DIRECTIVE:
        case NODE_TYPE_ALIAS:
            /* Static/tooling-only nodes do not emit bytecode. */
            break;
        default:
            /* Try as expression */
            compile_node(node);
            break;
    }
}

static void compile_block(ASTNode *node) {
    for (int i = 0; i < node->as.block.stmts.count; i++) {
        ASTNode *stmt = node->as.block.stmts.nodes[i];
        compile_stmt(stmt);
        /* Dead code elimination: skip unreachable statements after return/break */
        if (stmt->type == NODE_RETURN || stmt->type == NODE_BREAK || stmt->type == NODE_CONTINUE) {
            break;
        }
    }
}

static ObjFunction *compile_with_options(VM *vm, ASTNode *ast,
                                         bool module_mode,
                                         const char *name,
                                         int name_len) {
    Compiler compiler;
    Token script_name;
    if (name && name_len > 0) {
        script_name = (Token){TOKEN_IDENTIFIER, name, name_len, 0};
    } else {
        script_name = (Token){TOKEN_IDENTIFIER, "<script>", 8, 0};
    }
    compiler_init(&compiler, vm, FUNC_SCRIPT, &script_name);
    compiler.module_mode = module_mode;

    /* Initialize and pre-populate global table */
    register_builtins();
    register_vm_globals();

    if (module_mode) {
        Token exports_token = {TOKEN_IDENTIFIER, "", 0, 0};
        compiler.module_exports_reg = alloc_reg();
        add_local(exports_token, compiler.module_exports_reg, true);
        emit(ENCODE_ABC(OP_NEWDICT, compiler.module_exports_reg, 0, 0), 0);
    }

    /* Compile top-level block */
    compile_block(ast);

    /* Execute top-level defers in LIFO order before exit */
    for (int i = compiler.defer_count - 1; i >= 0; i--) {
        compile_node(compiler.defers[i].expr);
    }
    compiler.defer_count = 0;

    if (module_mode) {
        emit(ENCODE_ABC(OP_RETURN, compiler.module_exports_reg, 1, 0), 0);
    } else {
        /* Implicit return nil at end of script */
        int nil_reg = alloc_reg();
        emit(ENCODE_ABC(OP_LOADNIL, nil_reg, 0, 0), 0);
        emit(ENCODE_ABC(OP_RETURN, nil_reg, 1, 0), 0);
    }

    ObjFunction *fn = compiler.function;
    fn->reg_count = compiler.max_reg;  /* actual register count for stack sizing */
    current = compiler.enclosing;
    free(compiler.break_jumps);
    free(compiler.defers);
    free(compiler.globals);
    free(compiler.inline_candidates);
    free(compiler.field_loop_candidates);
    free_compiler_owned_strings(&compiler);

    if (compiler.had_error) return NULL;
    return fn;
}

ObjFunction *compile(VM *vm, ASTNode *ast) {
    return compile_with_options(vm, ast, false, "<script>", 8);
}

ObjFunction *compile_named(VM *vm, ASTNode *ast, const char *name, int name_len) {
    return compile_with_options(vm, ast, false, name, name_len);
}

/* ========================================================================
 * AST Optimizer - Constant Folding
 * Pre-computes constant expressions at compile time.
 * ======================================================================== */
static ASTNode *fold_node(ASTNode *node);

static ASTNode *fold_node(ASTNode *node) {
    if (!node) return node;

    switch (node->type) {
        case NODE_UNARY: {
            node->as.unary.operand = fold_node(node->as.unary.operand);
            ASTNode *op = node->as.unary.operand;
            if (node->as.unary.op == TOKEN_MINUS && op->type == NODE_NUMBER) {
                double val = -op->as.number.value;
                node->type = NODE_NUMBER;
                node->as.number.value = val;
                ast_free(op);
            } else if (node->as.unary.op == TOKEN_BANG && op->type == NODE_BOOL) {
                bool val = !op->as.boolean.value;
                node->type = NODE_BOOL;
                node->as.boolean.value = val;
                ast_free(op);
            }
            break;
        }
        case NODE_BINARY: {
            node->as.binary.left = fold_node(node->as.binary.left);
            node->as.binary.right = fold_node(node->as.binary.right);
            ASTNode *l = node->as.binary.left;
            ASTNode *r = node->as.binary.right;

            /* Fold number op number */
            if (l->type == NODE_NUMBER && r->type == NODE_NUMBER) {
                double a = l->as.number.value;
                double b = r->as.number.value;
                double result = 0;
                bool is_bool = false;
                bool bool_result = false;

                switch (node->as.binary.op) {
                    case TOKEN_PLUS:          result = a + b; break;
                    case TOKEN_MINUS:         result = a - b; break;
                    case TOKEN_STAR:          result = a * b; break;
                    case TOKEN_SLASH:
                        if (b == 0) goto no_fold;
                        result = a / b;
                        break;
                    case TOKEN_PERCENT:
                        if (b == 0) goto no_fold;
                        result = fmod(a, b);
                        break;
                    case TOKEN_EQUAL_EQUAL:   is_bool = true; bool_result = (a == b); break;
                    case TOKEN_BANG_EQUAL:     is_bool = true; bool_result = (a != b); break;
                    case TOKEN_LESS:           is_bool = true; bool_result = (a < b); break;
                    case TOKEN_LESS_EQUAL:     is_bool = true; bool_result = (a <= b); break;
                    case TOKEN_GREATER:        is_bool = true; bool_result = (a > b); break;
                    case TOKEN_GREATER_EQUAL:  is_bool = true; bool_result = (a >= b); break;
                    default: goto no_fold;
                }

                ast_free(l);
                ast_free(r);
                if (is_bool) {
                    node->type = NODE_BOOL;
                    node->as.boolean.value = bool_result;
                } else {
                    node->type = NODE_NUMBER;
                    node->as.number.value = result;
                }
            }
            /* Fold string + string */
            else if (l->type == NODE_STRING && r->type == NODE_STRING &&
                     node->as.binary.op == TOKEN_PLUS) {
                int new_len = l->as.string.length + r->as.string.length;
                char *new_str = (char *)malloc(new_len + 1);
                memcpy(new_str, l->as.string.value, l->as.string.length);
                memcpy(new_str + l->as.string.length, r->as.string.value, r->as.string.length);
                new_str[new_len] = '\0';

                ast_free(l);
                ast_free(r);
                node->type = NODE_STRING;
                node->as.string.value = new_str;
                node->as.string.length = new_len;
            }
            no_fold:
            break;
        }
        case NODE_LOGICAL: {
            node->as.logical.left = fold_node(node->as.logical.left);
            node->as.logical.right = fold_node(node->as.logical.right);
            ASTNode *l = node->as.logical.left;
            ASTNode *r = node->as.logical.right;
            if (l->type == NODE_BOOL) {
                if (node->as.logical.op == TOKEN_AND) {
                    /* true and x -> x, false and x -> false */
                    if (l->as.boolean.value) {
                        /* true and x -> x */
                        ast_free(l);
                        *node = *r;
                        free(r);
                    } else {
                        /* false and x -> false */
                        ast_free(l);
                        ast_free(r);
                        node->type = NODE_BOOL;
                        node->as.boolean.value = false;
                    }
                } else if (node->as.logical.op == TOKEN_OR) {
                    /* true or x -> true, false or x -> x */
                    if (l->as.boolean.value) {
                        ast_free(l);
                        ast_free(r);
                        node->type = NODE_BOOL;
                        node->as.boolean.value = true;
                    } else {
                        ast_free(l);
                        *node = *r;
                        free(r);
                    }
                }
            }
            break;
        }
        case NODE_BLOCK:
            for (int i = 0; i < node->as.block.stmts.count; i++)
                node->as.block.stmts.nodes[i] = fold_node(node->as.block.stmts.nodes[i]);
            break;
        case NODE_EXPRESSION_STMT:
            node->as.expr_stmt.expr = fold_node(node->as.expr_stmt.expr);
            break;
        case NODE_LET:
        case NODE_CONST:
            node->as.var_decl.initializer = fold_node(node->as.var_decl.initializer);
            break;
        case NODE_IF:
            node->as.if_stmt.condition = fold_node(node->as.if_stmt.condition);
            node->as.if_stmt.then_branch = fold_node(node->as.if_stmt.then_branch);
            node->as.if_stmt.else_branch = fold_node(node->as.if_stmt.else_branch);
            break;
        case NODE_LOOP:
            node->as.loop_stmt.body = fold_node(node->as.loop_stmt.body);
            break;
        case NODE_FOR_RANGE:
            node->as.for_range.start = fold_node(node->as.for_range.start);
            node->as.for_range.end = fold_node(node->as.for_range.end);
            node->as.for_range.body = fold_node(node->as.for_range.body);
            break;
        case NODE_FOR_IN:
            node->as.for_in.iterable = fold_node(node->as.for_in.iterable);
            node->as.for_in.body = fold_node(node->as.for_in.body);
            break;
        case NODE_RETURN:
            for (int i = 0; i < node->as.return_stmt.values.count; i++)
                node->as.return_stmt.values.nodes[i] = fold_node(node->as.return_stmt.values.nodes[i]);
            break;
        case NODE_CALL:
            node->as.call.callee = fold_node(node->as.call.callee);
            for (int i = 0; i < node->as.call.args.count; i++)
                node->as.call.args.nodes[i] = fold_node(node->as.call.args.nodes[i]);
            break;
        case NODE_TRY:
            node->as.try_expr.expr = fold_node(node->as.try_expr.expr);
            break;
        case NODE_TRY_BLOCK:
            node->as.try_block.body = fold_node(node->as.try_block.body);
            break;
        case NODE_TRY_CATCH:
            node->as.try_catch.body = fold_node(node->as.try_catch.body);
            node->as.try_catch.catch_body = fold_node(node->as.try_catch.catch_body);
            break;
        case NODE_ASSIGN:
            node->as.assign.value = fold_node(node->as.assign.value);
            break;
        case NODE_FN_DECL:
            node->as.fn_decl.body = fold_node(node->as.fn_decl.body);
            break;
        case NODE_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.items.count; i++)
                node->as.array_literal.items.nodes[i] = fold_node(node->as.array_literal.items.nodes[i]);
            break;
        default:
            break;
    }
    return node;
}

void optimize_ast(ASTNode *node) {
    fold_node(node);
}

/* ========================================================================
 * Debug Disassembler
 * ======================================================================== */
void disassemble_chunk(Chunk *chunk, const char *name) {
    printf("== %s ==\n", name);
    for (int offset = 0; offset < chunk->count; offset++) {
        disassemble_instruction(chunk, offset);
    }
}

static const char *opcode_names[] = {
    "LOADK", "LOADBOOL", "LOADNIL", "MOVE",
    "GETGLOBAL", "SETGLOBAL", "GETUPVAL", "SETUPVAL",
    "ADD", "SUB", "MUL", "DIV", "MOD", "NEG",
    "ADDK", "SUBK", "MULK", "DIVK", "MODK",
    "ADDI", "SUBI", "MULI", "DIVI", "MODI",
    "EQ", "NEQ", "LT", "LE",
    "EQI", "NEQI", "LTI", "LEI", "GTI", "GEI",
    "EQI_TEST", "NEQI_TEST", "LTI_TEST", "LEI_TEST", "GTI_TEST", "GEI_TEST",
    "MODI_EQI_TEST", "MODI_NEQI_TEST",
    "NOT", "TEST", "TESTSET", "TESTJMP", "TESTERRJMP", "CONCAT", "TOSTRING", "LEN",
    "JMP", "LOOP",
    "FORPREP", "FORPREP_NUM", "FORLOOP", "FORLOOP_INC", "FORLOOP_NUM", "FORLOOP_INC_NUM",
    "FORADDLOCAL_FIELD_PROP", "FORADDLOCAL_FIELD_PROP_INC",
    "FORADDGLOBAL_FIELD_PROP", "FORADDGLOBAL_FIELD_PROP_INC",
    "FOR_MODI_ACCUM", "FOR_MODI_ACCUM_INC",
    "FOR_FIELD2_ACCUM", "FOR_FIELD2_ACCUM_INC",
    "ARRAY_MARK_FALSE_STRIDE",
    "CLOSURE", "CALL", "CALLG", "MCALL", "CALLR", "CALLSELF", "ADDUP",
    "ADDLOCAL", "SUBLOCAL",
    "ADDLOCAL_FIELD_PROP", "SUBLOCAL_FIELD_PROP",
    "ADDLOCAL_LEN", "SUBLOCAL_LEN",
    "ADDLOCAL_MULI", "SUBLOCAL_MULI", "ADDLOCAL_MULK", "SUBLOCAL_MULK",
    "ADDLOCAL_DIVI", "SUBLOCAL_DIVI", "ADDLOCAL_MODI", "SUBLOCAL_MODI",
    "RETURN",
    "NEWARRAY", "SETARRAY", "ARRAY_PUSH", "GETINDEX", "GETINDEX_TRY", "SETINDEX", "NEWDICT",
    "GETFIELD", "GETFIELD_TRY", "GETFIELD_PROP", "SETFIELD",
    "GETFIELD_IDX", "SETFIELD_IDX",
    "ADDSUB", "SUBADD", "MULADD", "MULSUB",
    "ADD_GT_TEST", "ADD_GE_TEST", "SUB_GT_TEST", "SUB_GE_TEST",
    "MULLOCAL_ADD", "MULLOCAL_SUB",
    "ITER_PREP", "ITER_NEXT",
    "AUX", "CLOSE_UPVAL", "DEFER", "NEWSTRUCT",
    "MCALLFIELD", "MCALLFIELD0", "ADDI_LOOP"
};

void disassemble_instruction(Chunk *chunk, int offset) {
    Instruction inst = chunk->code[offset];
    int op = GET_OPCODE(inst);
    int line = chunk->lines[offset];

    printf("%04d [L%d] ", offset, line);

    if (op < OP_COUNT) {
        printf("%-12s ", opcode_names[op]);
    } else {
        printf("UNKNOWN(%d)  ", op);
    }

    printf("A=%d B=%d C=%d Bx=%d sBx=%d\n",
           GET_A(inst), GET_B(inst), GET_C(inst),
           GET_Bx(inst), GET_sBx(inst));
}
