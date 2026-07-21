/*
 * ast.c — VUS 抽象语法树节点创建函数实现
 *
 * 提供 ast.h 中声明的所有 AST 节点创建、释放和操作函数。
 */

#define _POSIX_C_SOURCE 200809L

#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============ VusAstList 操作 ============ */

VusAstList *vus_ast_list_new(void) {
    VusAstList *list = calloc(1, sizeof(VusAstList));
    if (!list) return NULL;
    list->capacity = 8;
    list->items = calloc(list->capacity, sizeof(VusAstNode *));
    list->count = 0;
    return list;
}

void vus_ast_list_push(VusAstList *list, VusAstNode *node) {
    if (!list || !node) return;
    if (list->count >= list->capacity) {
        list->capacity *= 2;
        list->items = realloc(list->items, list->capacity * sizeof(VusAstNode *));
    }
    list->items[list->count++] = node;
}

void vus_ast_list_free(VusAstList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        vus_ast_node_free(list->items[i]);
    }
    free(list->items);
    free(list);
}

/* ============ 节点创建函数 ============ */

VusAstProgram *vus_ast_program_new(VusAstList *stmts) {
    VusAstProgram *node = calloc(1, sizeof(VusAstProgram));
    if (!node) return NULL;
    node->type = VUS_AST_PROGRAM;
    node->statements = stmts ? stmts : vus_ast_list_new();
    return node;
}

VusAstFunctionDef *vus_ast_func_def_new(const char *name, VusAstList *type_params, VusAstList *params, VusAstList *body, int line, int col) {
    VusAstFunctionDef *node = calloc(1, sizeof(VusAstFunctionDef));
    if (!node) return NULL;
    node->type = VUS_AST_FUNCTION_DEF;
    node->line = line;
    node->column = col;
    node->name = name ? strdup(name) : NULL;
    node->type_params = type_params;
    node->params = params ? params : vus_ast_list_new();
    node->body = body ? body : vus_ast_list_new();
    return node;
}

VusAstParam *vus_ast_param_new(const char *name, const char *type_ann, int line, int col) {
    VusAstParam *node = calloc(1, sizeof(VusAstParam));
    if (!node) return NULL;
    node->type = VUS_AST_PARAM;
    node->line = line;
    node->column = col;
    node->name = name ? strdup(name) : NULL;
    node->type_annotation = type_ann ? strdup(type_ann) : NULL;
    return node;
}

VusAstParamDefault *vus_ast_param_default_new(const char *name, const char *type_ann, VusAstNode *default_val, int line, int col) {
    VusAstParamDefault *node = calloc(1, sizeof(VusAstParamDefault));
    if (!node) return NULL;
    node->type = VUS_AST_PARAM_DEFAULT;
    node->line = line;
    node->column = col;
    node->name = name ? strdup(name) : NULL;
    node->type_annotation = type_ann ? strdup(type_ann) : NULL;
    node->default_value = default_val;
    return node;
}

VusAstIf *vus_ast_if_new(VusAstNode *cond, VusAstList *then_body, int line, int col) {
    VusAstIf *node = calloc(1, sizeof(VusAstIf));
    if (!node) return NULL;
    node->type = VUS_AST_IF;
    node->line = line;
    node->column = col;
    node->condition = cond;
    node->then_body = then_body ? then_body : vus_ast_list_new();
    node->elif_conditions = vus_ast_list_new();
    node->elif_bodies = vus_ast_list_new();
    node->else_body = NULL;
    return node;
}

void vus_ast_if_add_elif(VusAstIf *if_node, VusAstNode *cond, VusAstList *body) {
    if (!if_node) return;
    vus_ast_list_push(if_node->elif_conditions, cond);
    VusAstList *body_wrapper = vus_ast_list_new();
    if (body) {
        for (size_t i = 0; i < body->count; i++) {
            vus_ast_list_push(body_wrapper, body->items[i]);
        }
        body->count = 0;
        vus_ast_list_free(body);
    }
    vus_ast_list_push(if_node->elif_bodies, (VusAstNode *)body_wrapper);
}

void vus_ast_if_set_else(VusAstIf *if_node, VusAstList *body) {
    if (!if_node) return;
    if_node->else_body = body;
}

VusAstForRange *vus_ast_for_range_new(const char *var, VusAstNode *start, VusAstNode *end, VusAstList *body, int line, int col) {
    VusAstForRange *node = calloc(1, sizeof(VusAstForRange));
    if (!node) return NULL;
    node->type = VUS_AST_FOR_RANGE;
    node->line = line;
    node->column = col;
    node->var_name = var ? strdup(var) : NULL;
    node->start = start;
    node->end = end;
    node->body = body ? body : vus_ast_list_new();
    return node;
}

VusAstForEach *vus_ast_for_each_new(const char *var, VusAstNode *iter, VusAstList *body, int line, int col) {
    VusAstForEach *node = calloc(1, sizeof(VusAstForEach));
    if (!node) return NULL;
    node->type = VUS_AST_FOR_EACH;
    node->line = line;
    node->column = col;
    node->var_name = var ? strdup(var) : NULL;
    node->iterable = iter;
    node->body = body ? body : vus_ast_list_new();
    return node;
}

VusAstWhile *vus_ast_while_new(VusAstNode *cond, VusAstList *body, int line, int col) {
    VusAstWhile *node = calloc(1, sizeof(VusAstWhile));
    if (!node) return NULL;
    node->type = VUS_AST_WHILE;
    node->line = line;
    node->column = col;
    node->condition = cond;
    node->body = body ? body : vus_ast_list_new();
    return node;
}

VusAstTry *vus_ast_try_new(VusAstList *try_body, int line, int col) {
    VusAstTry *node = calloc(1, sizeof(VusAstTry));
    if (!node) return NULL;
    node->type = VUS_AST_TRY;
    node->line = line;
    node->column = col;
    node->try_body = try_body ? try_body : vus_ast_list_new();
    node->except_types = vus_ast_list_new();
    node->except_bodies = vus_ast_list_new();
    return node;
}

void vus_ast_try_add_except(VusAstTry *try_node, const char *type, VusAstList *body) {
    if (!try_node) return;
    VusAstIdentifier *type_node = vus_ast_ident_new(type ? type : "", 0, 0);
    vus_ast_list_push(try_node->except_types, (VusAstNode *)type_node);
    vus_ast_list_push(try_node->except_bodies, (VusAstNode *)body);
}

VusAstReturn *vus_ast_return_new(VusAstNode *val, int line, int col) {
    VusAstReturn *node = calloc(1, sizeof(VusAstReturn));
    if (!node) return NULL;
    node->type = VUS_AST_RETURN;
    node->line = line;
    node->column = col;
    node->value = val;
    return node;
}

VusAstAssign *vus_ast_assign_new(const char *target, const char *type_ann, VusAstNode *val, int line, int col) {
    VusAstAssign *node = calloc(1, sizeof(VusAstAssign));
    if (!node) return NULL;
    node->type = VUS_AST_ASSIGN;
    node->line = line;
    node->column = col;
    node->target = target ? strdup(target) : NULL;
    node->type_annotation = type_ann ? strdup(type_ann) : NULL;
    node->value = val;
    return node;
}

VusAstExprStmt *vus_ast_expr_stmt_new(VusAstNode *expr, int line, int col) {
    VusAstExprStmt *node = calloc(1, sizeof(VusAstExprStmt));
    if (!node) return NULL;
    node->type = VUS_AST_EXPR_STMT;
    node->line = line;
    node->column = col;
    node->expr = expr;
    return node;
}

VusAstBinaryOp *vus_ast_binary_new(const char *op, VusAstNode *left, VusAstNode *right, int line, int col) {
    VusAstBinaryOp *node = calloc(1, sizeof(VusAstBinaryOp));
    if (!node) return NULL;
    node->type = VUS_AST_BINARY_OP;
    node->line = line;
    node->column = col;
    node->op = op ? strdup(op) : NULL;
    node->left = left;
    node->right = right;
    return node;
}

VusAstUnaryOp *vus_ast_unary_new(const char *op, VusAstNode *operand, int line, int col) {
    VusAstUnaryOp *node = calloc(1, sizeof(VusAstUnaryOp));
    if (!node) return NULL;
    node->type = VUS_AST_UNARY_OP;
    node->line = line;
    node->column = col;
    node->op = op ? strdup(op) : NULL;
    node->operand = operand;
    return node;
}

VusAstCall *vus_ast_call_new(const char *func, VusAstList *args, VusAstList *type_args, int line, int col) {
    VusAstCall *node = calloc(1, sizeof(VusAstCall));
    if (!node) return NULL;
    node->type = VUS_AST_CALL;
    node->line = line;
    node->column = col;
    node->func_name = func ? strdup(func) : NULL;
    node->args = args ? args : vus_ast_list_new();
    node->type_args = type_args;
    return node;
}

VusAstIdentifier *vus_ast_ident_new(const char *name, int line, int col) {
    VusAstIdentifier *node = calloc(1, sizeof(VusAstIdentifier));
    if (!node) return NULL;
    node->type = VUS_AST_IDENTIFIER;
    node->line = line;
    node->column = col;
    node->name = name ? strdup(name) : NULL;
    return node;
}

VusAstString *vus_ast_string_new(const char *val, int line, int col) {
    VusAstString *node = calloc(1, sizeof(VusAstString));
    if (!node) return NULL;
    node->type = VUS_AST_STRING_LITERAL;
    node->line = line;
    node->column = col;
    node->value = val ? strdup(val) : NULL;
    return node;
}

VusAstNumber *vus_ast_number_new(const char *val, int is_float, int line, int col) {
    VusAstNumber *node = calloc(1, sizeof(VusAstNumber));
    if (!node) return NULL;
    node->type = VUS_AST_NUMBER_LITERAL;
    node->line = line;
    node->column = col;
    node->value = val ? strdup(val) : NULL;
    node->is_float = is_float;
    return node;
}

VusAstBool *vus_ast_bool_new(int val, int line, int col) {
    VusAstBool *node = calloc(1, sizeof(VusAstBool));
    if (!node) return NULL;
    node->type = VUS_AST_BOOL_LITERAL;
    node->line = line;
    node->column = col;
    node->value = val;
    return node;
}

VusAstNull *vus_ast_null_new(int line, int col) {
    VusAstNull *node = calloc(1, sizeof(VusAstNull));
    if (!node) return NULL;
    node->type = VUS_AST_NULL_LITERAL;
    node->line = line;
    node->column = col;
    return node;
}

VusAstBreak *vus_ast_break_new(int line, int col) {
    VusAstBreak *node = calloc(1, sizeof(VusAstBreak));
    if (!node) return NULL;
    node->type = VUS_AST_BREAK;
    node->line = line;
    node->column = col;
    return node;
}

VusAstThrow *vus_ast_throw_new(VusAstNode *val, int line, int col) {
    VusAstThrow *node = calloc(1, sizeof(VusAstThrow));
    if (!node) return NULL;
    node->type = VUS_AST_THROW;
    node->line = line;
    node->column = col;
    node->value = val;
    return node;
}

VusAstGlobalDecl *vus_ast_global_new(const char *name, int line, int col) {
    VusAstGlobalDecl *node = calloc(1, sizeof(VusAstGlobalDecl));
    if (!node) return NULL;
    node->type = VUS_AST_GLOBAL_DECL;
    node->line = line;
    node->column = col;
    node->name = name ? strdup(name) : NULL;
    return node;
}

VusAstStructDef *vus_ast_struct_def_new(const char *name, VusAstList *fields, int line, int col) {
    VusAstStructDef *node = calloc(1, sizeof(VusAstStructDef));
    if (!node) return NULL;
    node->type = VUS_AST_STRUCT_DEF;
    node->line = line;
    node->column = col;
    node->name = name ? strdup(name) : NULL;
    node->fields = fields ? fields : vus_ast_list_new();
    return node;
}

VusAstStructInst *vus_ast_struct_inst_new(const char *name, VusAstList *args, int line, int col) {
    VusAstStructInst *node = calloc(1, sizeof(VusAstStructInst));
    if (!node) return NULL;
    node->type = VUS_AST_STRUCT_INSTANTIATE;
    node->line = line;
    node->column = col;
    node->struct_name = name ? strdup(name) : NULL;
    node->args = args ? args : vus_ast_list_new();
    return node;
}

VusAstAccess *vus_ast_access_new(VusAstNode *obj, const char *member, int line, int col) {
    VusAstAccess *node = calloc(1, sizeof(VusAstAccess));
    if (!node) return NULL;
    node->type = VUS_AST_ACCESS;
    node->line = line;
    node->column = col;
    node->object = obj;
    node->member = member ? strdup(member) : NULL;
    node->is_optional = 0;
    return node;
}

/* ============ AST 节点释放 ============ */

void vus_ast_node_free(VusAstNode *node) {
    if (!node) return;

    switch (node->type) {
    case VUS_AST_PROGRAM: {
        VusAstProgram *n = (VusAstProgram *)node;
        vus_ast_list_free(n->statements);
        break;
    }
    case VUS_AST_FUNCTION_DEF: {
        VusAstFunctionDef *n = (VusAstFunctionDef *)node;
        free(n->name);
        vus_ast_list_free(n->type_params);
        vus_ast_list_free(n->params);
        vus_ast_list_free(n->body);
        break;
    }
    case VUS_AST_PARAM: {
        VusAstParam *n = (VusAstParam *)node;
        free(n->name);
        free(n->type_annotation);
        break;
    }
    case VUS_AST_PARAM_DEFAULT: {
        VusAstParamDefault *n = (VusAstParamDefault *)node;
        free(n->name);
        free(n->type_annotation);
        vus_ast_node_free(n->default_value);
        break;
    }
    case VUS_AST_IF: {
        VusAstIf *n = (VusAstIf *)node;
        vus_ast_node_free(n->condition);
        vus_ast_list_free(n->then_body);
        vus_ast_list_free(n->elif_conditions);
        vus_ast_list_free(n->elif_bodies);
        vus_ast_list_free(n->else_body);
        break;
    }
    case VUS_AST_FOR_RANGE: {
        VusAstForRange *n = (VusAstForRange *)node;
        free(n->var_name);
        vus_ast_node_free(n->start);
        vus_ast_node_free(n->end);
        vus_ast_list_free(n->body);
        break;
    }
    case VUS_AST_FOR_EACH: {
        VusAstForEach *n = (VusAstForEach *)node;
        free(n->var_name);
        vus_ast_node_free(n->iterable);
        vus_ast_list_free(n->body);
        break;
    }
    case VUS_AST_WHILE: {
        VusAstWhile *n = (VusAstWhile *)node;
        vus_ast_node_free(n->condition);
        vus_ast_list_free(n->body);
        break;
    }
    case VUS_AST_TRY: {
        VusAstTry *n = (VusAstTry *)node;
        vus_ast_list_free(n->try_body);
        vus_ast_list_free(n->except_types);
        vus_ast_list_free(n->except_bodies);
        break;
    }
    case VUS_AST_RETURN: {
        VusAstReturn *n = (VusAstReturn *)node;
        vus_ast_node_free(n->value);
        break;
    }
    case VUS_AST_ASSIGN: {
        VusAstAssign *n = (VusAstAssign *)node;
        free(n->target);
        free(n->type_annotation);
        vus_ast_node_free(n->value);
        break;
    }
    case VUS_AST_EXPR_STMT: {
        VusAstExprStmt *n = (VusAstExprStmt *)node;
        vus_ast_node_free(n->expr);
        break;
    }
    case VUS_AST_BINARY_OP: {
        VusAstBinaryOp *n = (VusAstBinaryOp *)node;
        free(n->op);
        vus_ast_node_free(n->left);
        vus_ast_node_free(n->right);
        break;
    }
    case VUS_AST_UNARY_OP: {
        VusAstUnaryOp *n = (VusAstUnaryOp *)node;
        free(n->op);
        vus_ast_node_free(n->operand);
        break;
    }
    case VUS_AST_CALL: {
        VusAstCall *n = (VusAstCall *)node;
        free(n->func_name);
        vus_ast_list_free(n->args);
        vus_ast_list_free(n->type_args);
        break;
    }
    case VUS_AST_IDENTIFIER: {
        VusAstIdentifier *n = (VusAstIdentifier *)node;
        free(n->name);
        break;
    }
    case VUS_AST_STRING_LITERAL: {
        VusAstString *n = (VusAstString *)node;
        free(n->value);
        break;
    }
    case VUS_AST_NUMBER_LITERAL: {
        VusAstNumber *n = (VusAstNumber *)node;
        free(n->value);
        break;
    }
    case VUS_AST_BOOL_LITERAL:
    case VUS_AST_NULL_LITERAL:
    case VUS_AST_BREAK:
    case VUS_AST_CONTINUE:
        break;
    case VUS_AST_THROW: {
        VusAstThrow *n = (VusAstThrow *)node;
        vus_ast_node_free(n->value);
        break;
    }
    case VUS_AST_GLOBAL_DECL: {
        VusAstGlobalDecl *n = (VusAstGlobalDecl *)node;
        free(n->name);
        break;
    }
    case VUS_AST_STRUCT_DEF: {
        VusAstStructDef *n = (VusAstStructDef *)node;
        free(n->name);
        vus_ast_list_free(n->fields);
        break;
    }
    case VUS_AST_STRUCT_INSTANTIATE: {
        VusAstStructInst *n = (VusAstStructInst *)node;
        free(n->struct_name);
        vus_ast_list_free(n->args);
        break;
    }
    case VUS_AST_ACCESS: {
        VusAstAccess *n = (VusAstAccess *)node;
        vus_ast_node_free(n->object);
        free(n->member);
        break;
    }
    case VUS_AST_IMPORT:
    case VUS_AST_FROM_IMPORT:
        /* 由 parser 内部管理，直接释放节点本身 */
        break;
    default:
        break;
    }
    free(node);
}