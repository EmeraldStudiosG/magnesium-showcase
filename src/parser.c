/* ========================================================================
 * Parser
 * Recursive descent parser producing an Abstract Syntax Tree.
 *
 * Grammar (simplified):
 *   program     -> declaration* EOF
 *   declaration -> let_decl | const_decl | fn_decl | struct_decl | enum_decl | statement
 *   statement   -> if_stmt | loop_stmt | for_stmt | return_stmt | defer_stmt
 *                | break_stmt | continue_stmt | expr_stmt
 *   expression  -> assignment
 *   assignment  -> or_expr ( '=' assignment )?
 *   or_expr     -> and_expr ( 'or' and_expr )*
 *   and_expr    -> equality ( 'and' equality )*
 *   equality    -> comparison ( ('==' | '!=') comparison )*
 *   comparison  -> term ( ('<' | '>' | '<=' | '>=') term )*
 *   term        -> factor ( ('+' | '-') factor )*
 *   factor      -> unary ( ('*' | '/' | '%') unary )*
 *   unary       -> ('!' | '-') unary | call
 *   call        -> primary ( '(' args ')' | '[' expr ']' | '.' ident )*
 *   primary     -> NUMBER | STRING | INTERP_STRING | 'true' | 'false' | 'null'
 *                | IDENTIFIER | '(' expression ')' | '[' array_items ']'
 *                | '{' dict_entries '}'
 * ======================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "magnesium.h"

/* ========================================================================
 * Parser State
 * ======================================================================== */
typedef struct {
    Scanner scanner;
    Token current;
    Token previous;
    bool had_error;
    bool panic_mode;
} ParserState;

static MG_THREAD_LOCAL ParserState ps;

/* ========================================================================
 * AST Allocation Helpers
 * ======================================================================== */
ASTNode *ast_alloc(NodeType type, int line) {
    ASTNode *node = (ASTNode *)calloc(1, sizeof(ASTNode));
    if (!node) {
        fprintf(stderr, "Out of memory allocating AST node.\n");
        exit(1);
    }
    node->type = type;
    node->line = line;
    return node;
}

void node_list_init(NodeList *list) {
    list->nodes = NULL;
    list->count = 0;
    list->capacity = 0;
}

void node_list_write(NodeList *list, ASTNode *node) {
    if (list->capacity < list->count + 1) {
        list->capacity = list->capacity < 8 ? 8 : list->capacity * 2;
        list->nodes = (ASTNode **)realloc(list->nodes, sizeof(ASTNode *) * list->capacity);
    }
    list->nodes[list->count++] = node;
}

void node_list_free(NodeList *list) {
    free(list->nodes);
    node_list_init(list);
}

void kv_list_init(KVList *list) {
    list->pairs = NULL;
    list->count = 0;
    list->capacity = 0;
}

void kv_list_write(KVList *list, ASTNode *key, ASTNode *value) {
    if (list->capacity < list->count + 1) {
        list->capacity = list->capacity < 8 ? 8 : list->capacity * 2;
        list->pairs = (KVPair *)realloc(list->pairs, sizeof(KVPair) * list->capacity);
    }
    list->pairs[list->count].key = key;
    list->pairs[list->count].value = value;
    list->count++;
}

void kv_list_free(KVList *list) {
    free(list->pairs);
    kv_list_init(list);
}

MgTypeRef *type_ref_alloc(MgTypeKind kind, int line) {
    MgTypeRef *type = (MgTypeRef *)calloc(1, sizeof(MgTypeRef));
    if (!type) {
        fprintf(stderr, "Out of memory allocating type reference.\n");
        exit(1);
    }
    type->kind = kind;
    type->line = line;
    return type;
}

static void type_field_list_write(MgTypeRef *shape, Token name, MgTypeRef *field_type) {
    if (shape->kind != MG_TYPE_SHAPE) return;
    if (shape->as.shape.field_capacity < shape->as.shape.field_count + 1) {
        shape->as.shape.field_capacity = shape->as.shape.field_capacity < 4
            ? 4
            : shape->as.shape.field_capacity * 2;
        shape->as.shape.fields = (MgTypeField *)realloc(
            shape->as.shape.fields,
            sizeof(MgTypeField) * shape->as.shape.field_capacity);
    }
    shape->as.shape.fields[shape->as.shape.field_count].name = name;
    shape->as.shape.fields[shape->as.shape.field_count].type = field_type;
    shape->as.shape.field_count++;
}

void type_ref_free(MgTypeRef *type) {
    if (!type) return;
    switch (type->kind) {
        case MG_TYPE_ARRAY:
            type_ref_free(type->as.array.element);
            break;
        case MG_TYPE_DICT:
            type_ref_free(type->as.dict.key);
            type_ref_free(type->as.dict.value);
            break;
        case MG_TYPE_FUNCTION:
            for (int i = 0; i < type->as.function.param_count; i++) {
                type_ref_free(type->as.function.params[i]);
            }
            free(type->as.function.params);
            type_ref_free(type->as.function.return_type);
            break;
        case MG_TYPE_SHAPE:
            for (int i = 0; i < type->as.shape.field_count; i++) {
                type_ref_free(type->as.shape.fields[i].type);
            }
            free(type->as.shape.fields);
            break;
        case MG_TYPE_NAME:
            break;
    }
    free(type);
}

/* Recursively free an AST tree */
void ast_free(ASTNode *node) {
    if (!node) return;

    switch (node->type) {
        case NODE_NUMBER:
        case NODE_BOOL:
        case NODE_NULL:
        case NODE_IDENTIFIER:
        case NODE_BREAK:
        case NODE_CONTINUE:
            break;

        case NODE_STRING:
        case NODE_INTERP_STRING:
            free(node->as.string.value);
            break;

        case NODE_UNARY:
            ast_free(node->as.unary.operand);
            break;

        case NODE_BINARY:
            ast_free(node->as.binary.left);
            ast_free(node->as.binary.right);
            break;

        case NODE_LOGICAL:
            ast_free(node->as.logical.left);
            ast_free(node->as.logical.right);
            break;

        case NODE_ASSIGN:
            /*
             * Compound indexed assignments deliberately share the parsed
             * receiver and index between the write target and the synthetic
             * read on the right-hand side.  The compiler uses that identity
             * to evaluate the lvalue exactly once.  Detach the non-owning
             * references before recursively freeing the tree.
             */
            if (node->as.assign.target &&
                node->as.assign.target->type == NODE_INDEX &&
                node->as.assign.value &&
                node->as.assign.value->type == NODE_BINARY &&
                node->as.assign.value->as.binary.left &&
                node->as.assign.value->as.binary.left->type == NODE_INDEX) {
                ASTNode *target = node->as.assign.target;
                ASTNode *read = node->as.assign.value->as.binary.left;
                if (target->as.index_expr.object == read->as.index_expr.object &&
                    target->as.index_expr.index == read->as.index_expr.index) {
                    read->as.index_expr.object = NULL;
                    read->as.index_expr.index = NULL;
                }
            }
            ast_free(node->as.assign.target);
            ast_free(node->as.assign.value);
            break;

        case NODE_CALL:
            ast_free(node->as.call.callee);
            for (int i = 0; i < node->as.call.args.count; i++)
                ast_free(node->as.call.args.nodes[i]);
            node_list_free(&node->as.call.args);
            break;

        case NODE_INDEX:
            ast_free(node->as.index_expr.object);
            ast_free(node->as.index_expr.index);
            break;

        case NODE_TRY:
            ast_free(node->as.try_expr.expr);
            break;

        case NODE_FIELD_GET:
            ast_free(node->as.field_get.object);
            break;

        case NODE_FIELD_SET:
            /*
             * See NODE_ASSIGN above.  A compound field assignment shares its
             * receiver with the synthetic field read so it can be compiled as
             * one receiver evaluation.
             */
            if (node->as.field_set.value &&
                node->as.field_set.value->type == NODE_BINARY &&
                node->as.field_set.value->as.binary.left &&
                node->as.field_set.value->as.binary.left->type == NODE_FIELD_GET &&
                node->as.field_set.object ==
                    node->as.field_set.value->as.binary.left->as.field_get.object) {
                node->as.field_set.value->as.binary.left->as.field_get.object = NULL;
            }
            ast_free(node->as.field_set.object);
            ast_free(node->as.field_set.value);
            break;

        case NODE_ARRAY_LITERAL:
            for (int i = 0; i < node->as.array_literal.items.count; i++)
                ast_free(node->as.array_literal.items.nodes[i]);
            node_list_free(&node->as.array_literal.items);
            break;

        case NODE_DICT_LITERAL:
            for (int i = 0; i < node->as.dict_literal.entries.count; i++) {
                ast_free(node->as.dict_literal.entries.pairs[i].key);
                ast_free(node->as.dict_literal.entries.pairs[i].value);
            }
            kv_list_free(&node->as.dict_literal.entries);
            break;

        case NODE_STRUCT_LITERAL:
            for (int i = 0; i < node->as.struct_literal.fields.count; i++) {
                ast_free(node->as.struct_literal.fields.pairs[i].key);
                ast_free(node->as.struct_literal.fields.pairs[i].value);
            }
            kv_list_free(&node->as.struct_literal.fields);
            break;

        case NODE_EXPRESSION_STMT:
            ast_free(node->as.expr_stmt.expr);
            break;

        case NODE_LET:
        case NODE_CONST:
            free(node->as.var_decl.extra_names);
            if (node->as.var_decl.extra_type_annotations) {
                for (int i = 1; i < node->as.var_decl.name_count; i++) {
                    type_ref_free(node->as.var_decl.extra_type_annotations[i - 1]);
                }
            }
            free(node->as.var_decl.extra_type_annotations);
            type_ref_free(node->as.var_decl.type_annotation);
            ast_free(node->as.var_decl.initializer);
            break;

        case NODE_BLOCK:
            for (int i = 0; i < node->as.block.stmts.count; i++)
                ast_free(node->as.block.stmts.nodes[i]);
            node_list_free(&node->as.block.stmts);
            break;

        case NODE_IF:
            ast_free(node->as.if_stmt.condition);
            ast_free(node->as.if_stmt.then_branch);
            ast_free(node->as.if_stmt.else_branch);
            break;

        case NODE_LOOP:
            ast_free(node->as.loop_stmt.body);
            break;

        case NODE_FOR_RANGE:
            ast_free(node->as.for_range.start);
            ast_free(node->as.for_range.end);
            ast_free(node->as.for_range.body);
            break;

        case NODE_FOR_IN:
            ast_free(node->as.for_in.iterable);
            ast_free(node->as.for_in.body);
            break;

        case NODE_RETURN:
            for (int i = 0; i < node->as.return_stmt.values.count; i++)
                ast_free(node->as.return_stmt.values.nodes[i]);
            node_list_free(&node->as.return_stmt.values);
            break;

        case NODE_TRY_BLOCK:
            ast_free(node->as.try_block.body);
            break;

        case NODE_TRY_CATCH:
            ast_free(node->as.try_catch.body);
            ast_free(node->as.try_catch.catch_body);
            break;

        case NODE_DEFER:
            ast_free(node->as.defer_stmt.call);
            break;

        case NODE_FN_DECL:
            for (int i = 0; i < node->as.fn_decl.param_count; i++) {
                type_ref_free(node->as.fn_decl.params[i].type);
            }
            free(node->as.fn_decl.params);
            type_ref_free(node->as.fn_decl.return_type);
            ast_free(node->as.fn_decl.body);
            break;

        case NODE_STRUCT_DECL:
            for (int i = 0; i < node->as.struct_decl.field_count; i++) {
                type_ref_free(node->as.struct_decl.fields[i].type);
            }
            free(node->as.struct_decl.fields);
            break;

        case NODE_ENUM_DECL:
            free(node->as.enum_decl.variants);
            break;

        case NODE_EXPORT:
            ast_free(node->as.export_stmt.declaration);
            break;

        case NODE_IMPORT:
            break;

        case NODE_DIRECTIVE:
            break;

        case NODE_TYPE_ALIAS:
            type_ref_free(node->as.type_alias.type);
            break;

        case NODE_EXTERN_DECL:
            for (int i = 0; i < node->as.extern_decl.param_count; i++) {
                type_ref_free(node->as.extern_decl.params[i].type);
            }
            free(node->as.extern_decl.params);
            type_ref_free(node->as.extern_decl.type);
            break;
    }

    free(node);
}

/* ========================================================================
 * Error Reporting
 * ======================================================================== */
static void error_at(Token *token, const char *message) {
    if (ps.panic_mode) return;
    ps.panic_mode = true;
    fprintf(stderr, "[line %d] Error", token->line);

    if (token->type == TOKEN_EOF) {
        fprintf(stderr, " at end");
    } else if (token->type != TOKEN_ERROR) {
        fprintf(stderr, " at '%.*s'", token->length, token->start);
    }

    fprintf(stderr, ": %s\n", message);
    ps.had_error = true;
}

static void error(const char *message) {
    error_at(&ps.previous, message);
}

static void error_current(const char *message) {
    error_at(&ps.current, message);
}

/* ========================================================================
 * Token Stream
 * ======================================================================== */
static void advance_token(void) {
    ps.previous = ps.current;

    for (;;) {
        ps.current = scan_token(&ps.scanner);
        if (ps.current.type != TOKEN_ERROR) break;
        error_current(ps.current.start);
    }
}

static bool check(MgTokenType type) {
    return ps.current.type == type;
}

static bool match_token(MgTokenType type) {
    if (!check(type)) return false;
    advance_token();
    return true;
}

static void consume(MgTokenType type, const char *message) {
    if (ps.current.type == type) {
        advance_token();
        return;
    }
    error_current(message);
}

/* Synchronize after an error -- skip tokens until we find a statement boundary */
static void synchronize(void) {
    ps.panic_mode = false;

    while (ps.current.type != TOKEN_EOF) {
        /* Stop at likely statement boundaries */
        switch (ps.current.type) {
            case TOKEN_LET:
            case TOKEN_CONST:
            case TOKEN_FN:
            case TOKEN_STRUCT:
            case TOKEN_ENUM:
            case TOKEN_IF:
            case TOKEN_FOR:
            case TOKEN_LOOP:
            case TOKEN_RETURN:
            case TOKEN_DEFER:
            case TOKEN_IMPORT:
            case TOKEN_EXPORT:
            case TOKEN_END:
                return;
            default:
                break;
        }
        advance_token();
    }
}

/* ========================================================================
 * Expression Parsing (Pratt-style precedence climbing)
 * ======================================================================== */
static ASTNode *parse_expression(void);
static ASTNode *parse_precedence(int min_prec);
static ASTNode *parse_declaration(void);
static ASTNode *parse_statement(void);
static MgTypeRef *parse_type_ref(void);

static bool token_matches(Token *token, const char *text) {
    int len = (int)strlen(text);
    return token->length == len && memcmp(token->start, text, (size_t)len) == 0;
}

static Token peek_next_token(void) {
    Scanner lookahead = ps.scanner;
    return scan_token(&lookahead);
}

static bool check_identifier_text(const char *text) {
    return check(TOKEN_IDENTIFIER) && token_matches(&ps.current, text);
}

/* Helper: extract string content from a string token (strips quotes) */
static char *extract_string(Token *token) {
    int start = 1;
    int len = token->length - 2;
    if (len < 0) len = 0;

    char *str = (char *)malloc(len + 1);
    memcpy(str, token->start + start, len);
    str[len] = '\0';
    return str;
}

static MgTypeRef *parse_named_type_from_previous(void) {
    MgTypeRef *type = type_ref_alloc(MG_TYPE_NAME, ps.previous.line);
    type->as.name.name = ps.previous;
    return type;
}

static MgTypeRef *parse_shape_or_dict_type(void) {
    int line = ps.previous.line;

    if (check(TOKEN_IDENTIFIER)) {
        Token next = peek_next_token();
        if (next.type == TOKEN_COLON) {
            MgTypeRef *shape = type_ref_alloc(MG_TYPE_SHAPE, ps.current.line);
            if (!check(TOKEN_GREATER)) {
                do {
                    consume(TOKEN_IDENTIFIER, "Expected field name in data shape type.");
                    Token field_name = ps.previous;
                    consume(TOKEN_COLON, "Expected ':' after data shape field name.");
                    MgTypeRef *field_type = parse_type_ref();
                    type_field_list_write(shape, field_name, field_type);
                    if (check(TOKEN_GREATER)) break;
                } while (match_token(TOKEN_COMMA));
            }
            consume(TOKEN_GREATER, "Expected '>' after data shape type.");
            return shape;
        }
    }

    MgTypeRef *dict = type_ref_alloc(MG_TYPE_DICT, line);
    dict->as.dict.key = parse_type_ref();
    consume(TOKEN_COMMA, "Expected ',' between dict key and value types.");
    dict->as.dict.value = parse_type_ref();
    consume(TOKEN_GREATER, "Expected '>' after dict type.");
    return dict;
}

static MgTypeRef *parse_function_type(void) {
    MgTypeRef *type = type_ref_alloc(MG_TYPE_FUNCTION, ps.previous.line);
    consume(TOKEN_LEFT_PAREN, "Expected '(' after 'fn' in function type.");

    int capacity = 0;
    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (type->as.function.param_count >= capacity) {
                capacity = capacity < 4 ? 4 : capacity * 2;
                type->as.function.params = (MgTypeRef **)realloc(
                    type->as.function.params, sizeof(MgTypeRef *) * capacity);
            }
            type->as.function.params[type->as.function.param_count++] = parse_type_ref();
        } while (match_token(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after function type parameters.");
    if (match_token(TOKEN_COLON)) {
        type->as.function.return_type = parse_type_ref();
    } else {
        Token null_tok = {TOKEN_NULL, "null", 4, ps.previous.line};
        MgTypeRef *ret = type_ref_alloc(MG_TYPE_NAME, ps.previous.line);
        ret->as.name.name = null_tok;
        type->as.function.return_type = ret;
    }
    return type;
}

static MgTypeRef *parse_type_ref(void) {
    if (match_token(TOKEN_LEFT_BRACKET)) {
        MgTypeRef *type = type_ref_alloc(MG_TYPE_ARRAY, ps.previous.line);
        type->as.array.element = parse_type_ref();
        consume(TOKEN_RIGHT_BRACKET, "Expected ']' after array element type.");
        return type;
    }

    if (match_token(TOKEN_AMPERSAND)) {
        consume(TOKEN_LESS, "Expected '<' after '&' in dict or data shape type.");
        return parse_shape_or_dict_type();
    }

    if (match_token(TOKEN_FN)) {
        return parse_function_type();
    }

    if (match_token(TOKEN_IDENTIFIER) || match_token(TOKEN_NULL)) {
        return parse_named_type_from_previous();
    }

    error_current("Expected type.");
    advance_token();
    Token any_tok = {TOKEN_IDENTIFIER, "any", 3, ps.previous.line};
    MgTypeRef *fallback = type_ref_alloc(MG_TYPE_NAME, ps.previous.line);
    fallback->as.name.name = any_tok;
    return fallback;
}

/* extract_interp_string removed; interpolation now handled in parse_interp_string */

/* --- Primary expressions --- */

static ASTNode *parse_number(void) {
    ASTNode *node = ast_alloc(NODE_NUMBER, ps.previous.line);
    node->as.number.value = strtod(ps.previous.start, NULL);
    return node;
}

static ASTNode *parse_string_node(void) {
    ASTNode *node = ast_alloc(NODE_STRING, ps.previous.line);
    node->as.string.value = extract_string(&ps.previous);
    node->as.string.length = ps.previous.length - 2;
    return node;
}

static ASTNode *append_interp_part(ASTNode *result, ASTNode *part, int line) {
    if (!result) return part;
    ASTNode *cat = ast_alloc(NODE_BINARY, line);
    cat->as.binary.op = TOKEN_PLUS;
    cat->as.binary.left = result;
    cat->as.binary.right = part;
    return cat;
}

static ASTNode *parse_interp_string(void) {
    int line = ps.previous.line;
    /* Extract content between _" and " */
    const char *src = ps.previous.start + 2; /* skip _" */
    int total_len = ps.previous.length - 3;  /* exclude _" and closing " */
    if (total_len < 0) total_len = 0;

    /* Build a chain of string concat: "literal" + tostring(expr) + "literal" + ... */
    ASTNode *result = NULL;
    int pos = 0;

    while (pos < total_len) {
        /* Find next '{' */
        int seg_start = pos;
        while (pos < total_len) {
            if (src[pos] == '\\' && pos + 1 < total_len) {
                pos += 2;
                continue;
            }
            if (src[pos] == '{') break;
            pos++;
        }

        /* Emit literal segment if non-empty */
        if (pos > seg_start) {
            int len = pos - seg_start;
            ASTNode *lit = ast_alloc(NODE_STRING, line);
            lit->as.string.value = (char *)malloc(len + 1);
            memcpy(lit->as.string.value, src + seg_start, len);
            lit->as.string.value[len] = '\0';
            lit->as.string.length = len;

            result = append_interp_part(result, lit, line);
        }

        if (pos >= total_len) break;

        /* Skip '{' */
        pos++;
        int expr_start = pos;
        int brace_depth = 1;
        bool in_string = false;
        while (pos < total_len && brace_depth > 0) {
            if (src[pos] == '\\' && pos + 1 < total_len) {
                pos += 2;
                continue;
            }
            if (src[pos] == '"') {
                in_string = !in_string;
            } else if (!in_string && src[pos] == '{') {
                brace_depth++;
            } else if (!in_string && src[pos] == '}') {
                brace_depth--;
            }
            if (brace_depth > 0) pos++;
        }
        int expr_len = pos - expr_start;
        if (pos < total_len) pos++; /* skip '}' */

        if (expr_len > 0) {
            /*
             * Always use the normal lexer/parser.  Besides keeping interpolation
             * semantics in sync with ordinary expressions, this recognizes
             * keyword literals such as true, false, and null correctly.
             * The closing '}' naturally terminates parse_expression().
             */
            ParserState saved = ps;
            ps.scanner.start = src + expr_start;
            ps.scanner.current = src + expr_start;
            ps.scanner.line = line;
            ps.current = (Token){0};
            ps.previous = (Token){0};
            ps.had_error = false;
            ps.panic_mode = false;

            advance_token();
            ASTNode *expr = parse_expression();
            if (!check(TOKEN_RIGHT_BRACE)) {
                error_current("Expected '}' after interpolation expression.");
            }
            bool interpolation_error = ps.had_error;
            ps = saved;
            if (interpolation_error) ps.had_error = true;

            /* Wrap in __builtin_tostring() call */
            ASTNode *tostr_id = ast_alloc(NODE_IDENTIFIER, line);
            Token ts_tok = {TOKEN_IDENTIFIER, "__builtin_tostring", 18, line};
            tostr_id->as.identifier.name = ts_tok;
            tostr_id->as.identifier.is_global = false;

            ASTNode *call = ast_alloc(NODE_CALL, line);
            call->as.call.callee = tostr_id;
            node_list_init(&call->as.call.args);
            node_list_write(&call->as.call.args, expr);

            result = append_interp_part(result, call, line);
        }
    }

    if (!result) {
        result = ast_alloc(NODE_STRING, line);
        result->as.string.value = (char *)malloc(1);
        result->as.string.value[0] = '\0';
        result->as.string.length = 0;
    }

    return result;
}

static ASTNode *parse_bool(bool value) {
    ASTNode *node = ast_alloc(NODE_BOOL, ps.previous.line);
    node->as.boolean.value = value;
    return node;
}

static ASTNode *parse_null(void) {
    return ast_alloc(NODE_NULL, ps.previous.line);
}

static ASTNode *parse_identifier_node(void) {
    ASTNode *node = ast_alloc(NODE_IDENTIFIER, ps.previous.line);
    node->as.identifier.name = ps.previous;
    node->as.identifier.is_global = false;
    return node;
}

static ASTNode *parse_global_identifier(void) {
    consume(TOKEN_IDENTIFIER, "Expected identifier after '@'.");
    ASTNode *node = ast_alloc(NODE_IDENTIFIER, ps.previous.line);
    node->as.identifier.name = ps.previous;
    node->as.identifier.is_global = true;
    return node;
}

static ASTNode *parse_grouping(void) {
    ASTNode *expr = parse_expression();
    consume(TOKEN_RIGHT_PAREN, "Expected ')' after expression.");
    return expr;
}

static ASTNode *parse_array_literal(void) {
    ASTNode *node = ast_alloc(NODE_ARRAY_LITERAL, ps.previous.line);
    node_list_init(&node->as.array_literal.items);

    if (!check(TOKEN_RIGHT_BRACKET)) {
        do {
            node_list_write(&node->as.array_literal.items, parse_expression());
        } while (match_token(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_BRACKET, "Expected ']' after array elements.");
    return node;
}

static ASTNode *parse_dict_literal(void) {
    ASTNode *node = ast_alloc(NODE_DICT_LITERAL, ps.previous.line);
    kv_list_init(&node->as.dict_literal.entries);

    if (!check(TOKEN_GREATER)) {
        do {
            ASTNode *key;
            if (check(TOKEN_IDENTIFIER)) {
                advance_token();
                key = ast_alloc(NODE_STRING, ps.previous.line);
                key->as.string.value = (char *)malloc(ps.previous.length + 1);
                memcpy(key->as.string.value, ps.previous.start, ps.previous.length);
                key->as.string.value[ps.previous.length] = '\0';
                key->as.string.length = ps.previous.length;
            } else if (check(TOKEN_STRING)) {
                advance_token();
                key = parse_string_node();
            } else {
                error_current("Expected key in dictionary literal.");
                key = ast_alloc(NODE_NULL, ps.current.line);
            }

            consume(TOKEN_EQUAL, "Expected '=' after dictionary key.");
            ASTNode *value = parse_precedence(6); /* PREC_RANGE: stop before < > comparisons */
            kv_list_write(&node->as.dict_literal.entries, key, value);

            if (check(TOKEN_GREATER)) break;
        } while (match_token(TOKEN_COMMA));
    }

    consume(TOKEN_GREATER, "Expected '>' after dictionary entries.");
    return node;
}

static ASTNode *parse_fn_literal(void);
static ASTNode *parse_try_expression(void);

/* Primary */
static ASTNode *parse_primary(void) {
    if (match_token(TOKEN_NUMBER)) return parse_number();
    if (match_token(TOKEN_STRING)) return parse_string_node();
    if (match_token(TOKEN_INTERP_STRING)) return parse_interp_string();
    if (match_token(TOKEN_TRUE)) return parse_bool(true);
    if (match_token(TOKEN_FALSE)) return parse_bool(false);
    if (match_token(TOKEN_NULL)) return parse_null();
    if (match_token(TOKEN_IDENTIFIER)) {
        /* Check for struct literal: Name { field = val, ... } */
        if (check(TOKEN_LEFT_BRACE)) {
            Token name = ps.previous;
            advance_token(); /* consume '{' */
            ASTNode *node = ast_alloc(NODE_STRUCT_LITERAL, name.line);
            node->as.struct_literal.name = name;
            kv_list_init(&node->as.struct_literal.fields);

            if (!check(TOKEN_RIGHT_BRACE)) {
                do {
                    consume(TOKEN_IDENTIFIER, "Expected field name.");
                    ASTNode *key = ast_alloc(NODE_STRING, ps.previous.line);
                    key->as.string.value = (char *)malloc(ps.previous.length + 1);
                    memcpy(key->as.string.value, ps.previous.start, ps.previous.length);
                    key->as.string.value[ps.previous.length] = '\0';
                    key->as.string.length = ps.previous.length;

                    consume(TOKEN_EQUAL, "Expected '=' after field name.");
                    ASTNode *value = parse_expression();
                    kv_list_write(&node->as.struct_literal.fields, key, value);
                } while (match_token(TOKEN_COMMA));
            }

            consume(TOKEN_RIGHT_BRACE, "Expected '}' after struct fields.");
            return node;
        }
        return parse_identifier_node();
    }
    if (match_token(TOKEN_AT)) return parse_global_identifier();
    if (match_token(TOKEN_LEFT_PAREN)) return parse_grouping();
    if (match_token(TOKEN_LEFT_BRACKET)) return parse_array_literal();
    if (match_token(TOKEN_AMPERSAND)) {
        consume(TOKEN_LESS, "Expected '<' after '&' for dict literal.");
        return parse_dict_literal();
    }
    if (match_token(TOKEN_FN)) return parse_fn_literal();
    if (match_token(TOKEN_TRY)) return parse_try_expression();

    error_current("Expected expression.");
    advance_token();
    return ast_alloc(NODE_NULL, ps.previous.line);
}

/* Postfix (calls, indexing, field access) */
static bool less_starts_dict_index(void) {
    if (!check(TOKEN_LESS)) return false;

    Scanner lookahead = ps.scanner;
    int depth = 0;
    bool saw_key_token = false;

    for (;;) {
        Token token = scan_token(&lookahead);

        if (token.type == TOKEN_ERROR || token.type == TOKEN_EOF) {
            return false;
        }

        if (depth == 0) {
            switch (token.type) {
                case TOKEN_GREATER:
                    if (!saw_key_token) return false;
                    /*
                     * In `a < b and c > d`, the `>` is followed by another
                     * operand on the same source line, so it is a comparison,
                     * not the terminator of legacy dict-index syntax.
                     */
                    {
                        Scanner after = lookahead;
                        Token next = scan_token(&after);
                        bool starts_operand =
                            next.type == TOKEN_NUMBER ||
                            next.type == TOKEN_STRING ||
                            next.type == TOKEN_INTERP_STRING ||
                            next.type == TOKEN_TRUE ||
                            next.type == TOKEN_FALSE ||
                            next.type == TOKEN_NULL ||
                            next.type == TOKEN_IDENTIFIER ||
                            next.type == TOKEN_AT ||
                            next.type == TOKEN_AMPERSAND ||
                            next.type == TOKEN_FN ||
                            next.type == TOKEN_TRY;
                        if (starts_operand && next.line == token.line) return false;
                    }
                    return true;
                case TOKEN_THEN:
                case TOKEN_END:
                case TOKEN_ELSE:
                case TOKEN_ELSEIF:
                case TOKEN_CATCH:
                case TOKEN_COMMA:
                case TOKEN_SEMICOLON:
                    return false;
                default:
                    break;
            }
        }

        switch (token.type) {
            case TOKEN_LEFT_PAREN:
            case TOKEN_LEFT_BRACKET:
            case TOKEN_LEFT_BRACE:
                depth++;
                saw_key_token = true;
                break;
            case TOKEN_RIGHT_PAREN:
            case TOKEN_RIGHT_BRACKET:
            case TOKEN_RIGHT_BRACE:
                if (depth == 0) return false;
                depth--;
                saw_key_token = true;
                break;
            default:
                saw_key_token = true;
                break;
        }
    }
}

static ASTNode *parse_postfix(ASTNode *left) {
    for (;;) {
        if (match_token(TOKEN_LEFT_PAREN)) {
            ASTNode *node = ast_alloc(NODE_CALL, ps.previous.line);
            node->as.call.callee = left;
            node_list_init(&node->as.call.args);

            if (!check(TOKEN_RIGHT_PAREN)) {
                do {
                    node_list_write(&node->as.call.args, parse_expression());
                } while (match_token(TOKEN_COMMA));
            }

            consume(TOKEN_RIGHT_PAREN, "Expected ')' after arguments.");
            left = node;
        } else if (match_token(TOKEN_LEFT_BRACKET)) {
            ASTNode *node = ast_alloc(NODE_INDEX, ps.previous.line);
            node->as.index_expr.object = left;
            node->as.index_expr.index = parse_expression();
            consume(TOKEN_RIGHT_BRACKET, "Expected ']' after index.");
            left = node;
        } else if (less_starts_dict_index()) {
            advance_token(); /* consume '<' */
            ASTNode *node = ast_alloc(NODE_INDEX, ps.previous.line);
            node->as.index_expr.object = left;
            node->as.index_expr.index = parse_precedence(6); /* PREC_RANGE */
            consume(TOKEN_GREATER, "Expected '>' after dict index.");
            left = node;
        } else if (match_token(TOKEN_DOT)) {
            consume(TOKEN_IDENTIFIER, "Expected field name after '.'.");
            ASTNode *node = ast_alloc(NODE_FIELD_GET, ps.previous.line);
            node->as.field_get.object = left;
            node->as.field_get.name = ps.previous;
            left = node;
        } else if (match_token(TOKEN_QUESTION)) {
            ASTNode *node = ast_alloc(NODE_TRY, ps.previous.line);
            node->as.try_expr.expr = left;
            left = node;
        } else {
            break;
        }
    }
    return left;
}

/* Unary */
static ASTNode *parse_unary(void) {
    if (match_token(TOKEN_MINUS) || match_token(TOKEN_BANG) || match_token(TOKEN_NOT)) {
        MgTokenType op = ps.previous.type;
        if (op == TOKEN_NOT) op = TOKEN_BANG; /* normalize not to ! */
        int line = ps.previous.line;
        ASTNode *operand = parse_unary();
        ASTNode *node = ast_alloc(NODE_UNARY, line);
        node->as.unary.op = op;
        node->as.unary.operand = operand;
        return node;
    }

    ASTNode *left = parse_primary();
    return parse_postfix(left);
}

/* Binary operators by precedence */
enum {
    PREC_NONE = 0,
    PREC_ASSIGNMENT,
    PREC_OR,
    PREC_AND,
    PREC_EQUALITY,
    PREC_COMPARISON,
    PREC_RANGE,
    PREC_TERM,
    PREC_FACTOR,
    PREC_UNARY_LVL,
    PREC_CALL,
    PREC_PRIMARY
};

static int get_infix_precedence(MgTokenType type) {
    switch (type) {
        case TOKEN_OR:              return PREC_OR;
        case TOKEN_AND:             return PREC_AND;
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:      return PREC_EQUALITY;
        case TOKEN_LESS:
        case TOKEN_GREATER:
        case TOKEN_LESS_EQUAL:
        case TOKEN_GREATER_EQUAL:   return PREC_COMPARISON;
        case TOKEN_DOT_DOT:
        case TOKEN_DOT_DOT_EQUAL:   return PREC_RANGE;
        case TOKEN_PLUS:
        case TOKEN_MINUS:           return PREC_TERM;
        case TOKEN_STAR:
        case TOKEN_SLASH:
        case TOKEN_PERCENT:         return PREC_FACTOR;
        default:                    return PREC_NONE;
    }
}

static ASTNode *parse_precedence(int min_prec) {
    ASTNode *left = parse_unary();

    while (get_infix_precedence(ps.current.type) >= min_prec) {
        advance_token();
        MgTokenType op = ps.previous.type;
        int line = ps.previous.line;

        if (op == TOKEN_AND || op == TOKEN_OR) {
            ASTNode *right = parse_precedence(get_infix_precedence(op) + 1);
            ASTNode *node = ast_alloc(NODE_LOGICAL, line);
            node->as.logical.op = op;
            node->as.logical.left = left;
            node->as.logical.right = right;
            left = node;
        } else {
            ASTNode *right = parse_precedence(get_infix_precedence(op) + 1);
            ASTNode *node = ast_alloc(NODE_BINARY, line);
            node->as.binary.op = op;
            node->as.binary.left = left;
            node->as.binary.right = right;
            left = node;
        }
    }

    return left;
}

static ASTNode *parse_expression(void) {
    return parse_precedence(PREC_OR);
}

/* ========================================================================
 * Assignment (expression statements can also be assignments)
 * ======================================================================== */
static ASTNode *parse_expression_or_assignment(void) {
    ASTNode *expr = parse_precedence(PREC_OR);

    if (match_token(TOKEN_EQUAL)) {
        int line = ps.previous.line;
        ASTNode *value = parse_expression();

        if (expr->type == NODE_IDENTIFIER || expr->type == NODE_INDEX) {
            ASTNode *node = ast_alloc(NODE_ASSIGN, line);
            node->as.assign.target = expr;
            node->as.assign.value = value;
            return node;
        }

        if (expr->type == NODE_FIELD_GET) {
            ASTNode *set = ast_alloc(NODE_FIELD_SET, line);
            set->as.field_set.object = expr->as.field_get.object;
            set->as.field_set.name = expr->as.field_get.name;
            set->as.field_set.value = value;
            free(expr);
            return set;
        }

        error("Invalid assignment target.");
        ast_free(value);
    } else if (match_token(TOKEN_PLUS_EQUAL) || match_token(TOKEN_MINUS_EQUAL) ||
               match_token(TOKEN_STAR_EQUAL) || match_token(TOKEN_SLASH_EQUAL) ||
               match_token(TOKEN_PERCENT_EQUAL)) {
        int line = ps.previous.line;
        MgTokenType compound_op = ps.previous.type;
        ASTNode *value = parse_expression();

        MgTokenType base_op;
        switch (compound_op) {
            case TOKEN_PLUS_EQUAL:    base_op = TOKEN_PLUS; break;
            case TOKEN_MINUS_EQUAL:   base_op = TOKEN_MINUS; break;
            case TOKEN_STAR_EQUAL:    base_op = TOKEN_STAR; break;
            case TOKEN_SLASH_EQUAL:   base_op = TOKEN_SLASH; break;
            case TOKEN_PERCENT_EQUAL: base_op = TOKEN_PERCENT; break;
            default: base_op = TOKEN_PLUS; break;
        }

        if (expr->type == NODE_IDENTIFIER) {
            ASTNode *read = ast_alloc(NODE_IDENTIFIER, line);
            read->as.identifier = expr->as.identifier;
            ASTNode *rhs = ast_alloc(NODE_BINARY, line);
            rhs->as.binary.op = base_op;
            rhs->as.binary.left = read;
            rhs->as.binary.right = value;
            ASTNode *node = ast_alloc(NODE_ASSIGN, line);
            node->as.assign.target = expr;
            node->as.assign.value = rhs;
            return node;
        }

        if (expr->type == NODE_FIELD_GET) {
            ASTNode *read = ast_alloc(NODE_FIELD_GET, line);
            read->as.field_get.object = expr->as.field_get.object;
            read->as.field_get.name = expr->as.field_get.name;
            ASTNode *rhs = ast_alloc(NODE_BINARY, line);
            rhs->as.binary.op = base_op;
            rhs->as.binary.left = expr;
            rhs->as.binary.right = value;
            ASTNode *set = ast_alloc(NODE_FIELD_SET, line);
            set->as.field_set.object = read->as.field_get.object;
            set->as.field_set.name = read->as.field_get.name;
            set->as.field_set.value = rhs;
            free(read);
            return set;
        }

        if (expr->type == NODE_INDEX) {
            ASTNode *read = ast_alloc(NODE_INDEX, line);
            read->as.index_expr.object = expr->as.index_expr.object;
            read->as.index_expr.index = expr->as.index_expr.index;
            ASTNode *rhs = ast_alloc(NODE_BINARY, line);
            rhs->as.binary.op = base_op;
            rhs->as.binary.left = expr;
            rhs->as.binary.right = value;
            ASTNode *node = ast_alloc(NODE_ASSIGN, line);
            node->as.assign.target = read;
            node->as.assign.value = rhs;
            return node;
        }

        error("Invalid compound assignment target.");
        ast_free(value);
    }

    return expr;
}

/* ========================================================================
 * Statement Parsing
 * ======================================================================== */

static ASTNode *parse_block(void) {
    ASTNode *block = ast_alloc(NODE_BLOCK, ps.current.line);
    node_list_init(&block->as.block.stmts);

    while (!check(TOKEN_END) && !check(TOKEN_ELSE) && !check(TOKEN_ELSEIF) &&
           !check(TOKEN_CATCH) &&
           !check(TOKEN_EOF)) {
        ASTNode *decl = parse_declaration();
        if (decl) {
            node_list_write(&block->as.block.stmts, decl);
        }
        if (ps.panic_mode) synchronize();
    }

    return block;
}

static ASTNode *parse_try_expression(void) {
    ASTNode *node = ast_alloc(NODE_TRY_BLOCK, ps.previous.line);
    node->as.try_block.body = parse_block();
    consume(TOKEN_END, "Expected 'end' after try expression.");
    return node;
}

static ASTNode *parse_try_statement(void) {
    ASTNode *node = ast_alloc(NODE_TRY_CATCH, ps.previous.line);
    node->as.try_catch.body = parse_block();
    consume(TOKEN_CATCH, "Expected 'catch' after try block.");
    consume(TOKEN_IDENTIFIER, "Expected error name after 'catch'.");
    node->as.try_catch.err_name = ps.previous;
    node->as.try_catch.catch_body = parse_block();
    consume(TOKEN_END, "Expected 'end' after catch block.");
    return node;
}

/* let [@]name [, name2 [, name3 ...]] [= expr] */
static ASTNode *parse_let_declaration(void) {
    ASTNode *node = ast_alloc(NODE_LET, ps.previous.line);
    node->as.var_decl.is_const = false;
    node->as.var_decl.extra_names = NULL;
    node->as.var_decl.extra_type_annotations = NULL;
    node->as.var_decl.type_annotation = NULL;
    node->as.var_decl.name_count = 1;

    if (match_token(TOKEN_AT)) {
        node->as.var_decl.is_global = true;
        consume(TOKEN_IDENTIFIER, "Expected variable name after '@'.");
    } else {
        node->as.var_decl.is_global = false;
        consume(TOKEN_IDENTIFIER, "Expected variable name.");
    }

    node->as.var_decl.name = ps.previous;
    if (match_token(TOKEN_COLON)) {
        node->as.var_decl.type_annotation = parse_type_ref();
    }

    /* Check for additional names: let a, b, c = ... */
    int extra_cap = 0;
    while (match_token(TOKEN_COMMA)) {
        consume(TOKEN_IDENTIFIER, "Expected variable name after ','.");
        if (node->as.var_decl.name_count - 1 >= extra_cap) {
            extra_cap = extra_cap < 4 ? 4 : extra_cap * 2;
            node->as.var_decl.extra_names = (Token *)realloc(
                node->as.var_decl.extra_names, sizeof(Token) * extra_cap);
            node->as.var_decl.extra_type_annotations = (MgTypeRef **)realloc(
                node->as.var_decl.extra_type_annotations, sizeof(MgTypeRef *) * extra_cap);
        }
        int extra_index = node->as.var_decl.name_count - 1;
        node->as.var_decl.extra_names[extra_index] = ps.previous;
        node->as.var_decl.extra_type_annotations[extra_index] = NULL;
        if (match_token(TOKEN_COLON)) {
            node->as.var_decl.extra_type_annotations[extra_index] = parse_type_ref();
        }
        node->as.var_decl.name_count++;
    }

    if (match_token(TOKEN_EQUAL)) {
        node->as.var_decl.initializer = parse_expression();
    } else {
        node->as.var_decl.initializer = NULL;
    }

    return node;
}

/* const name = expr */
static ASTNode *parse_const_declaration(void) {
    ASTNode *node = ast_alloc(NODE_CONST, ps.previous.line);
    node->as.var_decl.is_const = true;
    node->as.var_decl.is_global = false;
    node->as.var_decl.extra_names = NULL;
    node->as.var_decl.extra_type_annotations = NULL;
    node->as.var_decl.type_annotation = NULL;
    node->as.var_decl.name_count = 1;

    consume(TOKEN_IDENTIFIER, "Expected constant name.");
    node->as.var_decl.name = ps.previous;

    if (match_token(TOKEN_COLON)) {
        node->as.var_decl.type_annotation = parse_type_ref();
    }

    consume(TOKEN_EQUAL, "Constants must be initialized.");
    node->as.var_decl.initializer = parse_expression();

    return node;
}

/* if <cond> then <body> [elseif <cond> then <body>]* [else <body>] end */
static ASTNode *parse_if_statement(void) {
    ASTNode *node = ast_alloc(NODE_IF, ps.previous.line);

    node->as.if_stmt.condition = parse_expression();
    consume(TOKEN_THEN, "Expected 'then' after if condition.");

    node->as.if_stmt.then_branch = parse_block();

    if (match_token(TOKEN_ELSEIF)) {
        node->as.if_stmt.else_branch = parse_if_statement();
    } else if (match_token(TOKEN_ELSE)) {
        node->as.if_stmt.else_branch = parse_block();
        consume(TOKEN_END, "Expected 'end' after else block.");
    } else {
        node->as.if_stmt.else_branch = NULL;
        consume(TOKEN_END, "Expected 'end' after if block.");
    }

    return node;
}

/* loop <body> end */
static ASTNode *parse_loop_statement(void) {
    ASTNode *node = ast_alloc(NODE_LOOP, ps.previous.line);
    node->as.loop_stmt.body = parse_block();
    consume(TOKEN_END, "Expected 'end' after loop body.");
    return node;
}

/* for <var> in <expr> ... end */
static ASTNode *parse_for_statement(void) {
    int line = ps.previous.line;

    consume(TOKEN_IDENTIFIER, "Expected variable name after 'for'.");
    Token var1 = ps.previous;

    Token var2 = {0};
    bool has_var2 = false;
    if (match_token(TOKEN_COMMA)) {
        consume(TOKEN_IDENTIFIER, "Expected second variable name.");
        var2 = ps.previous;
        has_var2 = true;
    }

    consume(TOKEN_IN, "Expected 'in' after for variable.");

    ASTNode *iterable = parse_expression();

    /* Check for range expression */
    if (iterable->type == NODE_BINARY &&
        (iterable->as.binary.op == TOKEN_DOT_DOT ||
         iterable->as.binary.op == TOKEN_DOT_DOT_EQUAL)) {
        if (has_var2) {
            error_at(&var2, "Range loops accept exactly one loop variable.");
        }
        ASTNode *node = ast_alloc(NODE_FOR_RANGE, line);
        node->as.for_range.var = var1;
        node->as.for_range.start = iterable->as.binary.left;
        node->as.for_range.end = iterable->as.binary.right;
        node->as.for_range.inclusive = (iterable->as.binary.op == TOKEN_DOT_DOT_EQUAL);
        free(iterable);

        node->as.for_range.body = parse_block();
        consume(TOKEN_END, "Expected 'end' after for body.");
        return node;
    }

    ASTNode *node = ast_alloc(NODE_FOR_IN, line);
    node->as.for_in.var = var1;
    node->as.for_in.var2 = var2;
    node->as.for_in.has_var2 = has_var2;
    node->as.for_in.iterable = iterable;
    node->as.for_in.body = parse_block();
    consume(TOKEN_END, "Expected 'end' after for body.");
    return node;
}

/* return [expr [, expr]*] */
static ASTNode *parse_return_statement(void) {
    ASTNode *node = ast_alloc(NODE_RETURN, ps.previous.line);
    node_list_init(&node->as.return_stmt.values);

    if (!check(TOKEN_END) && !check(TOKEN_ELSE) && !check(TOKEN_ELSEIF) &&
        !check(TOKEN_CATCH) &&
        !check(TOKEN_EOF)) {
        do {
            node_list_write(&node->as.return_stmt.values, parse_expression());
        } while (match_token(TOKEN_COMMA));
    }

    return node;
}

/* defer <expr> */
static ASTNode *parse_defer_statement(void) {
    ASTNode *node = ast_alloc(NODE_DEFER, ps.previous.line);
    node->as.defer_stmt.call = parse_expression();
    return node;
}

/* fn [Name[.MethodName]] (params) <body> end */
static ASTNode *parse_fn_declaration(void) {
    ASTNode *node = ast_alloc(NODE_FN_DECL, ps.previous.line);
    node->as.fn_decl.is_method = false;
    node->as.fn_decl.params = NULL;
    node->as.fn_decl.param_count = 0;
    node->as.fn_decl.return_type = NULL;

    if (check(TOKEN_IDENTIFIER)) {
        advance_token();
        node->as.fn_decl.name = ps.previous;

        if (match_token(TOKEN_DOT)) {
            node->as.fn_decl.method_struct = node->as.fn_decl.name;
            consume(TOKEN_IDENTIFIER, "Expected method name after '.'.");
            node->as.fn_decl.name = ps.previous;
            node->as.fn_decl.is_method = true;
        }
    } else {
        node->as.fn_decl.name.start = "";
        node->as.fn_decl.name.length = 0;
        node->as.fn_decl.name.line = ps.previous.line;
        node->as.fn_decl.name.type = TOKEN_IDENTIFIER;
    }

    consume(TOKEN_LEFT_PAREN, "Expected '(' after function name.");

    int param_capacity = 8;
    node->as.fn_decl.params = (Param *)malloc(sizeof(Param) * param_capacity);

    if (!check(TOKEN_RIGHT_PAREN)) {
        do {
            if (node->as.fn_decl.param_count >= param_capacity) {
                param_capacity *= 2;
                node->as.fn_decl.params = (Param *)realloc(
                    node->as.fn_decl.params, sizeof(Param) * param_capacity);
            }
            consume(TOKEN_IDENTIFIER, "Expected parameter name.");
            node->as.fn_decl.params[node->as.fn_decl.param_count].name = ps.previous;
            node->as.fn_decl.params[node->as.fn_decl.param_count].type = NULL;
            if (match_token(TOKEN_COLON)) {
                node->as.fn_decl.params[node->as.fn_decl.param_count].type = parse_type_ref();
            }
            node->as.fn_decl.param_count++;
        } while (match_token(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_PAREN, "Expected ')' after parameters.");

    if (match_token(TOKEN_COLON)) {
        node->as.fn_decl.return_type = parse_type_ref();
    } else {
        node->as.fn_decl.return_type = NULL;
    }

    node->as.fn_decl.body = parse_block();
    consume(TOKEN_END, "Expected 'end' after function body.");

    return node;
}

static ASTNode *parse_fn_literal(void) {
    return parse_fn_declaration();
}

/* struct Name <fields> end */
static ASTNode *parse_struct_declaration(void) {
    ASTNode *node = ast_alloc(NODE_STRUCT_DECL, ps.previous.line);

    consume(TOKEN_IDENTIFIER, "Expected struct name.");
    node->as.struct_decl.name = ps.previous;

    int field_capacity = 8;
    node->as.struct_decl.fields = (Param *)malloc(sizeof(Param) * field_capacity);
    node->as.struct_decl.field_count = 0;

    while (!check(TOKEN_END) && !check(TOKEN_EOF)) {
        if (node->as.struct_decl.field_count >= field_capacity) {
            field_capacity *= 2;
            node->as.struct_decl.fields = (Param *)realloc(
                node->as.struct_decl.fields, sizeof(Param) * field_capacity);
        }

        consume(TOKEN_IDENTIFIER, "Expected field name in struct.");
        node->as.struct_decl.fields[node->as.struct_decl.field_count].name = ps.previous;
        node->as.struct_decl.fields[node->as.struct_decl.field_count].type = NULL;
        if (match_token(TOKEN_COLON)) {
            node->as.struct_decl.fields[node->as.struct_decl.field_count].type = parse_type_ref();
        }
        node->as.struct_decl.field_count++;
    }

    consume(TOKEN_END, "Expected 'end' after struct declaration.");
    return node;
}

/* enum Name { Variant1, Variant2, ... } */
static ASTNode *parse_enum_declaration(void) {
    ASTNode *node = ast_alloc(NODE_ENUM_DECL, ps.previous.line);

    consume(TOKEN_IDENTIFIER, "Expected enum name.");
    node->as.enum_decl.name = ps.previous;

    consume(TOKEN_LEFT_BRACE, "Expected '{' after enum name.");

    int var_capacity = 8;
    node->as.enum_decl.variants = (Token *)malloc(sizeof(Token) * var_capacity);
    node->as.enum_decl.variant_count = 0;

    if (!check(TOKEN_RIGHT_BRACE)) {
        do {
            if (node->as.enum_decl.variant_count >= var_capacity) {
                var_capacity *= 2;
                node->as.enum_decl.variants = (Token *)realloc(
                    node->as.enum_decl.variants, sizeof(Token) * var_capacity);
            }
            consume(TOKEN_IDENTIFIER, "Expected variant name.");
            node->as.enum_decl.variants[node->as.enum_decl.variant_count++] = ps.previous;
        } while (match_token(TOKEN_COMMA));
    }

    consume(TOKEN_RIGHT_BRACE, "Expected '}' after enum variants.");
    return node;
}

/* import "path" [as alias] */
static ASTNode *parse_import(void) {
    ASTNode *node = ast_alloc(NODE_IMPORT, ps.previous.line);
    consume(TOKEN_STRING, "Expected module path string after 'import'.");
    node->as.import_stmt.path = ps.previous;
    node->as.import_stmt.has_alias = false;
    if (match_token(TOKEN_AS)) {
        consume(TOKEN_IDENTIFIER, "Expected alias name after 'as'.");
        node->as.import_stmt.alias = ps.previous;
        node->as.import_stmt.has_alias = true;
    }
    return node;
}

/* export <declaration> */
static ASTNode *parse_export(void) {
    ASTNode *node = ast_alloc(NODE_EXPORT, ps.previous.line);
    node->as.export_stmt.declaration = parse_declaration();
    return node;
}

static ASTNode *parse_directive(void) {
    int line = ps.previous.line; /* '!' already consumed */
    consume(TOKEN_IDENTIFIER, "Expected directive name after '!'.");
    ASTNode *node = ast_alloc(NODE_DIRECTIVE, line);
    node->as.directive.name = ps.previous;
    if (token_matches(&ps.previous, "strict")) {
        node->as.directive.kind = MG_DIRECTIVE_STRICT;
    } else if (token_matches(&ps.previous, "nocheck")) {
        node->as.directive.kind = MG_DIRECTIVE_NOCHECK;
    } else {
        error("Unknown directive. Expected '!strict' or '!nocheck'.");
        node->as.directive.kind = MG_DIRECTIVE_NOCHECK;
    }
    return node;
}

static ASTNode *parse_type_alias(void) {
    consume(TOKEN_IDENTIFIER, "Expected 'type'.");
    int line = ps.previous.line;
    ASTNode *node = ast_alloc(NODE_TYPE_ALIAS, line);
    consume(TOKEN_IDENTIFIER, "Expected type alias name.");
    node->as.type_alias.name = ps.previous;
    consume(TOKEN_EQUAL, "Expected '=' after type alias name.");
    node->as.type_alias.type = parse_type_ref();
    return node;
}

static ASTNode *parse_extern_declaration(void) {
    consume(TOKEN_IDENTIFIER, "Expected 'extern'.");
    int line = ps.previous.line;
    ASTNode *node = ast_alloc(NODE_EXTERN_DECL, line);
    node->as.extern_decl.params = NULL;
    node->as.extern_decl.param_count = 0;
    node->as.extern_decl.type = NULL;

    if (match_token(TOKEN_FN)) {
        node->as.extern_decl.is_function = true;
        consume(TOKEN_IDENTIFIER, "Expected extern function name.");
        node->as.extern_decl.name = ps.previous;
        consume(TOKEN_LEFT_PAREN, "Expected '(' after extern function name.");

        int param_capacity = 0;
        if (!check(TOKEN_RIGHT_PAREN)) {
            do {
                if (node->as.extern_decl.param_count >= param_capacity) {
                    param_capacity = param_capacity < 4 ? 4 : param_capacity * 2;
                    node->as.extern_decl.params = (Param *)realloc(
                        node->as.extern_decl.params, sizeof(Param) * param_capacity);
                }
                consume(TOKEN_IDENTIFIER, "Expected extern parameter name.");
                node->as.extern_decl.params[node->as.extern_decl.param_count].name = ps.previous;
                node->as.extern_decl.params[node->as.extern_decl.param_count].type = NULL;
                consume(TOKEN_COLON, "Expected ':' after extern parameter name.");
                node->as.extern_decl.params[node->as.extern_decl.param_count].type = parse_type_ref();
                node->as.extern_decl.param_count++;
            } while (match_token(TOKEN_COMMA));
        }

        consume(TOKEN_RIGHT_PAREN, "Expected ')' after extern function parameters.");
        consume(TOKEN_COLON, "Expected ':' before extern function return type.");
        node->as.extern_decl.type = parse_type_ref();
        return node;
    }

    if (match_token(TOKEN_CONST)) {
        node->as.extern_decl.is_function = false;
        consume(TOKEN_IDENTIFIER, "Expected extern const name.");
        node->as.extern_decl.name = ps.previous;
        consume(TOKEN_COLON, "Expected ':' after extern const name.");
        node->as.extern_decl.type = parse_type_ref();
        return node;
    }

    error_current("Expected 'fn' or 'const' after 'extern'.");
    node->as.extern_decl.is_function = false;
    node->as.extern_decl.name = ps.current;
    return node;
}

static bool starts_directive(void) {
    if (!check(TOKEN_BANG)) return false;
    Token next = peek_next_token();
    return next.type == TOKEN_IDENTIFIER &&
           (token_matches(&next, "strict") || token_matches(&next, "nocheck"));
}

static bool starts_type_alias(void) {
    if (!check_identifier_text("type")) return false;
    Token next = peek_next_token();
    return next.type == TOKEN_IDENTIFIER;
}

static bool starts_extern_declaration(void) {
    if (!check_identifier_text("extern")) return false;
    Token next = peek_next_token();
    return next.type == TOKEN_FN || next.type == TOKEN_CONST;
}

/* ========================================================================
 * Top-level: declaration / statement
 * ======================================================================== */
static ASTNode *parse_declaration(void) {
    if (starts_directive()) {
        advance_token(); /* consume '!' */
        return parse_directive();
    }
    if (starts_type_alias()) return parse_type_alias();
    if (starts_extern_declaration()) return parse_extern_declaration();
    if (match_token(TOKEN_LET)) return parse_let_declaration();
    if (match_token(TOKEN_CONST)) return parse_const_declaration();
    if (match_token(TOKEN_FN)) return parse_fn_declaration();
    if (match_token(TOKEN_STRUCT)) return parse_struct_declaration();
    if (match_token(TOKEN_ENUM)) return parse_enum_declaration();
    if (match_token(TOKEN_IMPORT)) return parse_import();
    if (match_token(TOKEN_EXPORT)) return parse_export();

    return parse_statement();
}

static ASTNode *parse_statement(void) {
    if (match_token(TOKEN_IF)) return parse_if_statement();
    if (match_token(TOKEN_LOOP)) return parse_loop_statement();
    if (match_token(TOKEN_FOR)) return parse_for_statement();
    if (match_token(TOKEN_RETURN)) return parse_return_statement();
    if (match_token(TOKEN_DEFER)) return parse_defer_statement();
    if (match_token(TOKEN_TRY)) return parse_try_statement();
    if (match_token(TOKEN_BREAK)) return ast_alloc(NODE_BREAK, ps.previous.line);
    if (match_token(TOKEN_CONTINUE)) return ast_alloc(NODE_CONTINUE, ps.previous.line);

    ASTNode *expr = parse_expression_or_assignment();
    ASTNode *stmt = ast_alloc(NODE_EXPRESSION_STMT, expr->line);
    stmt->as.expr_stmt.expr = expr;
    return stmt;
}

/* ========================================================================
 * Entry Point
 * ======================================================================== */
ASTNode *parse(const char *source, bool *had_error) {
    scanner_init(&ps.scanner, source);
    ps.had_error = false;
    ps.panic_mode = false;

    advance_token();

    ASTNode *program = ast_alloc(NODE_BLOCK, 1);
    node_list_init(&program->as.block.stmts);

    while (!check(TOKEN_EOF)) {
        ASTNode *decl = parse_declaration();
        if (decl) {
            node_list_write(&program->as.block.stmts, decl);
        }
        if (ps.panic_mode) synchronize();
    }

    *had_error = ps.had_error;
    return program;
}
