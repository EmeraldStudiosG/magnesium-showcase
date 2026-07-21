#ifndef MAGNESIUM_H
#define MAGNESIUM_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdatomic.h>
#ifndef _WIN32
#include <pthread.h>
#endif

/* ========================================================================
 * Core Header
 * ======================================================================== */

/* Configuration */
#define MG_VERSION_MAJOR 1
#define MG_VERSION_MINOR 0
#define MG_VERSION_PATCH 0
#define MG_VERSION_STRING "1.0.0"

#define MAX_REGISTERS   256
#define MAX_CONSTANTS   65536
#define MAX_LOCALS      256
#define MAX_UPVALUES    256
#define MAX_CALL_FRAMES 256
#define STACK_INIT_SIZE 65536
#define GC_HEAP_GROW_FACTOR 2

#define MG_ATOMIC_LOAD_BOOL(flag) atomic_load_explicit(&(flag), memory_order_acquire)
#define MG_ATOMIC_STORE_BOOL(flag, value) atomic_store_explicit(&(flag), (value), memory_order_release)

/* Slab allocator for small heap objects */
#define SLAB_CLASS_COUNT 14
#define SLAB_SIZE        (64 * 1024)  /* 64 KiB per slab */
#define MAX_SLAB_SIZE    1024
#define INT_STR_CACHE_SIZE 262144

typedef struct Slab {
    struct Slab *next;
    uint8_t     *memory;
} Slab;

#if defined(_MSC_VER)
#define MG_THREAD_LOCAL __declspec(thread)
#else
#define MG_THREAD_LOCAL _Thread_local
#endif

/* ========================================================================
 * Forward Declarations
 * ======================================================================== */
typedef struct Obj Obj;
typedef struct ObjString ObjString;
typedef struct ObjArray ObjArray;
typedef struct ObjDict ObjDict;
typedef struct ObjFunction ObjFunction;
typedef struct ObjClosure ObjClosure;
typedef struct ObjUpvalue ObjUpvalue;
typedef struct ObjNative ObjNative;
typedef struct ObjFFI ObjFFI;
typedef struct ObjNativeHandle ObjNativeHandle;
typedef struct ObjStruct ObjStruct;
typedef struct ObjInstance ObjInstance;
typedef struct ObjError ObjError;
typedef struct ObjVMTask ObjVMTask;
typedef struct ObjCoroutine ObjCoroutine;
typedef struct VM VM;

/* ========================================================================
 * Token Types
 * ======================================================================== */
typedef enum {
    /* Single-character tokens */
    TOKEN_LEFT_PAREN, TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACE, TOKEN_RIGHT_BRACE,
    TOKEN_LEFT_BRACKET, TOKEN_RIGHT_BRACKET,
    TOKEN_COMMA, TOKEN_DOT, TOKEN_MINUS, TOKEN_PLUS,
    TOKEN_SEMICOLON, TOKEN_SLASH, TOKEN_STAR, TOKEN_COLON,
    TOKEN_AT, /* @ for globals */
    TOKEN_PERCENT, /* % modulo */
    TOKEN_QUESTION,

    /* One or two character tokens */
    TOKEN_BANG, TOKEN_BANG_EQUAL,
    TOKEN_EQUAL, TOKEN_EQUAL_EQUAL,
    TOKEN_GREATER, TOKEN_GREATER_EQUAL,
    TOKEN_LESS, TOKEN_LESS_EQUAL,
    TOKEN_DOT_DOT, TOKEN_DOT_DOT_EQUAL, /* .. and ..= */
    TOKEN_PLUS_EQUAL, TOKEN_MINUS_EQUAL,
    TOKEN_STAR_EQUAL, TOKEN_SLASH_EQUAL, TOKEN_PERCENT_EQUAL,

    /* Literals */
    TOKEN_IDENTIFIER, TOKEN_STRING, TOKEN_INTERP_STRING, TOKEN_NUMBER,

    /* Keywords */
    TOKEN_AND, TOKEN_AS, TOKEN_BREAK, TOKEN_CATCH, TOKEN_CONST, TOKEN_CONTINUE,
    TOKEN_DEFER, TOKEN_ELSE, TOKEN_ELSEIF, TOKEN_END, TOKEN_ENUM,
    TOKEN_EXPORT, TOKEN_FALSE, TOKEN_FN, TOKEN_FOR,
    TOKEN_IF, TOKEN_IMPORT, TOKEN_IN, TOKEN_LET,
    TOKEN_LOOP, TOKEN_NOT, TOKEN_NULL, TOKEN_OR, TOKEN_RETURN,
    TOKEN_STRUCT, TOKEN_THEN, TOKEN_TRUE, TOKEN_TRY,

    TOKEN_AMPERSAND,
    TOKEN_ERROR, TOKEN_EOF,
    TOKEN_COUNT /* sentinel for table sizing */
} MgTokenType;

typedef struct {
    MgTokenType type;
    const char *start;
    int length;
    int line;
} Token;

typedef struct {
    const char *start;
    const char *current;
    int line;
} Scanner;

/* ========================================================================
 * Value Representation (Tagged Union)
 * ======================================================================== */
#include <stdint.h>
#include <string.h>

#define QNAN     ((uint64_t)0x7ffc000000000000)
#define SIGN_BIT ((uint64_t)0x8000000000000000)

#define TAG_NULL   1
#define TAG_FALSE  2
#define TAG_TRUE   3
#define TAG_INT    4

typedef uint64_t Value;

typedef union { uint64_t as_bits; double as_num; } ValuePun;

static inline bool IS_INT_VALUE(Value value) {
    return (value & (SIGN_BIT | QNAN | 0x7)) == (QNAN | TAG_INT);
}

static inline int32_t AS_INT_VALUE(Value value) {
    return (int32_t)((value >> 3) & 0xffffffffu);
}

static inline Value NUMBER_VAL(double num) {
    ValuePun p; p.as_num = num; return p.as_bits;
}

static inline Value INT_VAL(int32_t num) {
    return (Value)(QNAN | TAG_INT | ((uint64_t)(uint32_t)num << 3));
}

static inline double AS_NUMBER(Value value) {
    if (IS_INT_VALUE(value)) return (double)AS_INT_VALUE(value);
    ValuePun p; p.as_bits = value; return p.as_num;
}

static inline double AS_DOUBLE(Value value) {
    ValuePun p; p.as_bits = value; return p.as_num;
}

static inline Value NUMBER_AUTO_VAL(double num) {
    if (num >= (double)INT32_MIN && num <= (double)INT32_MAX) {
        int32_t whole = (int32_t)num;
        if ((double)whole == num) return INT_VAL(whole);
    }
    return NUMBER_VAL(num);
}

#define NULL_VAL           ((Value)(uint64_t)(QNAN | TAG_NULL))
#define FALSE_VAL          ((Value)(uint64_t)(QNAN | TAG_FALSE))
#define TRUE_VAL           ((Value)(uint64_t)(QNAN | TAG_TRUE))
#define BOOL_VAL(b)        ((b) ? TRUE_VAL : FALSE_VAL)
#define OBJ_VAL(obj)       (Value)(SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(obj))

#define AS_BOOL(value)     ((value) == TRUE_VAL)
#define AS_OBJ(value)      ((Obj*)(uintptr_t)((value) & ~(SIGN_BIT | QNAN)))

#define IS_INT(value)      IS_INT_VALUE(value)
#define AS_INT(value)      AS_INT_VALUE(value)
#define IS_NUMBER(value)   (((value) & QNAN) != QNAN)
#define IS_NUMERIC(value)  (IS_INT_VALUE(value) || IS_NUMBER(value))
#define IS_NULL(value)     ((value) == NULL_VAL)
#define IS_BOOL(value)     (((value) | 1) == TRUE_VAL)
#define IS_OBJ(value)      (((value) & (SIGN_BIT | QNAN)) == (SIGN_BIT | QNAN))

/* Only false and null are falsey */
#define IS_FALSEY(v)       (IS_NULL(v) || (IS_BOOL(v) && !AS_BOOL(v)))

bool values_equal(Value a, Value b);

void print_value(Value value);

/* ========================================================================
 * Heap Object System
 * ======================================================================== */
typedef enum {
    OBJ_STRING,
    OBJ_ARRAY,
    OBJ_DICT,
    OBJ_FUNCTION,
    OBJ_CLOSURE,
    OBJ_UPVALUE,
    OBJ_NATIVE,
    OBJ_FFI,
    OBJ_NATIVE_HANDLE,
    OBJ_STRUCT,
    OBJ_INSTANCE,
    OBJ_ERROR,
    OBJ_VM_TASK,
    OBJ_COROUTINE
} ObjType;

/* Base object header every heap object starts with this */
struct Obj {
    ObjType type;
    bool is_marked;   /* GC mark bit */
    bool is_old;      /* true = old generation, false = young */
    uint8_t size_class; /* 0 = malloc'd, otherwise slab class index */
    uint8_t gc_age;   /* nursery collections survived while young */
    Obj *next;        /* Intrusive linked list for GC tracking */
    size_t alloc_size; /* bytes requested for the object body */
};

#define OBJ_TYPE(v)        (AS_OBJ(v)->type)
#define IS_STRING(v)       (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_STRING)
#define IS_ARRAY(v)        (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_ARRAY)
#define IS_DICT(v)         (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_DICT)
#define IS_FUNCTION(v)     (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_FUNCTION)
#define IS_CLOSURE(v)      (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_CLOSURE)
#define IS_NATIVE(v)       (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_NATIVE)
#define IS_FFI(v)          (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_FFI)
#define IS_NATIVE_HANDLE(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_NATIVE_HANDLE)
#define IS_STRUCT(v)       (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_STRUCT)
#define IS_INSTANCE(v)     (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_INSTANCE)
#define IS_ERROR(v)        (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_ERROR)
#define IS_VM_TASK(v)      (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_VM_TASK)
#define IS_COROUTINE(v)    (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_COROUTINE)

#define AS_STRING(v)       ((ObjString*)AS_OBJ(v))
#define AS_CSTRING(v)      (((ObjString*)AS_OBJ(v))->chars)
#define AS_ARRAY(v)        ((ObjArray*)AS_OBJ(v))
#define AS_DICT(v)         ((ObjDict*)AS_OBJ(v))
#define AS_FUNCTION(v)     ((ObjFunction*)AS_OBJ(v))
#define AS_CLOSURE(v)      ((ObjClosure*)AS_OBJ(v))
#define AS_NATIVE(v)       ((ObjNative*)AS_OBJ(v))
#define AS_FFI(v)          ((ObjFFI*)AS_OBJ(v))
#define AS_NATIVE_HANDLE(v) ((ObjNativeHandle*)AS_OBJ(v))
#define AS_STRUCT_OBJ(v)   ((ObjStruct*)AS_OBJ(v))
#define AS_INSTANCE(v)     ((ObjInstance*)AS_OBJ(v))
#define AS_ERROR(v)        ((ObjError*)AS_OBJ(v))
#define AS_VM_TASK(v)      ((ObjVMTask*)AS_OBJ(v))
#define AS_COROUTINE(v)    ((ObjCoroutine*)AS_OBJ(v))

/* String */
struct ObjString {
    Obj obj;
    int length;
    int capacity;
    uint32_t hash;
    bool is_rope;
    char *chars;
    ObjString *left;
    ObjString *right;
};

/* Array */
struct ObjArray {
    Obj obj;
    int count;
    int capacity;
    Value *items;
};

/* Dictionary Entry */
typedef struct {
    ObjString *key;    /* NULL = empty slot, TOMBSTONE_KEY = deleted */
    uint32_t hash;     /* key->hash, 0 if empty/deleted */
    Value value;
} DictEntry;

#define TOMBSTONE_KEY ((ObjString *)(uintptr_t)1)

/* Dictionary
 *
 * Compact layout: a separate index array makes probing cache-friendly,
 * while a contiguous entries array preserves insertion order.
 *
 * indices[] values:
 *   0         = empty slot
 *   >0        = active entry at entries[value - 1]
 *   TOMBSTONE_IDX = deleted slot (still probes)
 * entries[key] values:
 *   key == NULL       = never used (tail of entries array)
 *   key == TOMBSTONE_KEY = deleted entry
 */
#define TOMBSTONE_IDX (-1)

struct ObjDict {
    Obj obj;
    int count;           /* number of active entries */
    int capacity;        /* size of indices array (power of 2) */
    uint32_t version;
    ObjString *mono_cache_key;
    DictEntry *mono_cache_entry;
    uint32_t mono_cache_version;
    int32_t *indices;    /* probe table: 0=empty, TOMBSTONE_IDX=deleted, >0=entry index+1 */
    int entry_count;     /* total entries allocated (active + deleted) */
    int entry_capacity;  /* allocated capacity of entries array */
    DictEntry *entries;  /* compact entry array */
};

/* Hash Table (globals, string interning, field index) */
typedef struct {
    ObjString *key;
    uint32_t hash;
    Value value;
} TableEntry;

typedef struct {
    int count;
    int capacity;
    TableEntry *entries;
} Table;

/* ========================================================================
 * Bytecode Chunk
 * ======================================================================== */
typedef uint32_t Instruction;

/*
 * Instruction Encoding:
 * Mode A: [8-bit Op] [8-bit A] [8-bit B] [8-bit C]
 * Mode B: [8-bit Op] [8-bit A] [16-bit Bx]
 * Mode C: [8-bit Op] [24-bit sBx] (signed for jumps)
 */
#define ENCODE_ABC(op,a,b,c) ((Instruction)(op) | ((Instruction)(a)<<8) | ((Instruction)(b)<<16) | ((Instruction)(c)<<24))
#define ENCODE_ABx(op,a,bx)  ((Instruction)(op) | ((Instruction)(a)<<8) | ((Instruction)(bx)<<16))
#define ENCODE_sBx(op,sbx)   ((Instruction)(op) | ((Instruction)((sbx)+0x7FFFFF)<<8))
#define ENCODE_AsBx(op,a,sbx) ((Instruction)(op) | ((Instruction)(a)<<8) | ((Instruction)((sbx)+0x8000)<<16))

#define GET_OPCODE(inst) ((inst) & 0xFF)
#define GET_A(inst)      (((inst) >> 8) & 0xFF)
#define GET_B(inst)      (((inst) >> 16) & 0xFF)
#define GET_sB(inst)     ((int8_t)GET_B(inst))
#define GET_C(inst)      (((inst) >> 24) & 0xFF)
#define GET_sC(inst)     ((int8_t)GET_C(inst))
#define GET_Bx(inst)     (((inst) >> 16) & 0xFFFF)
#define GET_sBx(inst)    ((int)(((inst) >> 8) & 0xFFFFFF) - 0x7FFFFF)
#define GET_AsBx_A(inst)  (((inst) >> 8) & 0xFF)
#define GET_AsBx_sBx(inst) ((int)(((inst) >> 16) & 0xFFFF) - 0x8000)

/* Opcode set */
typedef enum {
    /* Load/Store */
    OP_LOADK,       /* A Bx    : R[A] = K[Bx]                     */
    OP_LOADBOOL,    /* A B C   : R[A] = (bool)B; if C then pc++    */
    OP_LOADNIL,     /* A B     : R[A..A+B] = nil                   */
    OP_MOVE,        /* A B     : R[A] = R[B]                       */

    /* Globals */
    OP_GETGLOBAL,   /* A Bx    : R[A] = globals[K[Bx]]            */
    OP_SETGLOBAL,   /* A Bx    : globals[K[Bx]] = R[A]            */

    /* Upvalues */
    OP_GETUPVAL,    /* A B     : R[A] = upvalues[B]                */
    OP_SETUPVAL,    /* A B     : upvalues[B] = R[A]                */

    /* Arithmetic (register-register) */
    OP_ADD,         /* A B C   : R[A] = R[B] + R[C]               */
    OP_SUB,         /* A B C   : R[A] = R[B] - R[C]               */
    OP_MUL,         /* A B C   : R[A] = R[B] * R[C]               */
    OP_DIV,         /* A B C   : R[A] = R[B] / R[C]               */
    OP_MOD,         /* A B C   : R[A] = R[B] % R[C]               */
    OP_NEG,         /* A B     : R[A] = -R[B]                      */

    /* Fused arithmetic (register + constant) */
    OP_ADDK,        /* A B C   : R[A] = R[B] + K[C]               */
    OP_SUBK,        /* A B C   : R[A] = R[B] - K[C]               */
    OP_MULK,        /* A B C   : R[A] = R[B] * K[C]               */
    OP_DIVK,        /* A B C   : R[A] = R[B] / K[C]               */
    OP_MODK,        /* A B C   : R[A] = R[B] % K[C]               */
    OP_ADDI,        /* A B C   : R[A] = R[B] + (int8_t)C          */
    OP_SUBI,        /* A B C   : R[A] = R[B] - (int8_t)C          */
    OP_MULI,        /* A B C   : R[A] = R[B] * (int8_t)C          */
    OP_DIVI,        /* A B C   : R[A] = R[B] / (int8_t)C          */
    OP_MODI,        /* A B C   : R[A] = R[B] % (int8_t)C          */

    /* Comparison (sets result in R[A]) */
    OP_EQ,          /* A B C   : R[A] = R[B] == R[C]              */
    OP_NEQ,         /* A B C   : R[A] = R[B] != R[C]              */
    OP_LT,          /* A B C   : R[A] = R[B] <  R[C]              */
    OP_LE,          /* A B C   : R[A] = R[B] <= R[C]              */
    OP_EQI,         /* A B C   : R[A] = R[B] == (int8_t)C         */
    OP_NEQI,        /* A B C   : R[A] = R[B] != (int8_t)C         */
    OP_LTI,         /* A B C   : R[A] = R[B] <  (int8_t)C         */
    OP_LEI,         /* A B C   : R[A] = R[B] <= (int8_t)C         */
    OP_GTI,         /* A B C   : R[A] = R[B] >  (int8_t)C         */
    OP_GEI,         /* A B C   : R[A] = R[B] >= (int8_t)C         */
    OP_EQI_TEST,    /* A B     : if !(R[A] == (int8_t)B) then inline jump */
    OP_NEQI_TEST,   /* A B     : if !(R[A] != (int8_t)B) then inline jump */
    OP_LTI_TEST,    /* A B     : if !(R[A] <  (int8_t)B) then inline jump */
    OP_LEI_TEST,    /* A B     : if !(R[A] <= (int8_t)B) then inline jump */
    OP_GTI_TEST,    /* A B     : if !(R[A] >  (int8_t)B) then inline jump */
    OP_GEI_TEST,    /* A B     : if !(R[A] >= (int8_t)B) then inline jump */
    OP_MODI_EQI_TEST,  /* A B C : if !(R[A] % (int8_t)B == (int8_t)C) then jump */
    OP_MODI_NEQI_TEST, /* A B C : if !(R[A] % (int8_t)B != (int8_t)C) then jump */

    /* Logical */
    OP_NOT,         /* A B     : R[A] = !R[B]                      */
    OP_TEST,        /* A C     : if (bool)R[A] != C then pc++      */
    OP_TESTSET,     /* A B C   : if (bool)R[B] == C then R[A]=R[B] else pc++ */
    OP_TESTJMP,     /* A sBx   : if IS_FALSEY(R[A]) then pc += sBx */
    OP_TESTERRJMP,  /* A sBx   : if !IS_ERROR(R[A]) then pc += sBx */

    /* String */
    OP_CONCAT,      /* A B C   : R[A] = R[B] .. R[B+1] .. ... .. R[C] */
    OP_TOSTRING,    /* A B     : R[A] = tostring(R[B])               */
    OP_LEN,         /* A B     : R[A] = len(R[B])                    */

    /* Jump / Control flow */
    OP_JMP,         /* sBx     : pc += sBx                         */
    OP_LOOP,        /* sBx     : pc -= sBx (loop back)             */
    OP_FORPREP,     /* A       : validate range R[A]..R[A+1]; R[A] -= 1 */
    OP_FORPREP_NUM, /* A       : numeric-only range prep; never int-tags R[A] */
    OP_FORLOOP,     /* A sBx   : R[A] += 1; if R[A] >= R[A+1] then pc += sBx */
    OP_FORLOOP_INC, /* A sBx   : R[A] += 1; if R[A] >  R[A+1] then pc += sBx */
    OP_FORLOOP_NUM,     /* A sBx : numeric-only exclusive FORLOOP */
    OP_FORLOOP_INC_NUM, /* A sBx : numeric-only inclusive FORLOOP */
    OP_FORADDLOCAL_FIELD_PROP,     /* A B C + AUX : for A..A+1, R[B] += R[C].K? */
    OP_FORADDLOCAL_FIELD_PROP_INC, /* A B C + AUX : inclusive variant */
    OP_FORADDGLOBAL_FIELD_PROP,     /* A B C + AUX AUX : global += R[C].K? */
    OP_FORADDGLOBAL_FIELD_PROP_INC, /* A B C + AUX AUX : inclusive variant */
    OP_FOR_MODI_ACCUM,     /* A B C + AUX AUX : for range, R[B] += residue-selected immediates */
    OP_FOR_MODI_ACCUM_INC, /* inclusive variant */
    OP_FOR_FIELD2_ACCUM,     /* A B C + AUX AUX : for range, update two struct fields */
    OP_FOR_FIELD2_ACCUM_INC, /* inclusive variant */
    OP_ARRAY_MARK_FALSE_STRIDE, /* A B C + AUX : R[A][R[B]..R[C] step AUX] = false */

    /* Functions */
    OP_CLOSURE,     /* A Bx    : R[A] = closure(K[Bx])            */
    OP_CALL,        /* A B C   : R[A..A+C-1] = R[A](R[A+1..A+B]) */
    OP_CALLG,       /* A B C   : R[A] = globals[K[C]](R[A+1..A+B]) */
    OP_MCALL,       /* A B C   : Same as CALL, but checks if R[A+1] is instance */
    OP_CALLR,       /* A B C   : R[A] = R[B](R[A+1..A+C]); one return */
    OP_CALLSELF,    /* A B C   : R[A] = current_closure(R[A+1..A+B]) */
    OP_ADDUP,       /* A B     : upvalue[A] = upvalue[A] + R[B]       */
    OP_ADDLOCAL,    /* A B     : R[A] = R[A] + R[B]                   */
    OP_SUBLOCAL,    /* A B     : R[A] = R[A] - R[B]                   */
    OP_ADDLOCAL_FIELD_PROP, /* A B C : R[A] += R[B].K[C]? or return null,error */
    OP_SUBLOCAL_FIELD_PROP, /* A B C : R[A] -= R[B].K[C]? or return null,error */
    OP_ADDLOCAL_LEN, /* A B : R[A] += len(R[B]) */
    OP_SUBLOCAL_LEN, /* A B : R[A] -= len(R[B]) */
    OP_ADDLOCAL_MULI, /* A B C : R[A] += R[B] * (int8_t)C             */
    OP_SUBLOCAL_MULI, /* A B C : R[A] -= R[B] * (int8_t)C             */
    OP_ADDLOCAL_MULK, /* A B C : R[A] += R[B] * K[C]                  */
    OP_SUBLOCAL_MULK, /* A B C : R[A] -= R[B] * K[C]                  */
    OP_ADDLOCAL_DIVI, /* A B C : R[A] += R[B] / (int8_t)C             */
    OP_SUBLOCAL_DIVI, /* A B C : R[A] -= R[B] / (int8_t)C             */
    OP_ADDLOCAL_MODI, /* A B C : R[A] += R[B] % (int8_t)C             */
    OP_SUBLOCAL_MODI, /* A B C : R[A] -= R[B] % (int8_t)C             */
    OP_RETURN,      /* A B     : return R[A..A+B-1]                */

    /* Data structures */
    OP_NEWARRAY,    /* A B     : R[A] = new array of size B        */
    OP_SETARRAY,    /* A B C   : R[A][B] = R[C]                   */
    OP_ARRAY_PUSH,  /* A B     : push R[B] into array R[A]         */
    OP_GETINDEX,    /* A B C   : R[A] = R[B][R[C]]                */
    OP_GETINDEX_TRY,/* A B C   : R[A] = R[B][R[C]] or return nil,error */
    OP_SETINDEX,    /* A B C   : R[A][R[B]] = R[C]                */
    OP_NEWDICT,     /* A       : R[A] = new dict                   */

    /* Fields (for structs/dicts with string keys) */
    OP_GETFIELD,    /* A B C   : R[A] = R[B].K[C]                 */
    OP_GETFIELD_TRY,/* A B C   : R[A],R[A+1] = R[B].K[C] or error */
    OP_GETFIELD_PROP,/* A B C  : R[A] = R[B].K[C] or return null,error */
    OP_SETFIELD,    /* A B C   : R[A].K[B] = R[C]                 */
    OP_GETFIELD_IDX, /* A B C  : R[A] = R[B].fields[C] - O(1) field by index */
    OP_SETFIELD_IDX, /* A B C  : R[A].fields[C] = R[B] - O(1) field by index */

    /* Fused ternary arithmetic (2 words: ABC + AUX) */
    OP_ADDSUB,      /* A B C + AUX : R[A] = R[B] + R[C] - R[AUX] */
    OP_SUBADD,      /* A B C + AUX : R[A] = R[B] - R[C] + R[AUX] */
    OP_MULADD,      /* A B C + AUX : R[A] = R[B] * R[C] + R[AUX] */
    OP_MULSUB,      /* A B C + AUX : R[A] = R[B] * R[C] - R[AUX] */

    /* Fused comparison-of-sum (2 words: ABC + JMP) */
    OP_ADD_GT_TEST, /* A B C : if R[A]+R[B] > (int8_t)C then skip JMP */
    OP_ADD_GE_TEST, /* A B C : if R[A]+R[B] >= (int8_t)C then skip JMP */
    OP_SUB_GT_TEST, /* A B C : if R[A]-R[B] > (int8_t)C then skip JMP */
    OP_SUB_GE_TEST, /* A B C : if R[A]-R[B] >= (int8_t)C then skip JMP */

    /* Fused multiply-accumulate into local */
    OP_MULLOCAL_ADD,  /* A B C : R[A] += R[B] * R[C] */
    OP_MULLOCAL_SUB,  /* A B C : R[A] -= R[B] * R[C] */

    /* Iterators */
    OP_ITER_PREP,   /* A B     : prepare iterator on R[B], store in R[A] */
    OP_ITER_NEXT,   /* A B C   : R[A],R[A+1]=next(R[B]); if done jmp +C */

    /* Misc */
    OP_AUX,        /* aux data word consumed by previous instruction */
    OP_CLOSE_UPVAL, /* A       : close upvalues >= R[A]            */
    OP_DEFER,       /* A       : push R[A] onto defer stack        */
    OP_NEWSTRUCT,   /* A Bx    : R[A] = new instance of struct K[Bx] */
    OP_MCALLFIELD,  /* A B C   : R[A] = R[A+1].K[C](R[A+2..A+B+1]); monomorphic method call */
    OP_MCALLFIELD0, /* A B C   : side-effect-only MCALLFIELD, expects no return values */

    OP_ADDI_LOOP,   /* A B C   : R[A] += (int8_t)B; ip -= C; cancel check */

    OP_COUNT        /* sentinel */
} OpCode;

/* ========================================================================
 * Bytecode Chunk (holds instructions + constants + debug info)
 * ======================================================================== */
typedef struct {
    int count;
    int capacity;
    Instruction *code;
    int *lines;          /* line number per instruction for errors */

    int const_count;
    int const_capacity;
    Value *constants;
} Chunk;

void chunk_init(Chunk *chunk);
void chunk_free(Chunk *chunk);
void chunk_write(Chunk *chunk, Instruction inst, int line);
int chunk_add_constant(Chunk *chunk, Value value);

/* ========================================================================
 * Function Object (compiled bytecode + metadata)
 * ======================================================================== */
struct ObjFunction {
    Obj obj;
    int arity;           /* number of parameters */
    int upvalue_count;
    int reg_count;       /* actual number of registers used (for stack sizing) */
    Chunk chunk;
    ObjString *name;     /* NULL for top-level script */
    ObjString *source_name;
};

/* ========================================================================
 * Closure (function + captured upvalues)
 * ======================================================================== */
struct ObjUpvalue {
    Obj obj;
    Value *location;     /* pointer to the variable (on stack or closed) */
    Value closed;        /* storage for closed-over value */
    ObjUpvalue *next;    /* linked list for open upvalues */
};

struct ObjClosure {
    Obj obj;
    ObjFunction *function;
    int upvalue_count;
    ObjUpvalue *upvalues[];  /* flexible array member: eliminates 1 malloc per closure */
};

typedef struct {
    ObjClosure *closure;
    Instruction *ip;         /* instruction pointer */
    Value *slots;            /* pointer into register window */
    int call_dest;           /* register in caller where result should be stored */
    int expected_returns;    /* how many return values the caller expects */
} CallFrame;

typedef struct SavedVMContext {
    Value *stack;
    int stack_capacity;
    int stack_size;
    int stack_top;
    CallFrame frames[MAX_CALL_FRAMES];
    int frame_count;
    ObjUpvalue *open_upvalues;
    ObjClosure **defer_items;
    int defer_count;
    int defer_capacity;
    ObjCoroutine *current_coroutine;
    bool yield_requested;
    Value yield_values[256];
    int yield_count;
    Value native_return_values[256];
    int native_return_count;
    struct SavedVMContext *next;
} SavedVMContext;

/* ========================================================================
 * Native Function
 * ======================================================================== */
typedef Value (*NativeFn)(VM *vm, int arg_count, Value *args);
typedef void (*NativeHandleFinalizer)(void *data);

struct ObjNative {
    Obj obj;
    NativeFn function;
    const char *name;
    int arity;           /* -1 = variadic */
    void *userdata;      /* opaque pointer for FFI trampolines */
    void (*userdata_finalizer)(void *);  /* called when GC frees this ObjNative */
};

/* ========================================================================
 * FFI Native Function
 * ======================================================================== */
struct ObjFFI {
    Obj obj;
    void *c_function;
    const char *name;
    int arity;
};

struct ObjNativeHandle {
    Obj obj;
    void *data;
    ObjString *type_name;
    ObjDict *methods;
    NativeHandleFinalizer finalizer;
};

/* ========================================================================
 * Struct Definition & Instance
 * ======================================================================== */
struct ObjStruct {
    Obj obj;
    ObjString *name;
    ObjDict *methods;
    ObjString **field_names;
    int field_count;
    Table field_index;
};

struct ObjInstance {
    Obj obj;
    ObjStruct *klass;
    Value fields[];             /* flexible array member: eliminates separate malloc */
};

/* Structured recoverable error value */
struct ObjError {
    Obj obj;
    ObjString *kind;
    ObjString *message;
    ObjString *file;
    ObjString *function;
    ObjString *hint;
    int line;
};

typedef enum {
    VM_TASK_RUNNING,
    VM_TASK_DONE
} VMTaskState;

struct ObjVMTask {
    Obj obj;
    char *path;
    VMTaskState state;
    int result;
    bool started;
    bool joined;
    atomic_bool cancel_requested;
    VM *child_vm;
    char error_kind[64];
    char error_message[512];
    char error_hint[512];
#ifdef _WIN32
    void *thread;
    void *lock;
#else
    pthread_t thread;
    pthread_mutex_t lock;
#endif
};

typedef enum {
    COROUTINE_NEW,
    COROUTINE_RUNNING,
    COROUTINE_SUSPENDED,
    COROUTINE_DEAD
} CoroutineState;

struct ObjCoroutine {
    Obj obj;
    ObjClosure *closure;
    CoroutineState state;
    Value *stack;
    int stack_size;
    int stack_capacity;
    int stack_top;
    CallFrame frames[MAX_CALL_FRAMES];
    int frame_count;
    ObjUpvalue *open_upvalues;
    ObjClosure **defer_items;
    int defer_count;
    int defer_capacity;
    Value yield_values[256];
    int yield_count;
};

/* ========================================================================
 * Abstract Syntax Tree
 * ======================================================================== */
typedef enum {
    /* Literals */
    NODE_NUMBER,
    NODE_STRING,
    NODE_INTERP_STRING,
    NODE_BOOL,
    NODE_NULL,
    NODE_IDENTIFIER,

    /* Expressions */
    NODE_UNARY,
    NODE_BINARY,
    NODE_LOGICAL,
    NODE_ASSIGN,
    NODE_CALL,
    NODE_INDEX,
    NODE_TRY,
    NODE_FIELD_GET,
    NODE_FIELD_SET,
    NODE_ARRAY_LITERAL,
    NODE_DICT_LITERAL,
    NODE_STRUCT_LITERAL,

    /* Statements */
    NODE_EXPRESSION_STMT,
    NODE_LET,
    NODE_CONST,
    NODE_BLOCK,
    NODE_IF,
    NODE_LOOP,
    NODE_FOR_RANGE,
    NODE_FOR_IN,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_RETURN,
    NODE_TRY_BLOCK,
    NODE_TRY_CATCH,
    NODE_DEFER,
    NODE_FN_DECL,
    NODE_STRUCT_DECL,
    NODE_ENUM_DECL,

    /* Module (stubbed) */
    NODE_IMPORT,
    NODE_EXPORT,

    /* Static/tooling-only declarations */
    NODE_DIRECTIVE,
    NODE_TYPE_ALIAS,
    NODE_EXTERN_DECL
} NodeType;

/* AST Node */
typedef struct ASTNode ASTNode;
typedef struct MgTypeRef MgTypeRef;

typedef enum {
    MG_TYPE_NAME,
    MG_TYPE_ARRAY,
    MG_TYPE_DICT,
    MG_TYPE_FUNCTION,
    MG_TYPE_SHAPE
} MgTypeKind;

typedef enum {
    MG_DIRECTIVE_STRICT,
    MG_DIRECTIVE_NOCHECK
} MgDirectiveKind;

/* Array of AST node pointers used for block bodies, argument lists, etc */
typedef struct {
    ASTNode **nodes;
    int count;
    int capacity;
} NodeList;

/* Key-value pair for dict/struct literals */
typedef struct {
    ASTNode *key;
    ASTNode *value;
} KVPair;

typedef struct {
    KVPair *pairs;
    int count;
    int capacity;
} KVList;

typedef struct {
    Token name;
    MgTypeRef *type;
} MgTypeField;

struct MgTypeRef {
    MgTypeKind kind;
    int line;
    union {
        struct { Token name; } name;
        struct { MgTypeRef *element; } array;
        struct { MgTypeRef *key; MgTypeRef *value; } dict;
        struct {
            MgTypeRef **params;
            int param_count;
            MgTypeRef *return_type;
        } function;
        struct {
            MgTypeField *fields;
            int field_count;
            int field_capacity;
        } shape;
    } as;
};

/* Parameter definition */
typedef struct {
    Token name;
    MgTypeRef *type;
} Param;

struct ASTNode {
    NodeType type;
    int line;

    union {
        /* NODE_NUMBER */
        struct { double value; } number;

        /* NODE_STRING, NODE_INTERP_STRING */
        struct { char *value; int length; } string;

        /* NODE_BOOL */
        struct { bool value; } boolean;

        /* NODE_IDENTIFIER */
        struct { Token name; bool is_global; } identifier;

        /* NODE_UNARY: op <operand> */
        struct { MgTokenType op; ASTNode *operand; } unary;

        /* NODE_BINARY: <left> op <right> */
        struct { MgTokenType op; ASTNode *left; ASTNode *right; } binary;

        /* NODE_LOGICAL: <left> (and|or) <right> */
        struct { MgTokenType op; ASTNode *left; ASTNode *right; } logical;

        /* NODE_ASSIGN: <target> = <value> */
        struct { ASTNode *target; ASTNode *value; } assign;

        /* NODE_CALL: <callee>(args...) */
        struct { ASTNode *callee; NodeList args; } call;

        /* NODE_INDEX: <object>[<index>] */
        struct { ASTNode *object; ASTNode *index; } index_expr;

        /* NODE_TRY: <expr>? */
        struct { ASTNode *expr; } try_expr;

        /* NODE_FIELD_GET: <object>.<name> */
        struct { ASTNode *object; Token name; } field_get;

        /* NODE_FIELD_SET: <object>.<name> = <value> */
        struct { ASTNode *object; Token name; ASTNode *value; } field_set;

        /* NODE_ARRAY_LITERAL: [items...] */
        struct { NodeList items; } array_literal;

        /* NODE_DICT_LITERAL: {key=val, ...} */
        struct { KVList entries; } dict_literal;

        /* NODE_STRUCT_LITERAL: Name { field = val, ... } */
        struct { Token name; KVList fields; } struct_literal;

        /* NODE_EXPRESSION_STMT */
        struct { ASTNode *expr; } expr_stmt;

        /* NODE_LET / NODE_CONST */
        struct {
            Token name;          /* first (or only) variable name */
            Token *extra_names;  /* additional names for multi-return: let a, b = f() */
            MgTypeRef *type_annotation;
            MgTypeRef **extra_type_annotations;
            int name_count;      /* total count (1 = single, >1 = multi) */
            bool is_global;      /* has @ prefix */
            bool is_const;
            ASTNode *initializer;   /* NULL if no = */
        } var_decl;

        /* NODE_BLOCK */
        struct { NodeList stmts; } block;

        /* NODE_IF: if <cond> then <body> [elseif...] [else <else_body>] end */
        struct {
            ASTNode *condition;
            ASTNode *then_branch;
            ASTNode *else_branch;    /* NULL or another NODE_IF (elseif) or NODE_BLOCK */
        } if_stmt;

        /* NODE_LOOP: loop <body> end */
        struct { ASTNode *body; } loop_stmt;

        /* NODE_FOR_RANGE: for <var> in <start>..<end> <body> end */
        struct {
            Token var;
            ASTNode *start;
            ASTNode *end;
            bool inclusive;         /* ..= vs .. */
            ASTNode *body;
        } for_range;

        /* NODE_FOR_IN: for <var> [, <var2>] in <iterable> <body> end */
        struct {
            Token var;
            Token var2;             /* second var for dicts (key,value) */
            bool has_var2;
            ASTNode *iterable;
            ASTNode *body;
        } for_in;

        /* NODE_RETURN: return [values...] */
        struct { NodeList values; } return_stmt;

        /* NODE_TRY_BLOCK: try <body> end */
        struct { ASTNode *body; } try_block;

        /* NODE_TRY_CATCH: try <body> catch <name> <catch_body> end */
        struct { ASTNode *body; Token err_name; ASTNode *catch_body; } try_catch;

        /* NODE_DEFER: defer <expr> */
        struct { ASTNode *call; } defer_stmt;

        /* NODE_FN_DECL */
        struct {
            Token name;
            Token method_struct;   /* if this is StructName.method */
            bool is_method;
            Param *params;
            int param_count;
            MgTypeRef *return_type;
            ASTNode *body;
        } fn_decl;

        /* NODE_STRUCT_DECL */
        struct {
            Token name;
            Param *fields;
            int field_count;
        } struct_decl;

        /* NODE_ENUM_DECL (stubbed) */
        struct {
            Token name;
            Token *variants;
            int variant_count;
        } enum_decl;

        /* NODE_IMPORT */
        struct {
            Token path;
            Token alias;
            bool has_alias;
        } import_stmt;

        /* NODE_EXPORT (stubbed) */
        struct { ASTNode *declaration; } export_stmt;

        /* NODE_DIRECTIVE */
        struct { MgDirectiveKind kind; Token name; } directive;

        /* NODE_TYPE_ALIAS */
        struct { Token name; MgTypeRef *type; } type_alias;

        /* NODE_EXTERN_DECL */
        struct {
            bool is_function;
            Token name;
            Param *params;
            int param_count;
            MgTypeRef *type;
        } extern_decl;

    } as;
};

/* AST allocation / helpers */
ASTNode *ast_alloc(NodeType type, int line);
void ast_free(ASTNode *node);
MgTypeRef *type_ref_alloc(MgTypeKind kind, int line);
void type_ref_free(MgTypeRef *type);
void node_list_init(NodeList *list);
void node_list_write(NodeList *list, ASTNode *node);
void node_list_free(NodeList *list);
void kv_list_init(KVList *list);
void kv_list_write(KVList *list, ASTNode *key, ASTNode *value);
void kv_list_free(KVList *list);

/* ========================================================================
 * Hash Table API (defined above)
 * ======================================================================== */

void table_init(Table *table);
void table_free(Table *table);
bool table_get(Table *table, ObjString *key, Value *value);
bool table_set(Table *table, ObjString *key, Value value);
bool table_delete(Table *table, ObjString *key);
ObjString *table_find_string(Table *table, const char *chars, int length, uint32_t hash);

/* ========================================================================
 * Virtual Machine
 * ======================================================================== */
/* Global inline cache for fast global lookups */
#define GLOBAL_IC_SIZE 128  /* must be power of 2 */
typedef struct {
    ObjString *name;
    Value value;
} GlobalICEntry;

#define FIELD_IC_SIZE 128   /* must be power of 2 */
typedef struct {
    ObjStruct *klass;
    ObjString *name;
    int index;
} FieldICEntry;

#define METHOD_IC_SIZE 128  /* must be power of 2 */
typedef struct {
    ObjStruct *klass;
    ObjString *name;
    Value method;
} MethodICEntry;

#define DICT_IC_SIZE 256    /* must be power of 2 */
typedef struct {
    ObjDict *dict;
    ObjString *key;
    uint32_t version;
    DictEntry *entry;
    int index;
} DictICEntry;

struct VM {
    /* Call stack */
    CallFrame frames[MAX_CALL_FRAMES];
    int frame_count;

    /* Register file / value stack */
    Value *stack;
    int stack_size;
    int stack_capacity;
    int stack_top;       /* highest stack index in active use (for GC scanning) */

    /* Globals */
    Table globals;
    GlobalICEntry global_ic[GLOBAL_IC_SIZE];  /* inline cache for global lookups */
    FieldICEntry field_ic[FIELD_IC_SIZE];     /* inline cache for struct fields */
    MethodICEntry method_ic[METHOD_IC_SIZE];  /* inline cache for struct methods */
    DictICEntry dict_ic[DICT_IC_SIZE];        /* inline cache for dict string keys */

    /* Module cache (path -> exports dict) */
    Table modules;

    /* String interning */
    Table strings;

    /* Open upvalues (linked list) */
    ObjUpvalue *open_upvalues;

    /* Currently executing coroutine, if vm_execute was entered by coroutine.resume. */
    ObjCoroutine *current_coroutine;
    bool yield_requested;
    Value yield_values[256];
    int yield_count;
    Value native_return_values[256];
    int native_return_count;
    SavedVMContext *saved_contexts;
    atomic_bool cancel_requested;
    uint64_t deadline_ms;
    uint32_t loop_cancel_counter;
    uint32_t strings_interned_since_minor;

    /* Upvalue cache: direct-mapped by stack slot offset for O(1) capture */
    ObjUpvalue **upvalue_cache;
    int upvalue_cache_capacity;

    /* Defer stack */
    struct {
        ObjClosure **items;
        int count;
        int capacity;
    } defer_stack;

    /* Cooperative task queue used by task.run(). Host scheduler hooks can
       replace this later without exposing OS-thread ownership here. */
    struct {
        ObjClosure **items;
        int count;
        int capacity;
    } task_queue;

    /* Isolated VMs started by vm.spawn(). Running native threads keep their
       handles rooted here so GC cannot free a task object still owned by a
       worker thread. Completed tasks are untracked by join/try_join. */
    struct {
        ObjVMTask **items;
        int count;
        int capacity;
    } vm_tasks;

    /* Free-lists for frequently allocated objects */
    ObjClosure *closure_free_list;
    ObjUpvalue *upvalue_free_list;
    int closure_free_list_count;
    int upvalue_free_list_count;

    /* Integer-to-string cache: maps small non-negative integers to interned ObjString* */
    ObjString **int_str_cache;

    /* Slab allocator: free lists and backing slabs for small objects */
    Obj  *slab_free_lists[SLAB_CLASS_COUNT];
    Slab *slab_blocks;

    /* GC state */
    Obj *young_objects;       /* young generation object list */
    Obj *old_objects;         /* old generation object list */
    size_t bytes_allocated;
    size_t young_bytes;       /* bytes in young generation only */
    size_t next_gc;

    /* Remembered set: old-gen objects that reference young-gen objects */
    Obj **remembered_set;
    int remembered_count;
    int remembered_capacity;

    /* GC gray stack for tri-color marking */
    Obj **gray_stack;
    int gray_count;
    int gray_capacity;

    char last_error_message[512];
    char last_error_trace[2048];
    char last_error_file[256];
    char last_error_function[128];
    int last_error_line;
    void *calling_native_userdata;  /* set before native dispatch, for FFI trampolines */
};

typedef enum {
    INTERPRET_OK,
    INTERPRET_COMPILE_ERROR,
    INTERPRET_RUNTIME_ERROR,
    INTERPRET_YIELD
} InterpretResult;

void vm_init(VM *vm);
void vm_free(VM *vm);
VM *vm_new(void);
void vm_delete(VM *vm);
void vm_register_native(VM *vm, const char *name, NativeFn function, int arity,
                        void *userdata, void (*userdata_finalizer)(void *));
ObjFFI *vm_register_ffi(VM *vm, const char *name, void *c_func, int arity);
ObjNativeHandle *vm_new_native_handle(VM *vm, const char *type_name, void *data,
                                      NativeHandleFinalizer finalizer);
bool vm_native_handle_set_method(VM *vm, ObjNativeHandle *handle, const char *name,
                                 NativeFn function, int arity,
                                 void *userdata, void (*userdata_finalizer)(void *));
void *vm_native_handle_data(Value value, const char *type_name);
Value vm_native_handle_value(ObjNativeHandle *handle);
bool vm_set_global_value(VM *vm, const char *name, Value value);
bool vm_get_global_value(VM *vm, const char *name, Value *out);
InterpretResult vm_interpret(VM *vm, const char *source);
InterpretResult vm_interpret_named(VM *vm, const char *source, const char *name);
InterpretResult vm_run_function(VM *vm, ObjFunction *function);
ObjFunction *vm_compile(VM *vm, const char *source);
ObjFunction *vm_compile_named(VM *vm, const char *source, const char *name);
bool vm_save_bytecode(VM *vm, ObjFunction *function, const char *path);
ObjFunction *vm_load_bytecode(VM *vm, const char *path);
void vm_push(VM *vm, Value value);
Value vm_pop(VM *vm);

/* Runtime error reporting */
void vm_runtime_error(VM *vm, const char *format, ...);
const char *vm_last_error(VM *vm);
const char *vm_last_error_trace(VM *vm);
int vm_last_error_line(VM *vm);
const char *vm_last_error_file(VM *vm);
const char *vm_last_error_function(VM *vm);
void vm_clear_error(VM *vm);

/* VM introspection (safe accessors that do not depend on struct layout) */
int vm_frame_count(VM *vm);
size_t vm_bytes_allocated(VM *vm);

/* String introspection: return chars/length of a string Value, or NULL/0
   if the value is not a string or is a rope that has not been flattened. */
const char *vm_string_chars(Value value);
int vm_string_length(Value value);
ObjFunction *compile_named(VM *vm, ASTNode *ast, const char *name, int name_len);

/* ========================================================================
 * Object Allocation (requires VM for GC tracking)
 * ======================================================================== */
Obj *allocate_object(VM *vm, size_t size, ObjType type);
ObjString *copy_string(VM *vm, const char *chars, int length);
ObjString *take_string(VM *vm, char *chars, int length);
ObjString *concat_strings(VM *vm, ObjString *a, ObjString *b);
const char *string_chars(VM *vm, ObjString *string);
char string_char_at(ObjString *string, int index);
ObjFunction *new_function(VM *vm);
ObjClosure *new_closure(VM *vm, ObjFunction *function);
ObjUpvalue *new_upvalue(VM *vm, Value *slot);
ObjNative *new_native(VM *vm, NativeFn function, const char *name, int arity);
ObjFFI *new_ffi(VM *vm, void *c_func, const char *name, int arity);
ObjNativeHandle *new_native_handle(VM *vm, ObjString *type_name, void *data,
                                   NativeHandleFinalizer finalizer);
ObjArray *new_array(VM *vm);
ObjDict *new_dict(VM *vm);
ObjStruct *new_struct(VM *vm, ObjString *name);
ObjInstance *new_instance(VM *vm, ObjStruct *klass);
ObjError *new_error(VM *vm, ObjString *kind, ObjString *message,
                    ObjString *file, int line, ObjString *function,
                    ObjString *hint);
ObjVMTask *new_vm_task(VM *vm, const char *path);
ObjCoroutine *new_coroutine(VM *vm, ObjClosure *closure);
void vm_task_destroy(ObjVMTask *task);

void array_push(VM *vm, ObjArray *array, Value value);
Value array_get(ObjArray *array, int index);
void array_set(ObjArray *array, int index, Value value);

bool dict_get(ObjDict *dict, ObjString *key, Value *value);
bool dict_set(VM *vm, ObjDict *dict, ObjString *key, Value value);
DictEntry *dict_find_entry(ObjDict *dict, ObjString *key, int *entry_index, int *indices_slot);
void dict_adjust_capacity(VM *vm, ObjDict *dict, int capacity);
bool dict_delete(ObjDict *dict, ObjString *key);

/* ========================================================================
 * Garbage Collector
 * ======================================================================== */
void gc_minor_collect(VM *vm);
void gc_major_collect(VM *vm);
void remembered_set_add(VM *vm, Obj *old_obj);
static inline void gc_write_barrier(VM *vm, Obj *old_obj, Value young_val) {
    if (!old_obj->is_old) return;
    if (!IS_OBJ(young_val)) return;
    Obj *val_obj = AS_OBJ(young_val);
    if (!val_obj->is_old) {
        remembered_set_add(vm, old_obj);
    }
}
void gc_mark_value(VM *vm, Value value);
void gc_mark_object(VM *vm, Obj *object);

/* ========================================================================
 * Lexer
 * ======================================================================== */
void scanner_init(Scanner *scanner, const char *source);
Token scan_token(Scanner *scanner);

/* ========================================================================
 * Parser (produces AST)
 * ======================================================================== */
ASTNode *parse(const char *source, bool *had_error);

/* ========================================================================
 * Optimizer (AST -> AST)
 * ======================================================================== */
void optimize_ast(ASTNode *node);

/* ========================================================================
 * Compiler (AST -> Bytecode)
 * ======================================================================== */
ObjFunction *compile(VM *vm, ASTNode *ast);

/* ========================================================================
 * Static Checker
 * ======================================================================== */
bool mg_ast_has_nocheck(ASTNode *ast);
bool mg_ast_has_strict(ASTNode *ast);
bool mg_typecheck_ast(ASTNode *ast, bool force_check);

/* ========================================================================
 * Debug (optional disassembly)
 * ======================================================================== */
void disassemble_chunk(Chunk *chunk, const char *name);
void disassemble_instruction(Chunk *chunk, int offset);

#endif /* MAGNESIUM_H */
