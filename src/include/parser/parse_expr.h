/*-------------------------------------------------------------------------
 *
 * parse_expr.h
 *
 *
 *
 * Portions Copyright (c) 1996-2003, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL$
 *
 *-------------------------------------------------------------------------
 */
#ifndef PARSE_EXPR_H
#define PARSE_EXPR_H

#include "parser/parse_node.h"


/* GUC parameters */
extern bool Transform_null_equals;


extern Node *transformExpr(ParseState *pstate, Node *expr);
extern Oid	exprType(Node *expr);
extern int32 exprTypmod(Node *expr);
extern bool exprIsLengthCoercion(Node *expr, int32 *coercedTypmod);

#endif   /* PARSE_EXPR_H */
