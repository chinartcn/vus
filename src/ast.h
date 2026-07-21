/*
 * ast.h — VUS 抽象语法树节点定义
 *
 * 定义所有 AST 节点类型和操作函数。
 * 采用 tagged union 设计，每个节点以 VusAstNode 作为通用头。
 */

#ifndef VUS_AST_H
#define VUS_AST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============ AST 节点类型枚举 ============ */
typedef enum {
    VUS_AST_PROGRAM,
    VUS_AST_FUNCTION_DEF,
    VUS_AST_IF,
    VUS_AST_FOR_RANGE,
    VUS_AST_FOR_EACH,
    VUS_AST_WHILE,
    VUS_AST_TRY,
    VUS_AST_IMPORT,
    VUS_AST_FROM_IMPORT,
    VUS_AST_RETURN,
    VUS_AST_BREAK,
    VUS_AST_CONTINUE,
    VUS_AST_GLOBAL_DECL,
    VUS_AST_ASSIGN,
    VUS_AST_EXPR_STMT,
    VUS_AST_BINARY_OP,
    VUS_AST_UNARY_OP,
    VUS_AST_CALL,
    VUS_AST_IDENTIFIER,
    VUS_AST_STRING_LITERAL,
    VUS_AST_NUMBER_LITERAL,
    VUS_AST_BOOL_LITERAL,
    VUS_AST_NULL_LITERAL,
    VUS_AST_LIST_LITERAL,
    VUS_AST_DICT_LITERAL,
    VUS_AST_PARAM,
    VUS_AST_PARAM_DEFAULT,
    VUS_AST_THROW,
    VUS_AST_STRUCT_DEF,
    VUS_AST_STRUCT_INSTANTIATE,
    VUS_AST_ACCESS,
} VusAstNodeType;

/* ============ 前向声明 ============ */
typedef struct VusAstNode VusAstNode;
typedef struct VusAstList VusAstList;

/* ============ 通用节点头 ============ */
struct VusAstNode {
    VusAstNodeType type;
    int            line;
    int            column;
};

/* ============ 动态数组 — 用于子节点列表 ============ */
struct VusAstList {
    VusAstNode **items;
    size_t       count;
    size_t       capacity;
};

/* 创建 AST 列表 */
VusAstList *vus_ast_list_new(void);

/* 添加节点到列表 */
void vus_ast_list_push(VusAstList *list, VusAstNode *node);

/* 释放 AST 列表 */
void vus_ast_list_free(VusAstList *list);

/* ============ 具体节点类型 ============ */

/* Program — 程序根节点 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_PROGRAM */
    int            line;
    int            column;
    VusAstList    *statements;
} VusAstProgram;

/* FunctionDef — 函数定义 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_FUNCTION_DEF */
    int            line;
    int            column;
    char          *name;
    VusAstList    *type_params; /* 泛型类型参数列表（Param 节点），可为 NULL */
    VusAstList    *params;      /* Param 或 ParamDefault 节点 */
    VusAstList    *body;        /* 语句列表 */
} VusAstFunctionDef;

/* Param — 参数 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_PARAM */
    int            line;
    int            column;
    char          *name;
    char          *type_annotation;
} VusAstParam;

/* ParamDefault — 带默认值的参数 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_PARAM_DEFAULT */
    int            line;
    int            column;
    char          *name;
    char          *type_annotation;
    VusAstNode    *default_value;
} VusAstParamDefault;

/* IfStmt — 条件语句 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_IF */
    int            line;
    int            column;
    VusAstNode    *condition;
    VusAstList    *then_body;
    /* elif_clauses 和 else_body 通过扩展字段实现 */
    VusAstList    *elif_conditions;  /* 条件列表 */
    VusAstList    *elif_bodies;      /* 对应体列表 */
    VusAstList    *else_body;
} VusAstIf;

/* ForRangeStmt — 数值 for 循环 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_FOR_RANGE */
    int            line;
    int            column;
    char          *var_name;
    VusAstNode    *start;
    VusAstNode    *end;
    VusAstList    *body;
} VusAstForRange;

/* ForEachStmt — 遍历循环 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_FOR_EACH */
    int            line;
    int            column;
    char          *var_name;
    VusAstNode    *iterable;
    VusAstList    *body;
} VusAstForEach;

/* WhileStmt — while 循环 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_WHILE */
    int            line;
    int            column;
    VusAstNode    *condition;
    VusAstList    *body;
} VusAstWhile;

/* TryStmt — 异常处理 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_TRY */
    int            line;
    int            column;
    VusAstList    *try_body;
    VusAstList    *except_types;  /* 异常类型名列表，NULL 表示通配 */
    VusAstList    *except_bodies;
} VusAstTry;

/* ReturnStmt — 返回语句 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_RETURN */
    int            line;
    int            column;
    VusAstNode    *value;       /* 可为 NULL */
} VusAstReturn;

/* Assign — 赋值语句 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_ASSIGN */
    int            line;
    int            column;
    char          *target;
    char          *type_annotation;  /* 可为 NULL */
    VusAstNode    *value;
} VusAstAssign;

/* ExprStmt — 表达式语句 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_EXPR_STMT */
    int            line;
    int            column;
    VusAstNode    *expr;
} VusAstExprStmt;

/* BinaryOp — 二元运算 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_BINARY_OP */
    int            line;
    int            column;
    char          *op;          /* 运算符字符串 */
    VusAstNode    *left;
    VusAstNode    *right;
} VusAstBinaryOp;

/* UnaryOp — 一元运算 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_UNARY_OP */
    int            line;
    int            column;
    char          *op;
    VusAstNode    *operand;
} VusAstUnaryOp;

/* Call — 函数调用 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_CALL */
    int            line;
    int            column;
    char          *func_name;
    VusAstList    *args;
    VusAstList    *type_args; /* 泛型类型参数列表（Param 节点），可为 NULL */
} VusAstCall;

/* Identifier — 标识符引用 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_IDENTIFIER */
    int            line;
    int            column;
    char          *name;
} VusAstIdentifier;

/* StringLiteral — 字符串字面量 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_STRING_LITERAL */
    int            line;
    int            column;
    char          *value;
} VusAstString;

/* NumberLiteral — 数字字面量 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_NUMBER_LITERAL */
    int            line;
    int            column;
    char          *value;       /* 字符串形式 */
    int            is_float;
} VusAstNumber;

/* BooleanLiteral — 布尔字面量 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_BOOL_LITERAL */
    int            line;
    int            column;
    int            value;       /* 0 或 1 */
} VusAstBool;

/* 其他简单节点类型 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_NULL_LITERAL */
    int            line;
    int            column;
} VusAstNull;

typedef struct {
    VusAstNodeType type;   /* VUS_AST_BREAK / CONTINUE */
    int            line;
    int            column;
} VusAstBreak;

typedef struct {
    VusAstNodeType type;   /* VUS_AST_THROW */
    int            line;
    int            column;
    VusAstNode    *value;
} VusAstThrow;

typedef struct {
    VusAstNodeType type;   /* VUS_AST_GLOBAL_DECL */
    int            line;
    int            column;
    char          *name;
} VusAstGlobalDecl;

/* ============ 结构体相关节点类型 ============ */

/* StructDef — 结构体定义 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_STRUCT_DEF */
    int            line;
    int            column;
    char          *name;
    VusAstList    *fields;      /* Param 节点列表 */
} VusAstStructDef;

/* StructInstantiate — 结构体实例化 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_STRUCT_INSTANTIATE */
    int            line;
    int            column;
    char          *struct_name;
    VusAstList    *args;        /* 初始化参数 */
} VusAstStructInst;

/* Access — 成员访问 */
typedef struct {
    VusAstNodeType type;   /* VUS_AST_ACCESS */
    int            line;
    int            column;
    VusAstNode    *object;      /* 点号左侧表达式 */
    char          *member;      /* 成员名 */
    int            is_optional; /* 0=普通访问, 1=可选链 */
} VusAstAccess;

/* ============ AST 节点创建函数 ============ */

VusAstProgram    *vus_ast_program_new(VusAstList *stmts);
VusAstFunctionDef *vus_ast_func_def_new(const char *name, VusAstList *type_params, VusAstList *params, VusAstList *body, int line, int col);
VusAstParam      *vus_ast_param_new(const char *name, const char *type_ann, int line, int col);
VusAstParamDefault *vus_ast_param_default_new(const char *name, const char *type_ann, VusAstNode *default_val, int line, int col);
VusAstIf         *vus_ast_if_new(VusAstNode *cond, VusAstList *then_body, int line, int col);
VusAstForRange   *vus_ast_for_range_new(const char *var, VusAstNode *start, VusAstNode *end, VusAstList *body, int line, int col);
VusAstForEach    *vus_ast_for_each_new(const char *var, VusAstNode *iter, VusAstList *body, int line, int col);
VusAstWhile      *vus_ast_while_new(VusAstNode *cond, VusAstList *body, int line, int col);
VusAstTry        *vus_ast_try_new(VusAstList *try_body, int line, int col);
VusAstReturn     *vus_ast_return_new(VusAstNode *val, int line, int col);
VusAstAssign     *vus_ast_assign_new(const char *target, const char *type_ann, VusAstNode *val, int line, int col);
VusAstExprStmt   *vus_ast_expr_stmt_new(VusAstNode *expr, int line, int col);
VusAstBinaryOp   *vus_ast_binary_new(const char *op, VusAstNode *left, VusAstNode *right, int line, int col);
VusAstUnaryOp    *vus_ast_unary_new(const char *op, VusAstNode *operand, int line, int col);
VusAstCall       *vus_ast_call_new(const char *func, VusAstList *args, VusAstList *type_args, int line, int col);
VusAstIdentifier *vus_ast_ident_new(const char *name, int line, int col);
VusAstString     *vus_ast_string_new(const char *val, int line, int col);
VusAstNumber     *vus_ast_number_new(const char *val, int is_float, int line, int col);
VusAstBool       *vus_ast_bool_new(int val, int line, int col);
VusAstNull       *vus_ast_null_new(int line, int col);
VusAstBreak      *vus_ast_break_new(int line, int col);
VusAstThrow      *vus_ast_throw_new(VusAstNode *val, int line, int col);
VusAstGlobalDecl *vus_ast_global_new(const char *name, int line, int col);
VusAstStructDef  *vus_ast_struct_def_new(const char *name, VusAstList *fields, int line, int col);
VusAstStructInst *vus_ast_struct_inst_new(const char *name, VusAstList *args, int line, int col);
VusAstAccess     *vus_ast_access_new(VusAstNode *obj, const char *member, int line, int col);

/* 释放整个 AST 树 */
void vus_ast_node_free(VusAstNode *node);

/* 添加 elif 子句到 If 节点 */
void vus_ast_if_add_elif(VusAstIf *if_node, VusAstNode *cond, VusAstList *body);

/* 设置 If 节点的 else 体 */
void vus_ast_if_set_else(VusAstIf *if_node, VusAstList *body);

/* 添加 except 子句到 Try 节点 */
void vus_ast_try_add_except(VusAstTry *try_node, const char *type, VusAstList *body);

#ifdef __cplusplus
}
#endif

#endif /* VUS_AST_H */