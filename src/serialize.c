#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magnesium.h"

/* .mgc magic number: 'M' 'G' v1.15 */
#define MGC_MAGIC 0x4D47010F

static bool validate_function(ObjFunction *function);

static void write_string(FILE *f, ObjString *string) {
    if (string == NULL) {
        int32_t length = -1;
        fwrite(&length, sizeof(int32_t), 1, f);
        return;
    }
    int32_t length = string->length;
    fwrite(&length, sizeof(int32_t), 1, f);
    if (string->is_rope) {
        for (int i = 0; i < string->length; i++) {
            char ch = string_char_at(string, i);
            fwrite(&ch, sizeof(char), 1, f);
        }
    } else {
        fwrite(string->chars, sizeof(char), length, f);
    }
}

static bool read_string(VM *vm, FILE *f, bool allow_null, ObjString **out) {
    int32_t length;
    if (fread(&length, sizeof(int32_t), 1, f) != 1) return false;
    if (length == -1) {
        if (!allow_null) return false;
        *out = NULL;
        return true;
    }
    if (length < 0 || length > (1 << 20)) return false;

    char *chars = malloc(length + 1);
    if (!chars) return false;
    if ((int32_t)fread(chars, sizeof(char), length, f) != length) {
        free(chars);
        return false;
    }
    chars[length] = '\0';
    *out = take_string(vm, chars, length);
    return true;
}

static void write_value(VM *vm, FILE *f, Value value);
static bool read_value(VM *vm, FILE *f, Value *out);

static void write_function(VM *vm, FILE *f, ObjFunction *function) {
    fwrite(&function->arity, sizeof(int), 1, f);
    fwrite(&function->upvalue_count, sizeof(int), 1, f);
    fwrite(&function->reg_count, sizeof(int), 1, f);
    write_string(f, function->name);
    write_string(f, function->source_name);

    Chunk *chunk = &function->chunk;
    fwrite(&chunk->count, sizeof(int), 1, f);
    fwrite(chunk->code, sizeof(Instruction), chunk->count, f);
    fwrite(chunk->lines, sizeof(int), chunk->count, f);

    fwrite(&chunk->const_count, sizeof(int), 1, f);
    for (int i = 0; i < chunk->const_count; i++) {
        write_value(vm, f, chunk->constants[i]);
    }
}

static ObjFunction *read_function(VM *vm, FILE *f) {
    ObjFunction *function = new_function(vm);
    vm_push(vm, OBJ_VAL(function));

    if (fread(&function->arity, sizeof(int), 1, f) != 1) { vm_pop(vm); return NULL; }
    if (fread(&function->upvalue_count, sizeof(int), 1, f) != 1) { vm_pop(vm); return NULL; }
    if (fread(&function->reg_count, sizeof(int), 1, f) != 1) { vm_pop(vm); return NULL; }
    if (function->arity < 0 || function->arity > 256 ||
        function->upvalue_count < 0 || function->upvalue_count > 256 ||
        function->reg_count < 0 || function->reg_count > 256) {
        vm_pop(vm);
        return NULL;
    }
    if (!read_string(vm, f, true, &function->name)) { vm_pop(vm); return NULL; }
    if (!read_string(vm, f, true, &function->source_name)) { vm_pop(vm); return NULL; }

    Chunk *chunk = &function->chunk;
    if (fread(&chunk->count, sizeof(int), 1, f) != 1) { vm_pop(vm); return NULL; }
    if (chunk->count < 0 || chunk->count > (1 << 20)) { vm_pop(vm); return NULL; }
    chunk->capacity = chunk->count;
    chunk->code = malloc(sizeof(Instruction) * chunk->count);
    if (!chunk->code) { vm_pop(vm); return NULL; }
    if ((int)fread(chunk->code, sizeof(Instruction), chunk->count, f) != chunk->count) {
        vm_pop(vm); return NULL;
    }

    chunk->lines = malloc(sizeof(int) * chunk->count);
    if (!chunk->lines) { vm_pop(vm); return NULL; }
    if ((int)fread(chunk->lines, sizeof(int), chunk->count, f) != chunk->count) {
        vm_pop(vm); return NULL;
    }

    if (fread(&chunk->const_count, sizeof(int), 1, f) != 1) { vm_pop(vm); return NULL; }
    if (chunk->const_count < 0 || chunk->const_count > (1 << 20)) { vm_pop(vm); return NULL; }
    chunk->const_capacity = chunk->const_count;
    chunk->constants = malloc(sizeof(Value) * chunk->const_count);
    if (!chunk->constants) { vm_pop(vm); return NULL; }
    for (int i = 0; i < chunk->const_count; i++) {
        if (!read_value(vm, f, &chunk->constants[i])) { vm_pop(vm); return NULL; }
    }

    if (!validate_function(function)) { vm_pop(vm); return NULL; }

    vm_pop(vm);
    return function;
}

static void write_value(VM *vm, FILE *f, Value value) {
    if (IS_NUMERIC(value)) {
        uint8_t type = 0;
        fwrite(&type, sizeof(uint8_t), 1, f);
        fwrite(&value, sizeof(Value), 1, f);
    } else if (IS_NULL(value)) {
        uint8_t type = 1;
        fwrite(&type, sizeof(uint8_t), 1, f);
    } else if (IS_BOOL(value)) {
        uint8_t type = 2;
        fwrite(&type, sizeof(uint8_t), 1, f);
        uint8_t b = AS_BOOL(value) ? 1 : 0;
        fwrite(&b, sizeof(uint8_t), 1, f);
    } else if (IS_OBJ(value)) {
        switch (AS_OBJ(value)->type) {
            case OBJ_STRING: {
                uint8_t type = 3;
                fwrite(&type, sizeof(uint8_t), 1, f);
                write_string(f, AS_STRING(value));
                break;
            }
            case OBJ_FUNCTION: {
                uint8_t type = 4;
                fwrite(&type, sizeof(uint8_t), 1, f);
                write_function(vm, f, AS_FUNCTION(value));
                break;
            }
            default:
                fprintf(stderr, "Cannot serialize object type %d\n", AS_OBJ(value)->type);
                exit(1);
        }
    }
}

static bool read_value(VM *vm, FILE *f, Value *out) {
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
            if (!read_string(vm, f, false, &string)) return false;
            *out = OBJ_VAL(string);
            return true;
        }
        case 4: {
            ObjFunction *function = read_function(vm, f);
            if (!function) return false;
            *out = OBJ_VAL(function);
            return true;
        }
        default:
            return false;
    }
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

static bool validate_function(ObjFunction *function) {
    if (!function || function->reg_count <= 0 || function->reg_count > MAX_REGISTERS) {
        return false;
    }

    Chunk *chunk = &function->chunk;
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
            case OP_GETUPVAL:
            case OP_SETUPVAL:
            case OP_NEG:
            case OP_NOT:
            case OP_FORPREP:
            case OP_FORPREP_NUM:
            case OP_ARRAY_PUSH:
            case OP_ITER_PREP:
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst))) return false;
                if ((op == OP_FORPREP || op == OP_FORPREP_NUM) && !valid_reg(function, GET_A(inst) + 1)) return false;
                if (op == OP_GETUPVAL || op == OP_SETUPVAL) {
                    if ((int)GET_B(inst) >= function->upvalue_count) return false;
                }
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
            case OP_SETFIELD_IDX:
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
                if (!valid_reg(function, GET_A(inst)) || !valid_reg(function, GET_B(inst))) return false;
                break;
            case OP_AUX:
                break;
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

    return true;
}

bool vm_save_bytecode(VM *vm, ObjFunction *function, const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return false;

    uint32_t magic = MGC_MAGIC;
    fwrite(&magic, sizeof(uint32_t), 1, f);

    write_function(vm, f, function);

    fclose(f);
    return true;
}

ObjFunction *vm_load_bytecode(VM *vm, const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    uint32_t magic;
    if (fread(&magic, sizeof(uint32_t), 1, f) != 1 || magic != MGC_MAGIC) {
        fclose(f);
        return NULL;
    }

    ObjFunction *function = read_function(vm, f);
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
