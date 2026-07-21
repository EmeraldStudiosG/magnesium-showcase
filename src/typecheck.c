#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magnesium.h"

typedef enum {
    TC_ANY,
    TC_UNKNOWN,
    TC_NULL,
    TC_BOOL,
    TC_NUMBER,
    TC_STRING,
    TC_ARRAY,
    TC_DICT,
    TC_FUNCTION,
    TC_SHAPE,
    TC_STRUCT
} TcKind;

typedef struct TcType TcType;

typedef struct {
    char *name;
    TcType *type;
} TcField;

struct TcType {
    TcKind kind;
    char *name;
    TcType *element;
    TcType *key;
    TcType *value;
    TcType **params;
    int param_count;
    bool variadic;
    TcType *return_type;
    TcField *fields;
    int field_count;
};

typedef struct {
    char *name;
    TcType *type;
    bool annotated;
    bool is_const;
    int depth;
} TcSymbol;

typedef struct {
    char *name;
    TcType *type;
} TcAlias;

typedef struct {
    TcType **types;
    int type_count;
    int type_capacity;
    TcSymbol *symbols;
    int symbol_count;
    int symbol_capacity;
    TcAlias *aliases;
    int alias_count;
    int alias_capacity;
    int depth;
    int errors;
    TcType *current_return;
} Checker;

static char *copy_token_text(Token token) {
    char *out = (char *)malloc((size_t)token.length + 1);
    if (!out) return NULL;
    memcpy(out, token.start, (size_t)token.length);
    out[token.length] = '\0';
    return out;
}

static char *copy_cstr(const char *text) {
    size_t len = strlen(text);
    char *out = (char *)malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, text, len + 1);
    return out;
}

static void checker_error(Checker *checker, int line, const char *format, ...) {
    checker->errors++;
    fprintf(stderr, "[line %d] Error: Type error: ", line);
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static TcType *tc_new(Checker *checker, TcKind kind) {
    TcType *type = (TcType *)calloc(1, sizeof(TcType));
    if (!type) {
        fprintf(stderr, "Out of memory allocating checker type.\n");
        exit(1);
    }
    type->kind = kind;
    if (checker->type_capacity < checker->type_count + 1) {
        checker->type_capacity = checker->type_capacity < 32 ? 32 : checker->type_capacity * 2;
        checker->types = (TcType **)realloc(checker->types,
                                            sizeof(TcType *) * checker->type_capacity);
    }
    checker->types[checker->type_count++] = type;
    return type;
}

static TcType *tc_named_primitive(Checker *checker, const char *name) {
    if (strcmp(name, "any") == 0) return tc_new(checker, TC_ANY);
    if (strcmp(name, "unknown") == 0) return tc_new(checker, TC_UNKNOWN);
    if (strcmp(name, "null") == 0) return tc_new(checker, TC_NULL);
    if (strcmp(name, "bool") == 0) return tc_new(checker, TC_BOOL);
    if (strcmp(name, "number") == 0) return tc_new(checker, TC_NUMBER);
    if (strcmp(name, "string") == 0) return tc_new(checker, TC_STRING);
    TcType *type = tc_new(checker, TC_STRUCT);
    type->name = copy_cstr(name);
    return type;
}

static TcType *tc_function(Checker *checker, TcType **params, int param_count,
                           TcType *return_type, bool variadic) {
    TcType *type = tc_new(checker, TC_FUNCTION);
    type->params = params;
    type->param_count = param_count;
    type->return_type = return_type;
    type->variadic = variadic;
    return type;
}

static TcType *tc_any(Checker *checker) { return tc_new(checker, TC_ANY); }
static TcType *tc_unknown(Checker *checker) { return tc_new(checker, TC_UNKNOWN); }
static TcType *tc_null(Checker *checker) { return tc_new(checker, TC_NULL); }
static TcType *tc_bool(Checker *checker) { return tc_new(checker, TC_BOOL); }
static TcType *tc_number(Checker *checker) { return tc_new(checker, TC_NUMBER); }
static TcType *tc_string(Checker *checker) { return tc_new(checker, TC_STRING); }

static const char *tc_type_name(TcType *type) {
    if (!type) return "unknown";
    switch (type->kind) {
        case TC_ANY: return "any";
        case TC_UNKNOWN: return "unknown";
        case TC_NULL: return "null";
        case TC_BOOL: return "bool";
        case TC_NUMBER: return "number";
        case TC_STRING: return "string";
        case TC_ARRAY: return "array";
        case TC_DICT: return "dict";
        case TC_FUNCTION: return "function";
        case TC_SHAPE: return "data shape";
        case TC_STRUCT: return type->name ? type->name : "struct";
    }
    return "unknown";
}

static void checker_free(Checker *checker) {
    for (int i = 0; i < checker->symbol_count; i++) free(checker->symbols[i].name);
    free(checker->symbols);
    for (int i = 0; i < checker->alias_count; i++) free(checker->aliases[i].name);
    free(checker->aliases);
    for (int i = 0; i < checker->type_count; i++) {
        TcType *type = checker->types[i];
        free(type->name);
        for (int f = 0; f < type->field_count; f++) free(type->fields[f].name);
        free(type->fields);
        free(type->params);
        free(type);
    }
    free(checker->types);
}

static TcAlias *find_alias(Checker *checker, const char *name) {
    for (int i = checker->alias_count - 1; i >= 0; i--) {
        if (strcmp(checker->aliases[i].name, name) == 0) return &checker->aliases[i];
    }
    return NULL;
}

static TcSymbol *find_symbol(Checker *checker, const char *name) {
    for (int i = checker->symbol_count - 1; i >= 0; i--) {
        if (strcmp(checker->symbols[i].name, name) == 0) return &checker->symbols[i];
    }
    return NULL;
}

static void add_alias(Checker *checker, const char *name, TcType *type) {
    TcAlias *alias = find_alias(checker, name);
    if (alias) {
        alias->type = type;
        return;
    }
    if (checker->alias_capacity < checker->alias_count + 1) {
        checker->alias_capacity = checker->alias_capacity < 16 ? 16 : checker->alias_capacity * 2;
        checker->aliases = (TcAlias *)realloc(checker->aliases,
                                              sizeof(TcAlias) * checker->alias_capacity);
    }
    checker->aliases[checker->alias_count].name = copy_cstr(name);
    checker->aliases[checker->alias_count].type = type;
    checker->alias_count++;
}

static TcSymbol *add_symbol(Checker *checker, const char *name, TcType *type,
                            bool annotated, bool is_const) {
    TcSymbol *existing = NULL;
    for (int i = checker->symbol_count - 1; i >= 0; i--) {
        if (checker->symbols[i].depth != checker->depth) break;
        if (strcmp(checker->symbols[i].name, name) == 0) {
            existing = &checker->symbols[i];
            break;
        }
    }
    if (existing) {
        existing->type = type;
        existing->annotated = annotated;
        existing->is_const = is_const;
        return existing;
    }
    if (checker->symbol_capacity < checker->symbol_count + 1) {
        checker->symbol_capacity = checker->symbol_capacity < 64 ? 64 : checker->symbol_capacity * 2;
        checker->symbols = (TcSymbol *)realloc(checker->symbols,
                                               sizeof(TcSymbol) * checker->symbol_capacity);
    }
    TcSymbol *sym = &checker->symbols[checker->symbol_count++];
    sym->name = copy_cstr(name);
    sym->type = type;
    sym->annotated = annotated;
    sym->is_const = is_const;
    sym->depth = checker->depth;
    return sym;
}

static void begin_scope(Checker *checker) {
    checker->depth++;
}

static void end_scope(Checker *checker) {
    while (checker->symbol_count > 0 &&
           checker->symbols[checker->symbol_count - 1].depth == checker->depth) {
        free(checker->symbols[checker->symbol_count - 1].name);
        checker->symbol_count--;
    }
    checker->depth--;
}

static TcType *convert_type_ref(Checker *checker, MgTypeRef *ref);

static TcType *resolve_named_type(Checker *checker, Token token) {
    char *name = copy_token_text(token);
    if (!name) return tc_unknown(checker);
    TcAlias *alias = find_alias(checker, name);
    if (alias) {
        free(name);
        return alias->type;
    }
    TcSymbol *sym = find_symbol(checker, name);
    if (sym && sym->type && sym->type->kind == TC_STRUCT) {
        free(name);
        return sym->type;
    }
    TcType *type = tc_named_primitive(checker, name);
    free(name);
    return type;
}

static TcType *convert_type_ref(Checker *checker, MgTypeRef *ref) {
    if (!ref) return tc_any(checker);
    switch (ref->kind) {
        case MG_TYPE_NAME:
            return resolve_named_type(checker, ref->as.name.name);
        case MG_TYPE_ARRAY: {
            TcType *type = tc_new(checker, TC_ARRAY);
            type->element = convert_type_ref(checker, ref->as.array.element);
            return type;
        }
        case MG_TYPE_DICT: {
            TcType *type = tc_new(checker, TC_DICT);
            type->key = convert_type_ref(checker, ref->as.dict.key);
            type->value = convert_type_ref(checker, ref->as.dict.value);
            return type;
        }
        case MG_TYPE_FUNCTION: {
            TcType **params = NULL;
            if (ref->as.function.param_count > 0) {
                params = (TcType **)calloc((size_t)ref->as.function.param_count, sizeof(TcType *));
                for (int i = 0; i < ref->as.function.param_count; i++) {
                    params[i] = convert_type_ref(checker, ref->as.function.params[i]);
                }
            }
            return tc_function(checker, params, ref->as.function.param_count,
                               convert_type_ref(checker, ref->as.function.return_type), false);
        }
        case MG_TYPE_SHAPE: {
            TcType *type = tc_new(checker, TC_SHAPE);
            type->field_count = ref->as.shape.field_count;
            if (type->field_count > 0) {
                type->fields = (TcField *)calloc((size_t)type->field_count, sizeof(TcField));
                for (int i = 0; i < type->field_count; i++) {
                    type->fields[i].name = copy_token_text(ref->as.shape.fields[i].name);
                    type->fields[i].type = convert_type_ref(checker, ref->as.shape.fields[i].type);
                }
            }
            return type;
        }
    }
    return tc_unknown(checker);
}

static bool type_is_numeric(TcType *type) {
    return type && (type->kind == TC_NUMBER || type->kind == TC_ANY);
}

static bool type_is_string(TcType *type) {
    return type && (type->kind == TC_STRING || type->kind == TC_ANY);
}

static bool types_compatible(TcType *expected, TcType *actual) {
    if (!expected || !actual) return true;
    if (expected->kind == TC_ANY || actual->kind == TC_ANY) return true;
    if (expected->kind == TC_UNKNOWN) return true;
    if (actual->kind == TC_UNKNOWN) return expected->kind == TC_UNKNOWN || expected->kind == TC_ANY;
    if (expected->kind == TC_SHAPE) {
        return actual->kind == TC_SHAPE || actual->kind == TC_DICT;
    }
    if (expected->kind != actual->kind) return false;
    switch (expected->kind) {
        case TC_ARRAY:
            return types_compatible(expected->element, actual->element);
        case TC_DICT:
            return types_compatible(expected->key, actual->key) &&
                   types_compatible(expected->value, actual->value);
        case TC_FUNCTION:
            if (!expected->variadic && expected->param_count != actual->param_count) return false;
            for (int i = 0; i < expected->param_count && i < actual->param_count; i++) {
                if (!types_compatible(expected->params[i], actual->params[i])) return false;
            }
            return types_compatible(expected->return_type, actual->return_type);
        case TC_STRUCT:
            if (!expected->name || !actual->name) return true;
            return strcmp(expected->name, actual->name) == 0;
        case TC_SHAPE:
            return true;
        default:
            return true;
    }
}

static TcField *find_field(TcType *type, const char *name) {
    if (!type) return NULL;
    for (int i = 0; i < type->field_count; i++) {
        if (strcmp(type->fields[i].name, name) == 0) return &type->fields[i];
    }
    return NULL;
}

static void append_field_type(TcType *type, const char *name, TcType *field_type) {
    if (!type || !name) return;
    TcField *existing = find_field(type, name);
    if (existing) {
        existing->type = field_type;
        return;
    }
    type->fields = (TcField *)realloc(type->fields, sizeof(TcField) * (size_t)(type->field_count + 1));
    type->fields[type->field_count].name = copy_cstr(name);
    type->fields[type->field_count].type = field_type;
    type->field_count++;
}

static ASTNode *dict_literal_find_key(ASTNode *dict, const char *name) {
    if (!dict || dict->type != NODE_DICT_LITERAL) return NULL;
    for (int i = 0; i < dict->as.dict_literal.entries.count; i++) {
        ASTNode *key = dict->as.dict_literal.entries.pairs[i].key;
        if (key->type == NODE_STRING &&
            key->as.string.length == (int)strlen(name) &&
            memcmp(key->as.string.value, name, (size_t)key->as.string.length) == 0) {
            return dict->as.dict_literal.entries.pairs[i].value;
        }
    }
    return NULL;
}

static TcType *check_expr(Checker *checker, ASTNode *node);
static void check_stmt(Checker *checker, ASTNode *node);
static TcType *convert_param_type(Checker *checker, Param *param);

static void check_assignable(Checker *checker, int line, TcType *expected, TcType *actual,
                             const char *context) {
    if (!types_compatible(expected, actual)) {
        checker_error(checker, line, "Expected %s for %s, got %s.",
                      tc_type_name(expected), context, tc_type_name(actual));
    }
}

static void check_dict_literal_against_shape(Checker *checker, ASTNode *literal, TcType *shape) {
    if (!literal || literal->type != NODE_DICT_LITERAL || !shape || shape->kind != TC_SHAPE) return;
    for (int i = 0; i < shape->field_count; i++) {
        ASTNode *value = dict_literal_find_key(literal, shape->fields[i].name);
        if (!value) {
            checker_error(checker, literal->line,
                          "Missing required field '%s' for data shape.", shape->fields[i].name);
            continue;
        }
        TcType *actual = check_expr(checker, value);
        check_assignable(checker, value->line, shape->fields[i].type, actual, shape->fields[i].name);
    }
}

static void check_literal_against_expected(Checker *checker, ASTNode *literal, TcType *expected) {
    if (!literal || !expected) return;
    if (expected->kind == TC_SHAPE) {
        check_dict_literal_against_shape(checker, literal, expected);
        return;
    }
    if (expected->kind == TC_ARRAY && literal->type == NODE_ARRAY_LITERAL) {
        for (int i = 0; i < literal->as.array_literal.items.count; i++) {
            TcType *actual = check_expr(checker, literal->as.array_literal.items.nodes[i]);
            check_assignable(checker, literal->as.array_literal.items.nodes[i]->line,
                             expected->element, actual, "array element");
        }
        return;
    }
    if (expected->kind == TC_DICT && literal->type == NODE_DICT_LITERAL) {
        for (int i = 0; i < literal->as.dict_literal.entries.count; i++) {
            ASTNode *value = literal->as.dict_literal.entries.pairs[i].value;
            TcType *actual = check_expr(checker, value);
            check_assignable(checker, value->line, expected->value, actual, "dict value");
        }
    }
}

static TcType *join_element_type(Checker *checker, TcType *a, TcType *b) {
    if (!a) return b;
    if (!b) return a;
    if (types_compatible(a, b) && types_compatible(b, a)) return a;
    return tc_any(checker);
}

static TcType *check_expr(Checker *checker, ASTNode *node) {
    if (!node) return tc_null(checker);
    switch (node->type) {
        case NODE_NUMBER:
            return tc_number(checker);
        case NODE_STRING:
        case NODE_INTERP_STRING:
            return tc_string(checker);
        case NODE_BOOL:
            return tc_bool(checker);
        case NODE_NULL:
            return tc_null(checker);
        case NODE_IDENTIFIER: {
            char *name = copy_token_text(node->as.identifier.name);
            TcSymbol *sym = name ? find_symbol(checker, name) : NULL;
            if (!sym) {
                checker_error(checker, node->line, "Unknown symbol '%s'.", name ? name : "");
                free(name);
                return tc_unknown(checker);
            }
            free(name);
            return sym->type;
        }
        case NODE_ARRAY_LITERAL: {
            TcType *type = tc_new(checker, TC_ARRAY);
            TcType *element = NULL;
            for (int i = 0; i < node->as.array_literal.items.count; i++) {
                element = join_element_type(checker, element,
                                            check_expr(checker, node->as.array_literal.items.nodes[i]));
            }
            type->element = element ? element : tc_unknown(checker);
            return type;
        }
        case NODE_DICT_LITERAL: {
            TcType *type = tc_new(checker, TC_DICT);
            type->key = tc_string(checker);
            TcType *value = NULL;
            for (int i = 0; i < node->as.dict_literal.entries.count; i++) {
                value = join_element_type(checker, value,
                                          check_expr(checker, node->as.dict_literal.entries.pairs[i].value));
            }
            type->value = value ? value : tc_unknown(checker);
            return type;
        }
        case NODE_STRUCT_LITERAL: {
            char *name = copy_token_text(node->as.struct_literal.name);
            TcSymbol *sym = name ? find_symbol(checker, name) : NULL;
            if (sym && sym->type && sym->type->kind == TC_FUNCTION && sym->type->return_type) {
                TcType *out = sym->type->return_type;
                for (int i = 0; out->kind == TC_STRUCT && i < node->as.struct_literal.fields.count; i++) {
                    ASTNode *key = node->as.struct_literal.fields.pairs[i].key;
                    if (key->type != NODE_STRING) continue;
                    TcField *field = find_field(out, key->as.string.value);
                    if (field) {
                        TcType *actual = check_expr(checker, node->as.struct_literal.fields.pairs[i].value);
                        check_assignable(checker, node->as.struct_literal.fields.pairs[i].value->line,
                                         field->type, actual, field->name);
                    }
                }
                free(name);
                return out;
            }
            free(name);
            return tc_unknown(checker);
        }
        case NODE_UNARY: {
            TcType *operand = check_expr(checker, node->as.unary.operand);
            if (node->as.unary.op == TOKEN_MINUS && !type_is_numeric(operand)) {
                checker_error(checker, node->line, "Unary '-' requires number, got %s.",
                              tc_type_name(operand));
            }
            return node->as.unary.op == TOKEN_BANG ? tc_bool(checker) : tc_number(checker);
        }
        case NODE_BINARY: {
            TcType *left = check_expr(checker, node->as.binary.left);
            TcType *right = check_expr(checker, node->as.binary.right);
            switch (node->as.binary.op) {
                case TOKEN_PLUS:
                    if (type_is_numeric(left) && type_is_numeric(right)) return tc_number(checker);
                    if (type_is_string(left) && type_is_string(right)) return tc_string(checker);
                    if (left->kind != TC_ANY && right->kind != TC_ANY) {
                        checker_error(checker, node->line,
                                      "Operator '+' requires two numbers or two strings, got %s and %s.",
                                      tc_type_name(left), tc_type_name(right));
                    }
                    return tc_any(checker);
                case TOKEN_MINUS:
                case TOKEN_STAR:
                case TOKEN_SLASH:
                case TOKEN_PERCENT:
                    if (!type_is_numeric(left) || !type_is_numeric(right)) {
                        checker_error(checker, node->line,
                                      "Arithmetic requires numbers, got %s and %s.",
                                      tc_type_name(left), tc_type_name(right));
                    }
                    return tc_number(checker);
                case TOKEN_LESS:
                case TOKEN_LESS_EQUAL:
                case TOKEN_GREATER:
                case TOKEN_GREATER_EQUAL:
                    if (!type_is_numeric(left) || !type_is_numeric(right)) {
                        checker_error(checker, node->line,
                                      "Numeric comparison requires numbers, got %s and %s.",
                                      tc_type_name(left), tc_type_name(right));
                    }
                    return tc_bool(checker);
                case TOKEN_EQUAL_EQUAL:
                case TOKEN_BANG_EQUAL:
                    return tc_bool(checker);
                case TOKEN_DOT_DOT:
                case TOKEN_DOT_DOT_EQUAL:
                    if (!type_is_numeric(left) || !type_is_numeric(right)) {
                        checker_error(checker, node->line,
                                      "Range bounds must be numbers, got %s and %s.",
                                      tc_type_name(left), tc_type_name(right));
                    }
                    return tc_any(checker);
                default:
                    return tc_unknown(checker);
            }
        }
        case NODE_LOGICAL:
            check_expr(checker, node->as.logical.left);
            check_expr(checker, node->as.logical.right);
            return tc_bool(checker);
        case NODE_CALL: {
            TcType *callee = check_expr(checker, node->as.call.callee);
            if (callee->kind == TC_ANY) {
                for (int i = 0; i < node->as.call.args.count; i++) check_expr(checker, node->as.call.args.nodes[i]);
                return tc_any(checker);
            }
            if (callee->kind != TC_FUNCTION) {
                checker_error(checker, node->line, "Cannot call %s value.", tc_type_name(callee));
                for (int i = 0; i < node->as.call.args.count; i++) check_expr(checker, node->as.call.args.nodes[i]);
                return tc_unknown(checker);
            }
            if (!callee->variadic && node->as.call.args.count != callee->param_count) {
                checker_error(checker, node->line, "Wrong call arity: expected %d argument(s), got %d.",
                              callee->param_count, node->as.call.args.count);
            } else if (callee->variadic && node->as.call.args.count < callee->param_count) {
                checker_error(checker, node->line, "Wrong call arity: expected at least %d argument(s), got %d.",
                              callee->param_count, node->as.call.args.count);
            }
            int limit = node->as.call.args.count < callee->param_count
                ? node->as.call.args.count
                : callee->param_count;
            for (int i = 0; i < node->as.call.args.count; i++) {
                TcType *actual = check_expr(checker, node->as.call.args.nodes[i]);
                if (i < limit) {
                    check_assignable(checker, node->as.call.args.nodes[i]->line,
                                     callee->params[i], actual, "argument");
                }
            }
            return callee->return_type ? callee->return_type : tc_any(checker);
        }
        case NODE_INDEX: {
            TcType *object = check_expr(checker, node->as.index_expr.object);
            TcType *index = check_expr(checker, node->as.index_expr.index);
            if (object->kind == TC_ANY) return tc_any(checker);
            if (object->kind == TC_ARRAY) {
                if (!type_is_numeric(index)) {
                    checker_error(checker, node->line, "Array index must be number, got %s.",
                                  tc_type_name(index));
                }
                return object->element ? object->element : tc_unknown(checker);
            }
            if (object->kind == TC_DICT) {
                check_assignable(checker, node->line, object->key, index, "dict key");
                return object->value ? object->value : tc_unknown(checker);
            }
            if (object->kind == TC_SHAPE) {
                if (!type_is_string(index)) {
                    checker_error(checker, node->line, "Data shape key must be string, got %s.",
                                  tc_type_name(index));
                }
                if (node->as.index_expr.index->type == NODE_STRING) {
                    TcField *field = find_field(object, node->as.index_expr.index->as.string.value);
                    if (field) return field->type;
                }
                return tc_unknown(checker);
            }
            if (object->kind == TC_STRING) {
                if (!type_is_numeric(index)) {
                    checker_error(checker, node->line, "String index must be number, got %s.",
                                  tc_type_name(index));
                }
                return tc_string(checker);
            }
            checker_error(checker, node->line, "Cannot index %s value.", tc_type_name(object));
            return tc_unknown(checker);
        }
        case NODE_FIELD_GET: {
            TcType *object = check_expr(checker, node->as.field_get.object);
            if (object->kind == TC_ANY) return tc_any(checker);
            if (object->kind == TC_SHAPE || object->kind == TC_STRUCT) {
                char *field_name = copy_token_text(node->as.field_get.name);
                TcField *field = field_name ? find_field(object, field_name) : NULL;
                free(field_name);
                if (field) return field->type;
                return object->kind == TC_SHAPE ? tc_unknown(checker) : tc_unknown(checker);
            }
            if (object->kind == TC_DICT) return object->value ? object->value : tc_unknown(checker);
            checker_error(checker, node->line, "Cannot access field on %s value.", tc_type_name(object));
            return tc_unknown(checker);
        }
        case NODE_TRY:
            return check_expr(checker, node->as.try_expr.expr);
        case NODE_TRY_BLOCK:
            check_stmt(checker, node);
            return tc_any(checker);
        default:
            return tc_unknown(checker);
    }
}

static void check_assignment_target(Checker *checker, ASTNode *target, ASTNode *value) {
    TcType *actual = check_expr(checker, value);
    if (target->type == NODE_IDENTIFIER) {
        char *name = copy_token_text(target->as.identifier.name);
        TcSymbol *sym = name ? find_symbol(checker, name) : NULL;
        if (!sym) {
            checker_error(checker, target->line, "Unknown symbol '%s'.", name ? name : "");
            free(name);
            return;
        }
        if (sym->is_const) {
            checker_error(checker, target->line, "Cannot assign to const '%s'.", name);
        }
        if (sym->annotated) {
            check_assignable(checker, value->line, sym->type, actual, name);
        } else {
            sym->type = actual;
        }
        free(name);
        return;
    }
    if (target->type == NODE_INDEX) {
        TcType *object = check_expr(checker, target->as.index_expr.object);
        TcType *index = check_expr(checker, target->as.index_expr.index);
        if (object->kind == TC_ARRAY) {
            if (!type_is_numeric(index)) {
                checker_error(checker, target->line, "Array index must be number, got %s.",
                              tc_type_name(index));
            }
            check_assignable(checker, value->line, object->element, actual, "array element");
        } else if (object->kind == TC_DICT) {
            check_assignable(checker, target->line, object->key, index, "dict key");
            check_assignable(checker, value->line, object->value, actual, "dict value");
        } else if (object->kind != TC_ANY) {
            checker_error(checker, target->line, "Cannot assign through index on %s value.",
                          tc_type_name(object));
        }
        return;
    }
    if (target->type == NODE_FIELD_GET) {
        TcType *object = check_expr(checker, target->as.field_get.object);
        if (object->kind == TC_STRUCT || object->kind == TC_SHAPE) {
            char *field_name = copy_token_text(target->as.field_get.name);
            TcField *field = field_name ? find_field(object, field_name) : NULL;
            if (field) {
                check_assignable(checker, value->line, field->type, actual, field->name);
            } else if (object->kind == TC_STRUCT) {
                checker_error(checker, target->line, "Unknown field '%s' on %s.",
                              field_name ? field_name : "", tc_type_name(object));
            }
            free(field_name);
        } else if (object->kind != TC_ANY && object->kind != TC_DICT) {
            checker_error(checker, target->line, "Cannot assign field on %s value.",
                          tc_type_name(object));
        }
    }
}

static void check_block(Checker *checker, ASTNode *node) {
    if (!node || node->type != NODE_BLOCK) return;
    for (int i = 0; i < node->as.block.stmts.count; i++) {
        check_stmt(checker, node->as.block.stmts.nodes[i]);
    }
}

static void check_function_body(Checker *checker, ASTNode *node) {
    char *name = copy_token_text(node->as.fn_decl.name);
    TcSymbol *fn_sym = name ? find_symbol(checker, name) : NULL;
    TcType *fn_type = fn_sym ? fn_sym->type : NULL;
    free(name);

    begin_scope(checker);
    for (int i = 0; i < node->as.fn_decl.param_count; i++) {
        char *param_name = copy_token_text(node->as.fn_decl.params[i].name);
        TcType *param_type = fn_type && fn_type->kind == TC_FUNCTION && i < fn_type->param_count
            ? fn_type->params[i]
            : convert_param_type(checker, &node->as.fn_decl.params[i]);
        add_symbol(checker, param_name ? param_name : "", param_type,
                   node->as.fn_decl.params[i].type != NULL, false);
        free(param_name);
    }
    TcType *previous_return = checker->current_return;
    checker->current_return = fn_type && fn_type->kind == TC_FUNCTION
        ? fn_type->return_type
        : (node->as.fn_decl.return_type ? convert_type_ref(checker, node->as.fn_decl.return_type) : NULL);
    check_block(checker, node->as.fn_decl.body);
    checker->current_return = previous_return;
    end_scope(checker);
}

static void check_stmt(Checker *checker, ASTNode *node) {
    if (!node) return;
    switch (node->type) {
        case NODE_BLOCK:
            begin_scope(checker);
            check_block(checker, node);
            end_scope(checker);
            break;
        case NODE_DIRECTIVE:
        case NODE_TYPE_ALIAS:
        case NODE_EXTERN_DECL:
        case NODE_STRUCT_DECL:
        case NODE_ENUM_DECL:
            break;
        case NODE_IMPORT: {
            Token alias = node->as.import_stmt.has_alias ? node->as.import_stmt.alias : node->as.import_stmt.path;
            char *name = NULL;
            if (node->as.import_stmt.has_alias) {
                name = copy_token_text(alias);
            } else {
                int len = alias.length - 2;
                const char *start = alias.start + 1;
                const char *slash = NULL;
                for (int i = 0; i < len; i++) if (start[i] == '/') slash = start + i + 1;
                const char *base = slash ? slash : start;
                const char *dot = NULL;
                for (const char *p = base; p < start + len; p++) if (*p == '.') dot = p;
                int out_len = (int)((dot ? dot : start + len) - base);
                name = (char *)malloc((size_t)out_len + 1);
                memcpy(name, base, (size_t)out_len);
                name[out_len] = '\0';
            }
            add_symbol(checker, name ? name : "", tc_any(checker), false, false);
            free(name);
            break;
        }
        case NODE_LET:
        case NODE_CONST: {
            TcType *actual = node->as.var_decl.initializer
                ? check_expr(checker, node->as.var_decl.initializer)
                : tc_null(checker);
            TcType *decl_type = node->as.var_decl.type_annotation
                ? convert_type_ref(checker, node->as.var_decl.type_annotation)
                : actual;
            if (node->as.var_decl.type_annotation) {
                check_literal_against_expected(checker, node->as.var_decl.initializer, decl_type);
                bool checked_container_literal =
                    (decl_type->kind == TC_SHAPE && node->as.var_decl.initializer &&
                     node->as.var_decl.initializer->type == NODE_DICT_LITERAL) ||
                    (decl_type->kind == TC_DICT && node->as.var_decl.initializer &&
                     node->as.var_decl.initializer->type == NODE_DICT_LITERAL) ||
                    (decl_type->kind == TC_ARRAY && node->as.var_decl.initializer &&
                     node->as.var_decl.initializer->type == NODE_ARRAY_LITERAL);
                if (!checked_container_literal) {
                    check_assignable(checker, node->line, decl_type, actual, "initializer");
                }
            }
            char *name = copy_token_text(node->as.var_decl.name);
            add_symbol(checker, name ? name : "", decl_type,
                       node->as.var_decl.type_annotation != NULL, node->type == NODE_CONST);
            free(name);
            for (int i = 1; i < node->as.var_decl.name_count; i++) {
                char *extra = copy_token_text(node->as.var_decl.extra_names[i - 1]);
                MgTypeRef *extra_ref = node->as.var_decl.extra_type_annotations
                    ? node->as.var_decl.extra_type_annotations[i - 1]
                    : NULL;
                TcType *extra_type = extra_ref ? convert_type_ref(checker, extra_ref) : tc_any(checker);
                add_symbol(checker, extra ? extra : "", extra_type, extra_ref != NULL, node->type == NODE_CONST);
                free(extra);
            }
            break;
        }
        case NODE_ASSIGN:
            check_assignment_target(checker, node->as.assign.target, node->as.assign.value);
            break;
        case NODE_FIELD_SET: {
            ASTNode fake;
            memset(&fake, 0, sizeof(fake));
            fake.type = NODE_FIELD_GET;
            fake.line = node->line;
            fake.as.field_get.object = node->as.field_set.object;
            fake.as.field_get.name = node->as.field_set.name;
            check_assignment_target(checker, &fake, node->as.field_set.value);
            break;
        }
        case NODE_EXPRESSION_STMT:
            check_expr(checker, node->as.expr_stmt.expr);
            break;
        case NODE_IF:
            check_expr(checker, node->as.if_stmt.condition);
            check_stmt(checker, node->as.if_stmt.then_branch);
            check_stmt(checker, node->as.if_stmt.else_branch);
            break;
        case NODE_LOOP:
            check_stmt(checker, node->as.loop_stmt.body);
            break;
        case NODE_FOR_RANGE:
            check_expr(checker, node->as.for_range.start);
            check_expr(checker, node->as.for_range.end);
            begin_scope(checker);
            {
                char *name = copy_token_text(node->as.for_range.var);
                add_symbol(checker, name ? name : "", tc_number(checker), false, false);
                free(name);
            }
            check_stmt(checker, node->as.for_range.body);
            end_scope(checker);
            break;
        case NODE_FOR_IN:
            check_expr(checker, node->as.for_in.iterable);
            begin_scope(checker);
            {
                char *name = copy_token_text(node->as.for_in.var);
                add_symbol(checker, name ? name : "", tc_any(checker), false, false);
                free(name);
                if (node->as.for_in.has_var2) {
                    name = copy_token_text(node->as.for_in.var2);
                    add_symbol(checker, name ? name : "", tc_any(checker), false, false);
                    free(name);
                }
            }
            check_stmt(checker, node->as.for_in.body);
            end_scope(checker);
            break;
        case NODE_RETURN:
            if (node->as.return_stmt.values.count == 0) {
                if (checker->current_return) {
                    check_assignable(checker, node->line, checker->current_return, tc_null(checker), "return");
                }
            } else {
                TcType *actual = check_expr(checker, node->as.return_stmt.values.nodes[0]);
                if (checker->current_return) {
                    check_assignable(checker, node->line, checker->current_return, actual, "return");
                }
                for (int i = 1; i < node->as.return_stmt.values.count; i++) {
                    check_expr(checker, node->as.return_stmt.values.nodes[i]);
                }
            }
            break;
        case NODE_TRY_CATCH:
            check_stmt(checker, node->as.try_catch.body);
            begin_scope(checker);
            {
                char *err_name = copy_token_text(node->as.try_catch.err_name);
                add_symbol(checker, err_name ? err_name : "", tc_any(checker), false, false);
                free(err_name);
            }
            check_stmt(checker, node->as.try_catch.catch_body);
            end_scope(checker);
            break;
        case NODE_TRY_BLOCK:
            check_stmt(checker, node->as.try_block.body);
            break;
        case NODE_DEFER:
            check_expr(checker, node->as.defer_stmt.call);
            break;
        case NODE_FN_DECL:
            check_function_body(checker, node);
            break;
        case NODE_BREAK:
        case NODE_CONTINUE:
            break;
        default:
            check_expr(checker, node);
            break;
    }
}

static void register_builtin_fn(Checker *checker, const char *name, TcType *return_type,
                                int arity, bool variadic) {
    TcType **params = NULL;
    if (arity > 0) {
        params = (TcType **)calloc((size_t)arity, sizeof(TcType *));
        for (int i = 0; i < arity; i++) params[i] = tc_any(checker);
    }
    add_symbol(checker, name, tc_function(checker, params, arity, return_type, variadic), true, true);
}

static void register_builtins_for_checker(Checker *checker) {
    register_builtin_fn(checker, "print", tc_null(checker), 0, true);
    register_builtin_fn(checker, "len", tc_number(checker), 1, false);
    register_builtin_fn(checker, "push", tc_null(checker), 2, false);
    register_builtin_fn(checker, "type", tc_string(checker), 1, false);
    register_builtin_fn(checker, "tostring", tc_string(checker), 1, false);
    register_builtin_fn(checker, "tonumber", tc_any(checker), 1, false);
    register_builtin_fn(checker, "assert", tc_null(checker), 1, true);
    register_builtin_fn(checker, "error", tc_any(checker), 1, true);
    register_builtin_fn(checker, "input", tc_string(checker), 0, true);
    register_builtin_fn(checker, "Err", tc_any(checker), 2, false);
    register_builtin_fn(checker, "Error", tc_any(checker), 2, false);
    register_builtin_fn(checker, "__ffi_bind", tc_any(checker), 2, false);

    const char *modules[] = {
        "math", "string", "array", "dict", "io", "fs", "path", "os",
        "process", "task", "vm", "coroutine"
    };
    for (size_t i = 0; i < sizeof(modules) / sizeof(modules[0]); i++) {
        add_symbol(checker, modules[i], tc_any(checker), true, true);
    }
}

static TcType *convert_param_type(Checker *checker, Param *param) {
    return param->type ? convert_type_ref(checker, param->type) : tc_any(checker);
}

static void collect_declaration(Checker *checker, ASTNode *node) {
    if (!node) return;
    if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->as.block.stmts.count; i++) {
            collect_declaration(checker, node->as.block.stmts.nodes[i]);
        }
        return;
    }
    if (node->type == NODE_TYPE_ALIAS) {
        char *name = copy_token_text(node->as.type_alias.name);
        add_alias(checker, name ? name : "", convert_type_ref(checker, node->as.type_alias.type));
        free(name);
        return;
    }
    if (node->type == NODE_STRUCT_DECL) {
        char *name = copy_token_text(node->as.struct_decl.name);
        TcType *struct_type = tc_new(checker, TC_STRUCT);
        struct_type->name = name ? copy_cstr(name) : NULL;
        struct_type->field_count = node->as.struct_decl.field_count;
        if (struct_type->field_count > 0) {
            struct_type->fields = (TcField *)calloc((size_t)struct_type->field_count, sizeof(TcField));
            for (int i = 0; i < node->as.struct_decl.field_count; i++) {
                struct_type->fields[i].name = copy_token_text(node->as.struct_decl.fields[i].name);
                struct_type->fields[i].type = convert_param_type(checker, &node->as.struct_decl.fields[i]);
            }
        }
        TcType **params = NULL;
        if (node->as.struct_decl.field_count > 0) {
            params = (TcType **)calloc((size_t)node->as.struct_decl.field_count, sizeof(TcType *));
            for (int i = 0; i < node->as.struct_decl.field_count; i++) {
                params[i] = struct_type->fields[i].type;
            }
        }
        add_symbol(checker, name ? name : "",
                   tc_function(checker, params, node->as.struct_decl.field_count, struct_type, false),
                   true, true);
        add_alias(checker, name ? name : "", struct_type);
        free(name);
        return;
    }
    if (node->type == NODE_FN_DECL) {
        char *name = copy_token_text(node->as.fn_decl.name);
        int exposed_param_start = node->as.fn_decl.is_method && node->as.fn_decl.param_count > 0 ? 1 : 0;
        int exposed_param_count = node->as.fn_decl.param_count - exposed_param_start;
        if (exposed_param_count < 0) exposed_param_count = 0;
        TcType **params = NULL;
        if (exposed_param_count > 0) {
            params = (TcType **)calloc((size_t)exposed_param_count, sizeof(TcType *));
            for (int i = 0; i < exposed_param_count; i++) {
                params[i] = convert_param_type(checker, &node->as.fn_decl.params[i + exposed_param_start]);
            }
        }
        TcType *return_type = node->as.fn_decl.return_type
            ? convert_type_ref(checker, node->as.fn_decl.return_type)
            : tc_any(checker);
        TcType *fn_type = tc_function(checker, params, exposed_param_count, return_type, false);
        if (node->as.fn_decl.is_method) {
            char *struct_name = copy_token_text(node->as.fn_decl.method_struct);
            TcAlias *alias = struct_name ? find_alias(checker, struct_name) : NULL;
            if (alias && alias->type && alias->type->kind == TC_STRUCT) {
                append_field_type(alias->type, name ? name : "", fn_type);
            }
            free(struct_name);
        } else if (name && name[0] != '\0') {
            add_symbol(checker, name, fn_type, node->as.fn_decl.return_type != NULL, true);
        }
        free(name);
        return;
    }
    if (node->type == NODE_EXTERN_DECL) {
        char *name = copy_token_text(node->as.extern_decl.name);
        if (node->as.extern_decl.is_function) {
            TcType **params = NULL;
            if (node->as.extern_decl.param_count > 0) {
                params = (TcType **)calloc((size_t)node->as.extern_decl.param_count, sizeof(TcType *));
                for (int i = 0; i < node->as.extern_decl.param_count; i++) {
                    params[i] = convert_param_type(checker, &node->as.extern_decl.params[i]);
                }
            }
            add_symbol(checker, name ? name : "",
                       tc_function(checker, params, node->as.extern_decl.param_count,
                                   convert_type_ref(checker, node->as.extern_decl.type), false),
                       true, true);
        } else {
            add_symbol(checker, name ? name : "", convert_type_ref(checker, node->as.extern_decl.type),
                       true, true);
        }
        free(name);
    }
}

static bool scan_directive(ASTNode *node, MgDirectiveKind kind) {
    if (!node) return false;
    if (node->type == NODE_DIRECTIVE && node->as.directive.kind == kind) return true;
    if (node->type == NODE_BLOCK) {
        for (int i = 0; i < node->as.block.stmts.count; i++) {
            if (scan_directive(node->as.block.stmts.nodes[i], kind)) return true;
        }
    }
    return false;
}

bool mg_ast_has_nocheck(ASTNode *ast) {
    return scan_directive(ast, MG_DIRECTIVE_NOCHECK);
}

bool mg_ast_has_strict(ASTNode *ast) {
    return scan_directive(ast, MG_DIRECTIVE_STRICT);
}

bool mg_typecheck_ast(ASTNode *ast, bool force_check) {
    if (!ast || mg_ast_has_nocheck(ast)) return true;
    if (!force_check && !mg_ast_has_strict(ast)) return true;

    Checker checker;
    memset(&checker, 0, sizeof(checker));
    register_builtins_for_checker(&checker);
    collect_declaration(&checker, ast);
    check_block(&checker, ast);
    bool ok = checker.errors == 0;
    checker_free(&checker);
    return ok;
}
