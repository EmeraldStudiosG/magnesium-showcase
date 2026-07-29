#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magnesium.h"

/* .mgc magic number: 'M' 'G' v1.15 */
#define MGC_MAGIC 0x4D47010F

#define BYTECODE_MAX_FUNCTION_DEPTH 64u
#define BYTECODE_MAX_FUNCTIONS 65536u
#define BYTECODE_MAX_FUNCTION_INSTRUCTIONS (1 << 20)
#define BYTECODE_MAX_FUNCTION_CONSTANTS MAX_CONSTANTS
#define BYTECODE_MAX_TOTAL_INSTRUCTIONS ((uint64_t)1 << 22)
#define BYTECODE_MAX_TOTAL_CONSTANTS ((uint64_t)1 << 22)
#define BYTECODE_MAX_TOTAL_STRING_BYTES ((uint64_t)64 << 20)
#define BYTECODE_MAX_STRING_LENGTH (1 << 20)
#define BYTECODE_MAX_ROPE_DEPTH 256u
#define BYTECODE_MAX_ROPE_NODES 65536u

typedef struct {
    unsigned depth;
    uint64_t function_count;
    uint64_t instruction_count;
    uint64_t constant_count;
    uint64_t string_bytes;
} BytecodeReadContext;

typedef struct {
    unsigned depth;
    ObjFunction *active[BYTECODE_MAX_FUNCTION_DEPTH];
    uint64_t function_count;
    uint64_t instruction_count;
    uint64_t constant_count;
    uint64_t string_bytes;
} BytecodeWriteValidationContext;

static bool validate_function(ObjFunction *function);
static bool validate_serializable_function(
    BytecodeWriteValidationContext *context, ObjFunction *function);

static bool add_to_budget(uint64_t *total, uint64_t amount,
                          uint64_t maximum) {
    if (amount > maximum - *total) return false;
    *total += amount;
    return true;
}

static bool write_items(FILE *f, const void *data, size_t size, size_t count) {
    return count == 0 || fwrite(data, size, count, f) == count;
}

static bool write_string(FILE *f, ObjString *string) {
    if (string == NULL) {
        int32_t length = -1;
        return write_items(f, &length, sizeof(int32_t), 1);
    }
    int32_t length = string->length;
    if (!write_items(f, &length, sizeof(int32_t), 1)) return false;
    if (string->is_rope) {
        for (int i = 0; i < string->length; i++) {
            char ch = string_char_at(string, i);
            if (!write_items(f, &ch, sizeof(char), 1)) return false;
        }
    } else {
        if (!write_items(f, string->chars, sizeof(char), (size_t)length)) return false;
    }
    return true;
}

static bool read_string(VM *vm, FILE *f, bool allow_null,
                        BytecodeReadContext *context, ObjString **out) {
    int32_t length;
    if (fread(&length, sizeof(int32_t), 1, f) != 1) return false;
    if (length == -1) {
        if (!allow_null) return false;
        *out = NULL;
        return true;
    }
    if (length < 0 || length > BYTECODE_MAX_STRING_LENGTH ||
        !add_to_budget(&context->string_bytes, (uint64_t)length,
                       BYTECODE_MAX_TOTAL_STRING_BYTES)) {
        return false;
    }

    char *chars = malloc((size_t)length + 1);
    if (!chars) return false;
    if (fread(chars, sizeof(char), (size_t)length, f) != (size_t)length) {
        free(chars);
        return false;
    }
    chars[length] = '\0';
    *out = take_string(vm, chars, length);
    return true;
}

static bool write_value(VM *vm, FILE *f, Value value);
static bool read_value(VM *vm, FILE *f, BytecodeReadContext *context,
                       Value *out);

static bool write_function(VM *vm, FILE *f, ObjFunction *function) {
    if (!write_items(f, &function->arity, sizeof(int), 1) ||
        !write_items(f, &function->upvalue_count, sizeof(int), 1) ||
        !write_items(f, &function->reg_count, sizeof(int), 1) ||
        !write_string(f, function->name) ||
        !write_string(f, function->source_name)) {
        return false;
    }

    Chunk *chunk = &function->chunk;
    if (!write_items(f, &chunk->count, sizeof(int), 1) ||
        !write_items(f, chunk->code, sizeof(Instruction), (size_t)chunk->count) ||
        !write_items(f, chunk->lines, sizeof(int), (size_t)chunk->count) ||
        !write_items(f, &chunk->const_count, sizeof(int), 1)) {
        return false;
    }

    for (int i = 0; i < chunk->const_count; i++) {
        if (!write_value(vm, f, chunk->constants[i])) return false;
    }
    return true;
}

static ObjFunction *read_function(VM *vm, FILE *f,
                                  BytecodeReadContext *context) {
    if (context->depth >= BYTECODE_MAX_FUNCTION_DEPTH ||
        context->function_count >= BYTECODE_MAX_FUNCTIONS) {
        return NULL;
    }
    context->depth++;
    context->function_count++;

    ObjFunction *function = new_function(vm);
    vm_push(vm, OBJ_VAL(function));
    bool ok = false;

    if (fread(&function->arity, sizeof(int), 1, f) != 1) goto done;
    if (fread(&function->upvalue_count, sizeof(int), 1, f) != 1) goto done;
    if (fread(&function->reg_count, sizeof(int), 1, f) != 1) goto done;
    if (function->arity < 0 || function->arity > 256 ||
        function->upvalue_count < 0 || function->upvalue_count > 256 ||
        function->reg_count < 0 || function->reg_count > 256) {
        goto done;
    }
    if (!read_string(vm, f, true, context, &function->name)) goto done;
    if (function->name) {
        gc_write_barrier(vm, (Obj *)function, OBJ_VAL(function->name));
    }
    if (!read_string(vm, f, true, context, &function->source_name)) goto done;
    if (function->source_name) {
        gc_write_barrier(vm, (Obj *)function, OBJ_VAL(function->source_name));
    }

    Chunk *chunk = &function->chunk;
    if (fread(&chunk->count, sizeof(int), 1, f) != 1) goto done;
    if (chunk->count < 0 ||
        chunk->count > BYTECODE_MAX_FUNCTION_INSTRUCTIONS ||
        !add_to_budget(&context->instruction_count,
                       (uint64_t)chunk->count,
                       BYTECODE_MAX_TOTAL_INSTRUCTIONS)) {
        goto done;
    }
    chunk->capacity = chunk->count;
    if (chunk->count > 0) {
        chunk->code = malloc(sizeof(Instruction) * (size_t)chunk->count);
        if (!chunk->code) goto done;
        if (fread(chunk->code, sizeof(Instruction), (size_t)chunk->count, f) !=
            (size_t)chunk->count) {
            goto done;
        }
    }

    if (chunk->count > 0) {
        chunk->lines = malloc(sizeof(int) * (size_t)chunk->count);
        if (!chunk->lines) goto done;
        if (fread(chunk->lines, sizeof(int), (size_t)chunk->count, f) !=
            (size_t)chunk->count) {
            goto done;
        }
    }

    if (fread(&chunk->const_count, sizeof(int), 1, f) != 1) goto done;
    if (chunk->const_count < 0 ||
        chunk->const_count > BYTECODE_MAX_FUNCTION_CONSTANTS ||
        !add_to_budget(&context->constant_count,
                       (uint64_t)chunk->const_count,
                       BYTECODE_MAX_TOTAL_CONSTANTS)) {
        goto done;
    }
    chunk->const_capacity = chunk->const_count;
    if (chunk->const_count > 0) {
        chunk->constants = malloc(sizeof(Value) * (size_t)chunk->const_count);
        if (!chunk->constants) goto done;
    }
    for (int i = 0; i < chunk->const_count; i++) {
        if (!read_value(vm, f, context, &chunk->constants[i])) goto done;
        gc_write_barrier(vm, (Obj *)function, chunk->constants[i]);
    }

    if (!validate_function(function)) goto done;
    ok = true;

done:
    vm_pop(vm);
    context->depth--;
    return ok ? function : NULL;
}

static bool write_value(VM *vm, FILE *f, Value value) {
    if (IS_NUMERIC(value)) {
        uint8_t type = 0;
        return write_items(f, &type, sizeof(uint8_t), 1) &&
               write_items(f, &value, sizeof(Value), 1);
    } else if (IS_NULL(value)) {
        uint8_t type = 1;
        return write_items(f, &type, sizeof(uint8_t), 1);
    } else if (IS_BOOL(value)) {
        uint8_t type = 2;
        uint8_t b = AS_BOOL(value) ? 1 : 0;
        return write_items(f, &type, sizeof(uint8_t), 1) &&
               write_items(f, &b, sizeof(uint8_t), 1);
    } else if (IS_OBJ(value)) {
        switch (AS_OBJ(value)->type) {
            case OBJ_STRING: {
                uint8_t type = 3;
                return write_items(f, &type, sizeof(uint8_t), 1) &&
                       write_string(f, AS_STRING(value));
            }
            case OBJ_FUNCTION: {
                uint8_t type = 4;
                return write_items(f, &type, sizeof(uint8_t), 1) &&
                       write_function(vm, f, AS_FUNCTION(value));
            }
            default:
                fprintf(stderr, "Cannot serialize object type %d\n", AS_OBJ(value)->type);
                return false;
        }
    }
    return false;
}

static bool read_value(VM *vm, FILE *f, BytecodeReadContext *context,
                       Value *out) {
    uint8_t type;
    if (fread(&type, sizeof(uint8_t), 1, f) != 1) return false;
    switch (type) {
        case 0: { /* Number */
            Value val;
            if (fread(&val, sizeof(Value), 1, f) != 1) return false;
            if (!IS_NUMERIC(val)) return false;
            *out = val;
            return true;
        }
        case 1:
            *out = NULL_VAL;
            return true;
        case 2: { /* Bool */
            uint8_t b;
            if (fread(&b, sizeof(uint8_t), 1, f) != 1 || b > 1) return false;
            *out = BOOL_VAL(b);
            return true;
        }
        case 3: {
            ObjString *string;
            if (!read_string(vm, f, false, context, &string)) return false;
            *out = OBJ_VAL(string);
            return true;
        }
        case 4: {
            ObjFunction *function = read_function(vm, f, context);
            if (!function) return false;
            *out = OBJ_VAL(function);
            return true;
        }
        default:
            return false;
    }
}

static bool validate_rope_shape(ObjString *string, ObjString **active,
                                unsigned depth, uint64_t *node_count) {
    if (!string || string->obj.type != OBJ_STRING ||
        string->length < 0 ||
        string->length > BYTECODE_MAX_STRING_LENGTH ||
        *node_count >= BYTECODE_MAX_ROPE_NODES) {
        return false;
    }
    (*node_count)++;

    if (!string->is_rope) {
        return string->chars != NULL &&
               string->capacity >= string->length &&
               string->left == NULL && string->right == NULL;
    }

    if (depth >= BYTECODE_MAX_ROPE_DEPTH || string->chars != NULL ||
        !string->left || !string->right ||
        string->left->obj.type != OBJ_STRING ||
        string->right->obj.type != OBJ_STRING ||
        string->left->length < 0 || string->right->length < 0 ||
        (uint64_t)string->left->length +
                (uint64_t)string->right->length !=
            (uint64_t)string->length) {
        return false;
    }
    for (unsigned i = 0; i < depth; i++) {
        if (active[i] == string) return false;
    }

    active[depth] = string;
    return validate_rope_shape(string->left, active, depth + 1,
                               node_count) &&
           validate_rope_shape(string->right, active, depth + 1,
                               node_count);
}

static bool validate_serializable_string(
    BytecodeWriteValidationContext *context, ObjString *string) {
    ObjString *active[BYTECODE_MAX_ROPE_DEPTH];
    uint64_t node_count = 0;
    if (!validate_rope_shape(string, active, 0, &node_count)) return false;
    return add_to_budget(&context->string_bytes, (uint64_t)string->length,
                         BYTECODE_MAX_TOTAL_STRING_BYTES);
}

static bool validate_serializable_value(
    BytecodeWriteValidationContext *context, Value value) {
    if (IS_NUMERIC(value) || IS_NULL(value) || IS_BOOL(value)) return true;
    if (!IS_OBJ(value)) return false;

    Obj *object = AS_OBJ(value);
    if (!object) return false;
    if (object->type == OBJ_STRING) {
        return validate_serializable_string(context, (ObjString *)object);
    }
    return object->type == OBJ_FUNCTION;
}

static bool validate_serializable_function(
    BytecodeWriteValidationContext *context, ObjFunction *function) {
    if (!function || function->obj.type != OBJ_FUNCTION ||
        context->depth >= BYTECODE_MAX_FUNCTION_DEPTH ||
        context->function_count >= BYTECODE_MAX_FUNCTIONS) {
        return false;
    }
    for (unsigned i = 0; i < context->depth; i++) {
        if (context->active[i] == function) return false;
    }

    if (function->arity < 0 || function->arity > MAX_REGISTERS ||
        function->upvalue_count < 0 ||
        function->upvalue_count > MAX_UPVALUES ||
        function->reg_count <= 0 ||
        function->reg_count > MAX_REGISTERS) {
        return false;
    }

    Chunk *chunk = &function->chunk;
    if (chunk->count <= 0 ||
        chunk->count > BYTECODE_MAX_FUNCTION_INSTRUCTIONS ||
        chunk->capacity < chunk->count ||
        !chunk->code || !chunk->lines ||
        chunk->const_count < 0 ||
        chunk->const_count > BYTECODE_MAX_FUNCTION_CONSTANTS ||
        chunk->const_capacity < chunk->const_count ||
        (chunk->const_count > 0 && !chunk->constants)) {
        return false;
    }

    context->active[context->depth++] = function;
    context->function_count++;
    bool ok = false;

    if (!add_to_budget(&context->instruction_count,
                       (uint64_t)chunk->count,
                       BYTECODE_MAX_TOTAL_INSTRUCTIONS) ||
        !add_to_budget(&context->constant_count,
                       (uint64_t)chunk->const_count,
                       BYTECODE_MAX_TOTAL_CONSTANTS)) {
        goto done;
    }
    if ((function->name &&
         !validate_serializable_string(context, function->name)) ||
        (function->source_name &&
         !validate_serializable_string(context, function->source_name))) {
        goto done;
    }

    for (int i = 0; i < chunk->const_count; i++) {
        if (!validate_serializable_value(context, chunk->constants[i])) {
            goto done;
        }
    }
    if (!validate_function(function)) goto done;

    for (int i = 0; i < chunk->const_count; i++) {
        Value value = chunk->constants[i];
        if (IS_OBJ(value) && AS_OBJ(value)->type == OBJ_FUNCTION &&
            !validate_serializable_function(context,
                                            AS_FUNCTION(value))) {
            goto done;
        }
    }
    ok = true;

done:
    context->depth--;
    return ok;
}

static bool valid_reg(ObjFunction *function, int reg) {
    return reg >= 0 && reg < function->reg_count;
}

static bool valid_reg_span(ObjFunction *function, int first, int count) {
    return count >= 0 && first >= 0 && first + count <= function->reg_count;
}

static bool valid_const(ObjFunction *function, int index) {
    return index >= 0 && index < function->chunk.const_count;
}

static bool const_is_string(ObjFunction *function, int index) {
    return valid_const(function, index) && IS_STRING(function->chunk.constants[index]);
}

static bool const_is_function(ObjFunction *function, int index) {
    return valid_const(function, index) && IS_FUNCTION(function->chunk.constants[index]);
}

static bool const_is_numeric(ObjFunction *function, int index) {
    return valid_const(function, index) && IS_NUMERIC(function->chunk.constants[index]);
}

static bool valid_abs_target(ObjFunction *function, int target) {
    return target >= 0 && target < function->chunk.count;
}

static bool valid_sbx_target(ObjFunction *function, int offset, Instruction inst) {
    return valid_abs_target(function, offset + 1 + GET_sBx(inst));
}

static bool valid_loop_target(ObjFunction *function, int offset, Instruction inst) {
    return valid_abs_target(function, offset + 1 - GET_sBx(inst));
}

static bool valid_asbx_target(ObjFunction *function, int offset, Instruction inst) {
    return valid_abs_target(function, offset + 1 + GET_AsBx_sBx(inst));
}

static bool validate_inline_jump(ObjFunction *function, int offset) {
    if (offset + 1 >= function->chunk.count) return false;
    Instruction jump = function->chunk.code[offset + 1];
    if (GET_OPCODE(jump) != OP_JMP) return false;
    return valid_abs_target(function, offset + 2 + GET_sBx(jump));
}

static bool valid_instruction_target(const uint8_t *starts, int count,
                                     long long target) {
    return target >= 0 && target < count && starts[(int)target] != 0;
}

/*
 * Validate instruction boundaries separately from operands. Several
 * instructions consume inline data words (AUX records, closure captures,
 * struct field metadata, or an inline JMP). Those words are not legal jump
 * targets even when their numeric offsets are inside the chunk.
 */
static bool validate_control_flow(ObjFunction *function) {
    Chunk *chunk = &function->chunk;
    int count = chunk->count;
    uint8_t *starts = calloc((size_t)count, sizeof(uint8_t));
    int *spans = calloc((size_t)count, sizeof(int));
    if (!starts || !spans) {
        free(starts);
        free(spans);
        return false;
    }

    bool ok = false;
    for (int offset = 0; offset < count;) {
        Instruction inst = chunk->code[offset];
        int op = GET_OPCODE(inst);
        int span = 1;
        if (op < 0 || op >= OP_COUNT || op == OP_AUX) goto done;

        switch ((OpCode)op) {
            case OP_EQI_TEST:
            case OP_NEQI_TEST:
            case OP_LTI_TEST:
            case OP_LEI_TEST:
            case OP_GTI_TEST:
            case OP_GEI_TEST:
            case OP_MODI_EQI_TEST:
            case OP_MODI_NEQI_TEST:
            case OP_TEST:
            case OP_TESTSET:
            case OP_ADD_GT_TEST:
            case OP_ADD_GE_TEST:
            case OP_SUB_GT_TEST:
            case OP_SUB_GE_TEST:
                span = 2;
                if (offset + span > count ||
                    GET_OPCODE(chunk->code[offset + 1]) != OP_JMP) {
                    goto done;
                }
                break;

            case OP_FORADDLOCAL_FIELD_PROP:
            case OP_FORADDLOCAL_FIELD_PROP_INC:
            case OP_ARRAY_MARK_FALSE_STRIDE:
            case OP_ADDSUB:
            case OP_SUBADD:
            case OP_MULADD:
            case OP_MULSUB:
                span = 2;
                if (offset + span > count ||
                    GET_OPCODE(chunk->code[offset + 1]) != OP_AUX) {
                    goto done;
                }
                break;

            case OP_FORADDGLOBAL_FIELD_PROP:
            case OP_FORADDGLOBAL_FIELD_PROP_INC:
            case OP_FOR_MODI_ACCUM:
            case OP_FOR_MODI_ACCUM_INC:
            case OP_FOR_FIELD2_ACCUM:
            case OP_FOR_FIELD2_ACCUM_INC:
                span = 3;
                if (offset + span > count ||
                    GET_OPCODE(chunk->code[offset + 1]) != OP_AUX ||
                    GET_OPCODE(chunk->code[offset + 2]) != OP_AUX) {
                    goto done;
                }
                break;

            case OP_CLOSURE: {
                if (!const_is_function(function, GET_Bx(inst))) goto done;
                ObjFunction *inner =
                    AS_FUNCTION(chunk->constants[GET_Bx(inst)]);
                if (!inner || inner->upvalue_count < 0 ||
                    inner->upvalue_count > MAX_UPVALUES) {
                    goto done;
                }
                span = 1 + inner->upvalue_count;
                break;
            }

            case OP_NEWSTRUCT: {
                if (offset + 1 >= count) goto done;
                int field_count = GET_OPCODE(chunk->code[offset + 1]);
                if (field_count < 0 || field_count > MAX_REGISTERS) goto done;
                span = 2 + field_count;
                break;
            }

            case OP_COUNT:
            case OP_AUX:
                goto done;

            default:
                break;
        }

        if (span <= 0 || span > count - offset) goto done;
        starts[offset] = 1;
        spans[offset] = span;
        offset += span;
    }

    for (int offset = 0; offset < count; offset++) {
        if (!starts[offset]) continue;

        Instruction inst = chunk->code[offset];
        OpCode op = (OpCode)GET_OPCODE(inst);
        long long next = (long long)offset + spans[offset];
        long long target;

        switch (op) {
            case OP_RETURN:
                break;

            case OP_JMP:
                target = (long long)offset + 1 + GET_sBx(inst);
                if (!valid_instruction_target(starts, count, target)) goto done;
                break;

            case OP_LOOP:
                if (GET_sBx(inst) <= 0) goto done;
                target = (long long)offset + 1 - GET_sBx(inst);
                if (!valid_instruction_target(starts, count, target)) goto done;
                break;

            case OP_ADDI_LOOP:
                if (GET_C(inst) == 0) goto done;
                target = (long long)offset + 1 - GET_C(inst);
                if (!valid_instruction_target(starts, count, target)) goto done;
                break;

            case OP_LOADBOOL:
                target = (long long)offset + 1 + (GET_C(inst) != 0);
                if (!valid_instruction_target(starts, count, target)) goto done;
                break;

            case OP_TESTJMP:
            case OP_TESTERRJMP:
            case OP_FORLOOP:
            case OP_FORLOOP_INC:
            case OP_FORLOOP_NUM:
            case OP_FORLOOP_INC_NUM:
                if (!valid_instruction_target(starts, count,
                                              (long long)offset + 1)) {
                    goto done;
                }
                target =
                    (long long)offset + 1 + GET_AsBx_sBx(inst);
                if (!valid_instruction_target(starts, count, target)) goto done;
                break;

            case OP_ITER_NEXT:
                if (!valid_instruction_target(starts, count,
                                              (long long)offset + 1)) {
                    goto done;
                }
                target = (long long)offset + 1 + GET_C(inst);
                if (!valid_instruction_target(starts, count, target)) goto done;
                break;

            case OP_EQI_TEST:
            case OP_NEQI_TEST:
            case OP_LTI_TEST:
            case OP_LEI_TEST:
            case OP_GTI_TEST:
            case OP_GEI_TEST:
            case OP_MODI_EQI_TEST:
            case OP_MODI_NEQI_TEST:
            case OP_TEST:
            case OP_TESTSET:
            case OP_ADD_GT_TEST:
            case OP_ADD_GE_TEST:
            case OP_SUB_GT_TEST:
            case OP_SUB_GE_TEST:
                if (!valid_instruction_target(starts, count, next)) goto done;
                target = next + GET_sBx(chunk->code[offset + 1]);
                if (!valid_instruction_target(starts, count, target)) goto done;
                break;

            default:
                if (!valid_instruction_target(starts, count, next)) goto done;
                break;
        }
    }

    ok = true;

done:
    free(starts);
    free(spans);
    return ok;
}

static bool validate_function(ObjFunction *function) {
    if (!function ||
        function->arity < 0 || function->arity > MAX_REGISTERS ||
        function->upvalue_count < 0 ||
        function->upvalue_count > MAX_UPVALUES ||
        function->reg_count <= 0 || function->reg_count > MAX_REGISTERS) {
        return false;
    }

    Chunk *chunk = &function->chunk;
    if (chunk->count <= 0 || chunk->count > (1 << 20) ||
        !chunk->code || !chunk->lines ||
        chunk->const_count < 0 || chunk->const_count > MAX_CONSTANTS ||
        (chunk->const_count > 0 && !chunk->constants)) {
        return false;
    }

    for (int offset = 0; offset < chunk->count; offset++) {
        Instruction inst = chunk->code[offset];
        int op = GET_OPCODE(inst);
        if (op < 0 || op >= OP_COUNT) return false;

        switch ((OpCode)op) {
            case OP_LOADK:
                if (!valid_reg(function, GET_A(inst)) || !valid_const(function, GET_Bx(inst))) return false;
                break;
            case OP_LOADBOOL:
                if (!valid_reg(function, GET_A(inst))) return false;
                if (GET_C(inst) && offset + 1 >= chunk->count) return false;
                break;
            case OP_LOADNIL:
            case OP_CLOSE_UPVAL:
            case OP_DEFER:
                if (!valid_reg(function, GET_A(inst))) return false;
                break;
            case OP_MOVE:
            case OP_NEG:
            case OP_NOT:
            case OP_FORPREP:
            case OP_FORPREP_NUM:
            case OP_ARRAY_PUSH:
            case OP_ITER_PREP:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst))) return false;
                if ((op == OP_FORPREP || op == OP_FORPREP_NUM) && !valid_reg(function, GET_A(inst) + 1)) return false;
                break;
            case OP_GETUPVAL:
            case OP_SETUPVAL:
                if (!valid_reg(function, GET_A(inst)) ||
                    (int)GET_B(inst) >= function->upvalue_count) return false;
                break;
            case OP_GETGLOBAL:
            case OP_SETGLOBAL:
                if (!valid_reg(function, GET_A(inst)) || !const_is_string(function, GET_Bx(inst))) return false;
                break;
            case OP_ADD:
            case OP_SUB:
            case OP_MUL:
            case OP_DIV:
            case OP_MOD:
            case OP_EQ:
            case OP_NEQ:
            case OP_LT:
            case OP_LE:
            case OP_SETINDEX:
            case OP_GETINDEX:
            case OP_GETINDEX_TRY:
            case OP_MULLOCAL_ADD:
            case OP_MULLOCAL_SUB:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !valid_reg(function, GET_C(inst))) return false;
                if (op == OP_GETINDEX_TRY && !valid_reg(function, GET_A(inst) + 1)) return false;
                break;
            case OP_ADDK:
            case OP_SUBK:
            case OP_MULK:
            case OP_DIVK:
            case OP_MODK:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !const_is_numeric(function, GET_C(inst))) return false;
                break;
            case OP_ADDI:
            case OP_SUBI:
            case OP_MULI:
            case OP_DIVI:
            case OP_MODI:
            case OP_EQI:
            case OP_NEQI:
            case OP_LTI:
            case OP_LEI:
            case OP_GTI:
            case OP_GEI:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst))) return false;
                break;
            case OP_EQI_TEST:
            case OP_NEQI_TEST:
            case OP_LTI_TEST:
            case OP_LEI_TEST:
            case OP_GTI_TEST:
            case OP_GEI_TEST:
            case OP_MODI_EQI_TEST:
            case OP_MODI_NEQI_TEST:
                if (!valid_reg(function, GET_A(inst)) || !validate_inline_jump(function, offset)) return false;
                break;
            case OP_TEST:
                if (!valid_reg(function, GET_A(inst)) || !validate_inline_jump(function, offset)) return false;
                break;
            case OP_TESTSET:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !validate_inline_jump(function, offset)) return false;
                break;
            case OP_TESTJMP:
            case OP_TESTERRJMP:
                if (!valid_reg(function, GET_AsBx_A(inst)) || !valid_asbx_target(function, offset, inst)) return false;
                break;
            case OP_CONCAT:
                if (GET_B(inst) > GET_C(inst) || !valid_reg_span(function, GET_B(inst), GET_C(inst) - GET_B(inst) + 1) ||
                    !valid_reg(function, GET_A(inst))) return false;
                break;
            case OP_TOSTRING:
            case OP_LEN:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst))) return false;
                break;
            case OP_JMP:
                if (!valid_sbx_target(function, offset, inst)) return false;
                break;
            case OP_LOOP:
                if (!valid_loop_target(function, offset, inst)) return false;
                break;
            case OP_FORLOOP:
            case OP_FORLOOP_INC:
            case OP_FORLOOP_NUM:
            case OP_FORLOOP_INC_NUM:
                if (!valid_reg(function, GET_AsBx_A(inst)) ||
                    !valid_reg(function, GET_AsBx_A(inst) + 1) ||
                    !valid_asbx_target(function, offset, inst)) return false;
                break;
            case OP_FORADDLOCAL_FIELD_PROP:
            case OP_FORADDLOCAL_FIELD_PROP_INC: {
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_A(inst) + 1) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !valid_reg(function, GET_C(inst)) ||
                    offset + 1 >= chunk->count) return false;
                Instruction aux = chunk->code[offset + 1];
                if (GET_OPCODE(aux) != OP_AUX || !const_is_string(function, GET_Bx(aux))) return false;
                offset++;
                break;
            }
            case OP_FORADDGLOBAL_FIELD_PROP:
            case OP_FORADDGLOBAL_FIELD_PROP_INC: {
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_A(inst) + 1) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !valid_reg(function, GET_B(inst) + 1) ||
                    !valid_reg(function, GET_C(inst)) ||
                    offset + 2 >= chunk->count) return false;
                Instruction target_aux = chunk->code[offset + 1];
                Instruction field_aux = chunk->code[offset + 2];
                if (GET_OPCODE(target_aux) != OP_AUX || !const_is_string(function, GET_Bx(target_aux)) ||
                    GET_OPCODE(field_aux) != OP_AUX || !const_is_string(function, GET_Bx(field_aux))) return false;
                offset += 2;
                break;
            }
            case OP_FOR_MODI_ACCUM:
            case OP_FOR_MODI_ACCUM_INC: {
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_A(inst) + 1) ||
                    !valid_reg(function, GET_B(inst)) ||
                    GET_sC(inst) == 0 ||
                    offset + 2 >= chunk->count) return false;
                Instruction aux0 = chunk->code[offset + 1];
                Instruction aux1 = chunk->code[offset + 2];
                if (GET_OPCODE(aux0) != OP_AUX || GET_OPCODE(aux1) != OP_AUX) return false;
                offset += 2;
                break;
            }
            case OP_FOR_FIELD2_ACCUM:
            case OP_FOR_FIELD2_ACCUM_INC: {
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_A(inst) + 1) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !const_is_string(function, GET_C(inst)) ||
                    offset + 2 >= chunk->count) return false;
                Instruction aux0 = chunk->code[offset + 1];
                Instruction aux1 = chunk->code[offset + 2];
                if (GET_OPCODE(aux0) != OP_AUX ||
                    GET_OPCODE(aux1) != OP_AUX ||
                    !const_is_string(function, GET_A(aux0)) ||
                    !const_is_string(function, GET_C(aux0))) return false;
                offset += 2;
                break;
            }
            case OP_ARRAY_MARK_FALSE_STRIDE: {
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !valid_reg(function, GET_C(inst)) ||
                    offset + 1 >= chunk->count) return false;
                Instruction aux = chunk->code[offset + 1];
                if (GET_OPCODE(aux) != OP_AUX ||
                    !valid_reg(function, GET_Bx(aux))) return false;
                offset++;
                break;
            }
            case OP_ADDLOCAL_MULI:
            case OP_SUBLOCAL_MULI:
            case OP_ADDLOCAL_DIVI:
            case OP_SUBLOCAL_DIVI:
            case OP_ADDLOCAL_MODI:
            case OP_SUBLOCAL_MODI:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst))) return false;
                break;
            case OP_ADDLOCAL_MULK:
            case OP_SUBLOCAL_MULK:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !const_is_numeric(function, GET_C(inst))) return false;
                break;
            case OP_CLOSURE: {
                if (!valid_reg(function, GET_A(inst)) || !const_is_function(function, GET_Bx(inst))) return false;
                ObjFunction *inner = AS_FUNCTION(chunk->constants[GET_Bx(inst)]);
                if (offset + inner->upvalue_count >= chunk->count) return false;
                for (int i = 0; i < inner->upvalue_count; i++) {
                    Instruction up = chunk->code[offset + 1 + i];
                    int is_local = GET_OPCODE(up);
                    int index = GET_A(up);
                    if (is_local != 0 && is_local != 1) return false;
                    if (is_local) {
                        if (!valid_reg(function, index)) return false;
                    } else if (index >= function->upvalue_count) {
                        return false;
                    }
                }
                offset += inner->upvalue_count;
                break;
            }
            case OP_CALL:
            case OP_MCALL: {
                int base = GET_A(inst);
                int arg_count = GET_B(inst);
                int expected = GET_C(inst) - 1;
                if (!valid_reg_span(function, base, arg_count + 1) ||
                    !valid_reg_span(function, base, expected)) return false;
                break;
            }
            case OP_MCALLFIELD:
            case OP_MCALLFIELD0: {
                int base = GET_A(inst);
                int arg_count = GET_B(inst);
                if (!const_is_string(function, GET_C(inst)) ||
                    !valid_reg_span(function, base, arg_count + 1)) return false;
                break;
            }
            case OP_CALLR: {
                int base = GET_A(inst);
                int callee = GET_B(inst);
                int arg_count = GET_C(inst);
                if (!valid_reg(function, callee) ||
                    !valid_reg_span(function, base, arg_count + 1) ||
                    !valid_reg(function, base)) return false;
                break;
            }
            case OP_CALLSELF: {
                int base = GET_A(inst);
                int arg_count = GET_B(inst);
                int expected = GET_C(inst) - 1;
                if (!valid_reg_span(function, base, arg_count + 1) ||
                    !valid_reg_span(function, base, expected)) return false;
                break;
            }
            case OP_ADDUP:
                if ((int)GET_A(inst) >= function->upvalue_count ||
                    !valid_reg(function, GET_B(inst))) return false;
                break;
            case OP_ADDLOCAL:
            case OP_SUBLOCAL:
            case OP_ADDLOCAL_LEN:
            case OP_SUBLOCAL_LEN:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst))) return false;
                break;
            case OP_ADDLOCAL_FIELD_PROP:
            case OP_SUBLOCAL_FIELD_PROP:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_A(inst) + 1) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !const_is_string(function, GET_C(inst))) return false;
                break;
            case OP_ADDSUB:
            case OP_SUBADD:
            case OP_MULADD:
            case OP_MULSUB:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst)) ||
                    !valid_reg(function, GET_C(inst)) ||
                    offset + 1 >= chunk->count) return false;
                Instruction aux_word = chunk->code[offset + 1];
                if (GET_OPCODE(aux_word) != OP_AUX ||
                    !valid_reg(function, GET_Bx(aux_word))) return false;
                offset++;
                break;
            case OP_ADD_GT_TEST:
            case OP_ADD_GE_TEST:
            case OP_SUB_GT_TEST:
            case OP_SUB_GE_TEST:
                if (!valid_reg(function, GET_A(inst)) ||
                    !valid_reg(function, GET_B(inst)) ||
                    offset + 1 >= chunk->count) return false;
                Instruction jmp_word = chunk->code[offset + 1];
                if (GET_OPCODE(jmp_word) != OP_JMP) return false;
                offset++;
                break;
            case OP_CALLG: {
                int base = GET_A(inst);
                int arg_count = GET_B(inst);
                if (!valid_reg_span(function, base, arg_count + 1) || !const_is_string(function, GET_C(inst))) return false;
                break;
            }
            case OP_RETURN:
                if (!valid_reg_span(function, GET_A(inst), GET_B(inst))) return false;
                break;
            case OP_NEWARRAY:
            case OP_NEWDICT:
                if (!valid_reg(function, GET_A(inst))) return false;
                break;
            case OP_SETARRAY:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_C(inst))) return false;
                break;
            case OP_GETFIELD:
            case OP_GETFIELD_TRY:
            case OP_GETFIELD_PROP:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst)) || !const_is_string(function, GET_C(inst))) return false;
                if ((op == OP_GETFIELD_TRY || op == OP_GETFIELD_PROP) && !valid_reg(function, GET_A(inst) + 1)) return false;
                break;
            case OP_SETFIELD:
                if (!valid_reg(function, GET_A(inst)) || !const_is_string(function, GET_B(inst)) || !valid_reg(function, GET_C(inst))) return false;
                break;
            case OP_GETFIELD_IDX:
            case OP_SETFIELD_IDX:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst))) return false;
                break;
            case OP_AUX:
                return false;
            case OP_ITER_NEXT:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_A(inst) + 1) ||
                    GET_B(inst) == 0 || !valid_reg(function, GET_B(inst)) || !valid_reg(function, GET_B(inst) - 1) ||
                    !valid_abs_target(function, offset + 1 + GET_C(inst))) return false;
                break;
            case OP_NEWSTRUCT: {
                if (!valid_reg(function, GET_A(inst)) || !const_is_string(function, GET_Bx(inst))) return false;
                if (offset + 1 >= chunk->count) return false;
                int field_count = GET_OPCODE(chunk->code[offset + 1]);
                if (field_count < 0 || field_count > MAX_REGISTERS || offset + 1 + field_count >= chunk->count) return false;
                for (int i = 0; i < field_count; i++) {
                    if (!const_is_string(function, GET_OPCODE(chunk->code[offset + 2 + i]))) return false;
                }
                offset += 1 + field_count;
                break;
            }
            case OP_ADDI_LOOP: {
                if (!valid_reg(function, GET_A(inst)) || GET_C(inst) == 0) return false;
                break;
            }
            case OP_COUNT:
                return false;
        }
    }

    return validate_control_flow(function);
}

bool vm_save_bytecode(VM *vm, ObjFunction *function, const char *path) {
    if (!vm || !path) return false;
    BytecodeWriteValidationContext context = {0};
    if (!validate_serializable_function(&context, function)) return false;

    FILE *f = fopen(path, "wb");
    if (!f) return false;

    uint32_t magic = MGC_MAGIC;
    bool ok = write_items(f, &magic, sizeof(uint32_t), 1) &&
              write_function(vm, f, function) &&
              !ferror(f);
    if (fclose(f) != 0) ok = false;
    return ok;
}

ObjFunction *vm_load_bytecode(VM *vm, const char *path) {
    if (!vm || !path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint32_t magic;
    if (fread(&magic, sizeof(uint32_t), 1, f) != 1 || magic != MGC_MAGIC) {
        fclose(f);
        return NULL;
    }

    BytecodeReadContext context = {0};
    ObjFunction *function = read_function(vm, f, &context);
    if (!function) {
        fclose(f);
        return NULL;
    }
    if (fgetc(f) != EOF || ferror(f)) {
        fclose(f);
        return NULL;
    }
    fclose(f);
    return function;
}
