/*-------------------------------------------------------------------------
 *
 * plpgsql.h		- Definitions for the PL/pgSQL
 *			  procedural language
 *
 * Portions Copyright (c) 1996-2026, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2003-2014, Jonah H. Harris <jonah.harris@gmail.com>
 * Portions Copyright (c) 2014-2026, NEXTGRES, LLC. <oss@nextgres.com>
 *
 * Derived from PostgreSQL src/pl/plpgsql/src/plpgsql.h; modifications are
 * licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License. You may obtain a
 * copy of the License in LICENSE or at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 * SPDX-License-Identifier: Apache-2.0 AND PostgreSQL
 *
 *
 * IDENTIFICATION
 *	  src/pl/plpgsql/src/plpgsql.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef UPL_PLPGSQL_H
#define UPL_PLPGSQL_H

#include "access/xact.h"
#include "commands/event_trigger.h"
#include "commands/trigger.h"
#include "executor/spi.h"
#include "utils/expandedrecord.h"
#include "utils/funccache.h"
#include "utils/typcache.h"


/**********************************************************************
 * Definitions
 **********************************************************************/

/* define our text domain for translations */
#undef TEXTDOMAIN
#define TEXTDOMAIN PG_TEXTDOMAIN("plpgsql")

#undef _
#define _(x) dgettext(TEXTDOMAIN, x)

/*
 * Compiler's namespace item types
 */
typedef enum UPLpgSQL_nsitem_type
{
	UPLPGSQL_NSTYPE_LABEL,		/* block label */
	UPLPGSQL_NSTYPE_VAR,			/* scalar variable */
	UPLPGSQL_NSTYPE_REC,			/* composite variable */
} UPLpgSQL_nsitem_type;

/*
 * A UPLPGSQL_NSTYPE_LABEL stack entry must be one of these types
 */
typedef enum UPLpgSQL_label_type
{
	UPLPGSQL_LABEL_BLOCK,		/* DECLARE/BEGIN block */
	UPLPGSQL_LABEL_LOOP,			/* looping construct */
	UPLPGSQL_LABEL_OTHER,		/* anything else */
} UPLpgSQL_label_type;

/*
 * Datum array node types
 */
typedef enum UPLpgSQL_datum_type
{
	UPLPGSQL_DTYPE_VAR,
	UPLPGSQL_DTYPE_ROW,
	UPLPGSQL_DTYPE_REC,
	UPLPGSQL_DTYPE_RECFIELD,
	UPLPGSQL_DTYPE_PROMISE,
} UPLpgSQL_datum_type;

/*
 * DTYPE_PROMISE datums have these possible ways of computing the promise
 */
typedef enum UPLpgSQL_promise_type
{
	UPLPGSQL_PROMISE_NONE = 0,	/* not a promise, or promise satisfied */
	UPLPGSQL_PROMISE_TG_NAME,
	UPLPGSQL_PROMISE_TG_WHEN,
	UPLPGSQL_PROMISE_TG_LEVEL,
	UPLPGSQL_PROMISE_TG_OP,
	UPLPGSQL_PROMISE_TG_RELID,
	UPLPGSQL_PROMISE_TG_TABLE_NAME,
	UPLPGSQL_PROMISE_TG_TABLE_SCHEMA,
	UPLPGSQL_PROMISE_TG_NARGS,
	UPLPGSQL_PROMISE_TG_ARGV,
	UPLPGSQL_PROMISE_TG_EVENT,
	UPLPGSQL_PROMISE_TG_TAG,
} UPLpgSQL_promise_type;

/*
 * Variants distinguished in UPLpgSQL_type structs
 */
typedef enum UPLpgSQL_type_type
{
	UPLPGSQL_TTYPE_SCALAR,		/* scalar types and domains */
	UPLPGSQL_TTYPE_REC,			/* composite types, including RECORD */
	UPLPGSQL_TTYPE_PSEUDO,		/* pseudotypes */
} UPLpgSQL_type_type;

/*
 * Execution tree node types
 */
typedef enum UPLpgSQL_stmt_type
{
	UPLPGSQL_STMT_BLOCK,
	UPLPGSQL_STMT_ASSIGN,
	UPLPGSQL_STMT_IF,
	UPLPGSQL_STMT_CASE,
	UPLPGSQL_STMT_LOOP,
	UPLPGSQL_STMT_WHILE,
	UPLPGSQL_STMT_FORI,
	UPLPGSQL_STMT_FORS,
	UPLPGSQL_STMT_FORC,
	UPLPGSQL_STMT_FOREACH_A,
	UPLPGSQL_STMT_EXIT,
	UPLPGSQL_STMT_RETURN,
	UPLPGSQL_STMT_RETURN_NEXT,
	UPLPGSQL_STMT_RETURN_QUERY,
	UPLPGSQL_STMT_RAISE,
	UPLPGSQL_STMT_ASSERT,
	UPLPGSQL_STMT_EXECSQL,
	UPLPGSQL_STMT_DYNEXECUTE,
	UPLPGSQL_STMT_DYNFORS,
	UPLPGSQL_STMT_GETDIAG,
	UPLPGSQL_STMT_OPEN,
	UPLPGSQL_STMT_FETCH,
	UPLPGSQL_STMT_CLOSE,
	UPLPGSQL_STMT_PERFORM,
	UPLPGSQL_STMT_CALL,
	UPLPGSQL_STMT_COMMIT,
	UPLPGSQL_STMT_ROLLBACK,
} UPLpgSQL_stmt_type;

/*
 * Execution node return codes
 */
enum
{
	UPLPGSQL_RC_OK,
	UPLPGSQL_RC_EXIT,
	UPLPGSQL_RC_RETURN,
	UPLPGSQL_RC_CONTINUE,
};

/*
 * GET DIAGNOSTICS information items
 */
typedef enum UPLpgSQL_getdiag_kind
{
	UPLPGSQL_GETDIAG_ROW_COUNT,
	UPLPGSQL_GETDIAG_ROUTINE_OID,
	UPLPGSQL_GETDIAG_CONTEXT,
	UPLPGSQL_GETDIAG_ERROR_CONTEXT,
	UPLPGSQL_GETDIAG_ERROR_DETAIL,
	UPLPGSQL_GETDIAG_ERROR_HINT,
	UPLPGSQL_GETDIAG_RETURNED_SQLSTATE,
	UPLPGSQL_GETDIAG_COLUMN_NAME,
	UPLPGSQL_GETDIAG_CONSTRAINT_NAME,
	UPLPGSQL_GETDIAG_DATATYPE_NAME,
	UPLPGSQL_GETDIAG_MESSAGE_TEXT,
	UPLPGSQL_GETDIAG_TABLE_NAME,
	UPLPGSQL_GETDIAG_SCHEMA_NAME,
} UPLpgSQL_getdiag_kind;

/*
 * RAISE statement options
 */
typedef enum UPLpgSQL_raise_option_type
{
	UPLPGSQL_RAISEOPTION_ERRCODE,
	UPLPGSQL_RAISEOPTION_MESSAGE,
	UPLPGSQL_RAISEOPTION_DETAIL,
	UPLPGSQL_RAISEOPTION_HINT,
	UPLPGSQL_RAISEOPTION_COLUMN,
	UPLPGSQL_RAISEOPTION_CONSTRAINT,
	UPLPGSQL_RAISEOPTION_DATATYPE,
	UPLPGSQL_RAISEOPTION_TABLE,
	UPLPGSQL_RAISEOPTION_SCHEMA,
} UPLpgSQL_raise_option_type;

/*
 * Behavioral modes for plpgsql variable resolution
 */
typedef enum UPLpgSQL_resolve_option
{
	UPLPGSQL_RESOLVE_ERROR,		/* throw error if ambiguous */
	UPLPGSQL_RESOLVE_VARIABLE,	/* prefer plpgsql var to table column */
	UPLPGSQL_RESOLVE_COLUMN,		/* prefer table column to plpgsql var */
} UPLpgSQL_resolve_option;

/*
 * Status of optimization of assignment to a read/write expanded object
 */
typedef enum UPLpgSQL_rwopt
{
	UPLPGSQL_RWOPT_UNKNOWN = 0,	/* applicability not determined yet */
	UPLPGSQL_RWOPT_NOPE,			/* cannot do any optimization */
	UPLPGSQL_RWOPT_TRANSFER,		/* transfer the old value into expr state */
	UPLPGSQL_RWOPT_INPLACE,		/* pass value as R/W to top-level function */
} UPLpgSQL_rwopt;


/**********************************************************************
 * Node and structure definitions
 **********************************************************************/

/*
 * Postgres data type
 */
typedef struct UPLpgSQL_type
{
	char	   *typname;		/* (simple) name of the type */
	Oid			typoid;			/* OID of the data type */
	UPLpgSQL_type_type ttype;	/* UPLPGSQL_TTYPE_ code */
	int16		typlen;			/* stuff copied from its pg_type entry */
	bool		typbyval;
	char		typtype;
	Oid			collation;		/* from pg_type, but can be overridden */
	bool		typisarray;		/* is "true" array, or domain over one */
	int32		atttypmod;		/* typmod (taken from someplace else) */
	/* Remaining fields are used only for named composite types (not RECORD) */
	TypeName   *origtypname;	/* type name as written by user */
	TypeCacheEntry *tcache;		/* typcache entry for composite type */
	uint64		tupdesc_id;		/* last-seen tupdesc identifier */
} UPLpgSQL_type;

/*
 * SQL Query to plan and execute
 */
typedef struct UPLpgSQL_expr
{
	char	   *query;			/* query string, verbatim from function body */
	RawParseMode parseMode;		/* raw_parser() mode to use */
	struct UPLpgSQL_function *func;	/* function containing this expr */
	struct UPLpgSQL_nsitem *ns;	/* namespace chain visible to this expr */

	/*
	 * These fields are used to help optimize assignments to expanded-datum
	 * variables.  If this expression is the source of an assignment to a
	 * simple variable, target_param holds that variable's dno (else it's -1),
	 * and target_is_local indicates whether the target is declared inside the
	 * closest exception block containing the assignment.
	 */
	int			target_param;	/* dno of assign target, or -1 if none */
	bool		target_is_local;	/* is it within nearest exception block? */

	/*
	 * Fields above are set during plpgsql parsing.  Remaining fields are left
	 * as zeroes/NULLs until we first parse/plan the query.
	 */
	SPIPlanPtr	plan;			/* plan, or NULL if not made yet */
	Bitmapset  *paramnos;		/* all dnos referenced by this query */

	/* fields for "simple expression" fast-path execution: */
	Expr	   *expr_simple_expr;	/* NULL means not a simple expr */
	Oid			expr_simple_type;	/* result type Oid, if simple */
	int32		expr_simple_typmod; /* result typmod, if simple */
	bool		expr_simple_mutable;	/* true if simple expr is mutable */

	/*
	 * expr_rwopt tracks whether we have determined that assignment to a
	 * read/write expanded object (stored in the target_param datum) can be
	 * optimized by passing it to the expr as a read/write expanded-object
	 * pointer.  If so, expr_rw_param identifies the specific Param that
	 * should emit a read/write pointer; any others will emit read-only
	 * pointers.
	 */
	UPLpgSQL_rwopt expr_rwopt;	/* can we apply R/W optimization? */
	Param	   *expr_rw_param;	/* read/write Param within expr, if any */

	/*
	 * If the expression was ever determined to be simple, we remember its
	 * CachedPlanSource and CachedPlan here.  If expr_simple_plan_lxid matches
	 * current LXID, then we hold a refcount on expr_simple_plan in the
	 * current transaction.  Otherwise we need to get one before re-using it.
	 */
	CachedPlanSource *expr_simple_plansource;	/* extracted from "plan" */
	CachedPlan *expr_simple_plan;	/* extracted from "plan" */
	LocalTransactionId expr_simple_plan_lxid;

	/*
	 * if expr is simple AND prepared in current transaction,
	 * expr_simple_state and expr_simple_in_use are valid. Test validity by
	 * seeing if expr_simple_lxid matches current LXID.  (If not,
	 * expr_simple_state probably points at garbage!)
	 */
	ExprState  *expr_simple_state;	/* eval tree for expr_simple_expr */
	bool		expr_simple_in_use; /* true if eval tree is active */
	LocalTransactionId expr_simple_lxid;
} UPLpgSQL_expr;

/*
 * Generic datum array item
 *
 * UPLpgSQL_datum is the common supertype for UPLpgSQL_var, UPLpgSQL_row,
 * UPLpgSQL_rec, and UPLpgSQL_recfield.
 */
typedef struct UPLpgSQL_datum
{
	UPLpgSQL_datum_type dtype;
	int			dno;
} UPLpgSQL_datum;

/*
 * Scalar or composite variable
 *
 * The variants UPLpgSQL_var, UPLpgSQL_row, and UPLpgSQL_rec share these
 * fields.
 */
typedef struct UPLpgSQL_variable
{
	UPLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	UPLpgSQL_expr *default_val;
} UPLpgSQL_variable;

/*
 * Scalar variable
 *
 * DTYPE_VAR and DTYPE_PROMISE datums both use this struct type.
 * A PROMISE datum works exactly like a VAR datum for most purposes,
 * but if it is read without having previously been assigned to, then
 * a special "promised" value is computed and assigned to the datum
 * before the read is performed.  This technique avoids the overhead of
 * computing the variable's value in cases where we expect that many
 * functions will never read it.
 */
typedef struct UPLpgSQL_var
{
	UPLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	UPLpgSQL_expr *default_val;
	/* end of UPLpgSQL_variable fields */

	UPLpgSQL_type *datatype;

	/*
	 * Variables declared as CURSOR FOR <query> are mostly like ordinary
	 * scalar variables of type refcursor, but they have these additional
	 * properties:
	 */
	UPLpgSQL_expr *cursor_explicit_expr;
	int			cursor_explicit_argrow;
	int			cursor_options;

	/* Fields below here can change at runtime */

	Datum		value;
	bool		isnull;
	bool		freeval;

	/*
	 * The promise field records which "promised" value to assign if the
	 * promise must be honored.  If it's a normal variable, or the promise has
	 * been fulfilled, this is UPLPGSQL_PROMISE_NONE.
	 */
	UPLpgSQL_promise_type promise;
} UPLpgSQL_var;

/*
 * Row variable - this represents one or more variables that are listed in an
 * INTO clause, FOR-loop targetlist, cursor argument list, etc.  We also use
 * a row to represent a function's OUT parameters when there's more than one.
 *
 * Note that there's no way to name the row as such from PL/pgSQL code,
 * so many functions don't need to support these.
 *
 * That also means that there's no real name for the row variable, so we
 * conventionally set refname to "(unnamed row)".  We could leave it NULL,
 * but it's too convenient to be able to assume that refname is valid in
 * all variants of UPLpgSQL_variable.
 *
 * isconst, notnull, and default_val are unsupported (and hence
 * always zero/null) for a row.  The member variables of a row should have
 * been checked to be writable at compile time, so isconst is correctly set
 * to false.  notnull and default_val aren't applicable.
 */
typedef struct UPLpgSQL_row
{
	UPLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	UPLpgSQL_expr *default_val;
	/* end of UPLpgSQL_variable fields */

	/*
	 * rowtupdesc is only set up if we might need to convert the row into a
	 * composite datum, which currently only happens for OUT parameters.
	 * Otherwise it is NULL.
	 */
	TupleDesc	rowtupdesc;

	int			nfields;
	char	  **fieldnames;
	int		   *varnos;
} UPLpgSQL_row;

/*
 * Record variable (any composite type, including RECORD)
 */
typedef struct UPLpgSQL_rec
{
	UPLpgSQL_datum_type dtype;
	int			dno;
	char	   *refname;
	int			lineno;
	bool		isconst;
	bool		notnull;
	UPLpgSQL_expr *default_val;
	/* end of UPLpgSQL_variable fields */

	/*
	 * Note: for non-RECORD cases, we may from time to time re-look-up the
	 * composite type, using datatype->origtypname.  That can result in
	 * changing rectypeid.
	 */

	UPLpgSQL_type *datatype;		/* can be NULL, if rectypeid is RECORDOID */
	Oid			rectypeid;		/* declared type of variable */
	/* RECFIELDs for this record are chained together for easy access */
	int			firstfield;		/* dno of first RECFIELD, or -1 if none */

	/* Fields below here can change at runtime */

	/* We always store record variables as "expanded" records */
	ExpandedRecordHeader *erh;
} UPLpgSQL_rec;

/*
 * Field in record
 */
typedef struct UPLpgSQL_recfield
{
	UPLpgSQL_datum_type dtype;
	int			dno;
	/* end of UPLpgSQL_datum fields */

	char	   *fieldname;		/* name of field */
	int			recparentno;	/* dno of parent record */
	int			nextfield;		/* dno of next child, or -1 if none */
	uint64		rectupledescid; /* record's tupledesc ID as of last lookup */
	ExpandedRecordFieldInfo finfo;	/* field's attnum and type info */
	/* if rectupledescid == INVALID_TUPLEDESC_IDENTIFIER, finfo isn't valid */
} UPLpgSQL_recfield;

/*
 * Item in the compilers namespace tree
 */
typedef struct UPLpgSQL_nsitem
{
	UPLpgSQL_nsitem_type itemtype;

	/*
	 * For labels, itemno is a value of enum UPLpgSQL_label_type. For other
	 * itemtypes, itemno is the associated UPLpgSQL_datum's dno.
	 */
	int			itemno;
	struct UPLpgSQL_nsitem *prev;
	char		name[FLEXIBLE_ARRAY_MEMBER];	/* nul-terminated string */
} UPLpgSQL_nsitem;

/*
 * Generic execution node
 */
typedef struct UPLpgSQL_stmt
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;

	/*
	 * Unique statement ID in this function (starting at 1; 0 is invalid/not
	 * set).  This can be used by a profiler as the index for an array of
	 * per-statement metrics.
	 */
	unsigned int stmtid;
} UPLpgSQL_stmt;

/*
 * One EXCEPTION condition name
 */
typedef struct UPLpgSQL_condition
{
	int			sqlerrstate;	/* SQLSTATE code, or UPLPGSQL_OTHERS */
	char	   *condname;		/* condition name (for debugging) */
	struct UPLpgSQL_condition *next;
} UPLpgSQL_condition;

/* This value mustn't match any possible output of MAKE_SQLSTATE() */
#define UPLPGSQL_OTHERS (-1)

/*
 * EXCEPTION block
 */
typedef struct UPLpgSQL_exception_block
{
	int			sqlstate_varno;
	int			sqlerrm_varno;
	List	   *exc_list;		/* List of WHEN clauses */
} UPLpgSQL_exception_block;

/*
 * One EXCEPTION ... WHEN clause
 */
typedef struct UPLpgSQL_exception
{
	int			lineno;
	UPLpgSQL_condition *conditions;
	List	   *action;			/* List of statements */
} UPLpgSQL_exception;

/*
 * Block of statements
 */
typedef struct UPLpgSQL_stmt_block
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	List	   *body;			/* List of statements */
	int			n_initvars;		/* Length of initvarnos[] */
	int		   *initvarnos;		/* dnos of variables declared in this block */
	UPLpgSQL_exception_block *exceptions;
	int			sqlstate_varno; /* dno of SQLSTATE variable, or -1 */
} UPLpgSQL_stmt_block;

/*
 * Assign statement
 */
typedef struct UPLpgSQL_stmt_assign
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			varno;
	UPLpgSQL_expr *expr;
} UPLpgSQL_stmt_assign;

/*
 * PERFORM statement
 */
typedef struct UPLpgSQL_stmt_perform
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *expr;
} UPLpgSQL_stmt_perform;

/*
 * CALL statement
 */
typedef struct UPLpgSQL_stmt_call
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *expr;
	bool		is_call;
	UPLpgSQL_variable *target;
} UPLpgSQL_stmt_call;

/*
 * COMMIT statement
 */
typedef struct UPLpgSQL_stmt_commit
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		chain;
} UPLpgSQL_stmt_commit;

/*
 * ROLLBACK statement
 */
typedef struct UPLpgSQL_stmt_rollback
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		chain;
} UPLpgSQL_stmt_rollback;

/*
 * GET DIAGNOSTICS item
 */
typedef struct UPLpgSQL_diag_item
{
	UPLpgSQL_getdiag_kind kind;	/* id for diagnostic value desired */
	int			target;			/* where to assign it */
} UPLpgSQL_diag_item;

/*
 * GET DIAGNOSTICS statement
 */
typedef struct UPLpgSQL_stmt_getdiag
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		is_stacked;		/* STACKED or CURRENT diagnostics area? */
	List	   *diag_items;		/* List of UPLpgSQL_diag_item */
} UPLpgSQL_stmt_getdiag;

/*
 * IF statement
 */
typedef struct UPLpgSQL_stmt_if
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *cond;			/* boolean expression for THEN */
	List	   *then_body;		/* List of statements */
	List	   *elsif_list;		/* List of UPLpgSQL_if_elsif structs */
	List	   *else_body;		/* List of statements */
} UPLpgSQL_stmt_if;

/*
 * one ELSIF arm of IF statement
 */
typedef struct UPLpgSQL_if_elsif
{
	int			lineno;
	UPLpgSQL_expr *cond;			/* boolean expression for this case */
	List	   *stmts;			/* List of statements */
} UPLpgSQL_if_elsif;

/*
 * CASE statement
 */
typedef struct UPLpgSQL_stmt_case
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *t_expr;		/* test expression, or NULL if none */
	int			t_varno;		/* var to store test expression value into */
	List	   *case_when_list; /* List of UPLpgSQL_case_when structs */
	bool		have_else;		/* flag needed because list could be empty */
	List	   *else_stmts;		/* List of statements */
} UPLpgSQL_stmt_case;

/*
 * one arm of CASE statement
 */
typedef struct UPLpgSQL_case_when
{
	int			lineno;
	UPLpgSQL_expr *expr;			/* boolean expression for this case */
	List	   *stmts;			/* List of statements */
} UPLpgSQL_case_when;

/*
 * Unconditional LOOP statement
 */
typedef struct UPLpgSQL_stmt_loop
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	List	   *body;			/* List of statements */
} UPLpgSQL_stmt_loop;

/*
 * WHILE cond LOOP statement (also used for REPEAT UNTIL)
 *
 * test_at_top = true:  WHILE semantics (test before body, default)
 * test_at_top = false: REPEAT UNTIL semantics (test after body)
 */
typedef struct UPLpgSQL_stmt_while
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	UPLpgSQL_expr *cond;
	List	   *body;			/* List of statements */
	bool		test_at_top;	/* true=WHILE, false=REPEAT UNTIL */
} UPLpgSQL_stmt_while;

/*
 * FOR statement with integer loopvar
 */
typedef struct UPLpgSQL_stmt_fori
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	UPLpgSQL_var *var;
	UPLpgSQL_expr *lower;
	UPLpgSQL_expr *upper;
	UPLpgSQL_expr *step;			/* NULL means default (ie, BY 1) */
	int			reverse;
	List	   *body;			/* List of statements */
} UPLpgSQL_stmt_fori;

/*
 * UPLpgSQL_stmt_forq represents a FOR statement running over a SQL query.
 * It is the common supertype of UPLpgSQL_stmt_fors, UPLpgSQL_stmt_forc
 * and UPLpgSQL_stmt_dynfors.
 */
typedef struct UPLpgSQL_stmt_forq
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	UPLpgSQL_variable *var;		/* Loop variable (record or row) */
	List	   *body;			/* List of statements */
} UPLpgSQL_stmt_forq;

/*
 * FOR statement running over SELECT
 */
typedef struct UPLpgSQL_stmt_fors
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	UPLpgSQL_variable *var;		/* Loop variable (record or row) */
	List	   *body;			/* List of statements */
	/* end of fields that must match UPLpgSQL_stmt_forq */
	UPLpgSQL_expr *query;
} UPLpgSQL_stmt_fors;

/*
 * FOR statement running over cursor
 */
typedef struct UPLpgSQL_stmt_forc
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	UPLpgSQL_variable *var;		/* Loop variable (record or row) */
	List	   *body;			/* List of statements */
	/* end of fields that must match UPLpgSQL_stmt_forq */
	int			curvar;
	UPLpgSQL_expr *argquery;		/* cursor arguments if any */
} UPLpgSQL_stmt_forc;

/*
 * FOR statement running over EXECUTE
 */
typedef struct UPLpgSQL_stmt_dynfors
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	UPLpgSQL_variable *var;		/* Loop variable (record or row) */
	List	   *body;			/* List of statements */
	/* end of fields that must match UPLpgSQL_stmt_forq */
	UPLpgSQL_expr *query;
	List	   *params;			/* USING expressions */
} UPLpgSQL_stmt_dynfors;

/*
 * FOREACH item in array loop
 */
typedef struct UPLpgSQL_stmt_foreach_a
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	char	   *label;
	int			varno;			/* loop target variable */
	int			slice;			/* slice dimension, or 0 */
	UPLpgSQL_expr *expr;			/* array expression */
	List	   *body;			/* List of statements */
} UPLpgSQL_stmt_foreach_a;

/*
 * OPEN a curvar
 */
typedef struct UPLpgSQL_stmt_open
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			curvar;
	int			cursor_options;
	UPLpgSQL_expr *argquery;
	UPLpgSQL_expr *query;
	UPLpgSQL_expr *dynquery;
	List	   *params;			/* USING expressions */
} UPLpgSQL_stmt_open;

/*
 * FETCH or MOVE statement
 */
typedef struct UPLpgSQL_stmt_fetch
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_variable *target;	/* target (record or row) */
	int			curvar;			/* cursor variable to fetch from */
	FetchDirection direction;	/* fetch direction */
	long		how_many;		/* count, if constant (expr is NULL) */
	UPLpgSQL_expr *expr;			/* count, if expression */
	bool		is_move;		/* is this a fetch or move? */
	bool		returns_multiple_rows;	/* can return more than one row? */
} UPLpgSQL_stmt_fetch;

/*
 * CLOSE curvar
 */
typedef struct UPLpgSQL_stmt_close
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			curvar;
} UPLpgSQL_stmt_close;

/*
 * EXIT or CONTINUE statement
 */
typedef struct UPLpgSQL_stmt_exit
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	bool		is_exit;		/* Is this an exit or a continue? */
	char	   *label;			/* NULL if it's an unlabeled EXIT/CONTINUE */
	UPLpgSQL_expr *cond;
} UPLpgSQL_stmt_exit;

/*
 * RETURN statement
 */
typedef struct UPLpgSQL_stmt_return
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *expr;
	int			retvarno;
} UPLpgSQL_stmt_return;

/*
 * RETURN NEXT statement
 */
typedef struct UPLpgSQL_stmt_return_next
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *expr;
	int			retvarno;
} UPLpgSQL_stmt_return_next;

/*
 * RETURN QUERY statement
 */
typedef struct UPLpgSQL_stmt_return_query
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *query;		/* if static query */
	UPLpgSQL_expr *dynquery;		/* if dynamic query (RETURN QUERY EXECUTE) */
	List	   *params;			/* USING arguments for dynamic query */
} UPLpgSQL_stmt_return_query;

/*
 * RAISE statement
 */
typedef struct UPLpgSQL_stmt_raise
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	int			elog_level;
	char	   *condname;		/* condition name, SQLSTATE, or NULL */
	char	   *message;		/* old-style message format literal, or NULL */
	List	   *params;			/* list of expressions for old-style message */
	List	   *options;		/* list of UPLpgSQL_raise_option */
} UPLpgSQL_stmt_raise;

/*
 * RAISE statement option
 */
typedef struct UPLpgSQL_raise_option
{
	UPLpgSQL_raise_option_type opt_type;
	UPLpgSQL_expr *expr;
} UPLpgSQL_raise_option;

/*
 * ASSERT statement
 */
typedef struct UPLpgSQL_stmt_assert
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *cond;
	UPLpgSQL_expr *message;
} UPLpgSQL_stmt_assert;

/*
 * Generic SQL statement to execute
 */
typedef struct UPLpgSQL_stmt_execsql
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *sqlstmt;
	bool		mod_stmt;		/* is the stmt INSERT/UPDATE/DELETE/MERGE? */
	bool		mod_stmt_set;	/* is mod_stmt valid yet? */
	bool		into;			/* INTO supplied? */
	bool		strict;			/* INTO STRICT flag */
	UPLpgSQL_variable *target;	/* INTO target (record or row) */
} UPLpgSQL_stmt_execsql;

/*
 * Dynamic SQL string to execute
 */
typedef struct UPLpgSQL_stmt_dynexecute
{
	UPLpgSQL_stmt_type cmd_type;
	int			lineno;
	unsigned int stmtid;
	UPLpgSQL_expr *query;		/* string expression */
	bool		into;			/* INTO supplied? */
	bool		strict;			/* INTO STRICT flag */
	UPLpgSQL_variable *target;	/* INTO target (record or row) */
	List	   *params;			/* USING expressions */
} UPLpgSQL_stmt_dynexecute;

/*
 * Trigger type
 */
typedef enum UPLpgSQL_trigtype
{
	UPLPGSQL_DML_TRIGGER,
	UPLPGSQL_EVENT_TRIGGER,
	UPLPGSQL_NOT_TRIGGER,
} UPLpgSQL_trigtype;

/*
 * Complete compiled function
 */
typedef struct UPLpgSQL_function
{
	CachedFunction cfunc;		/* fields managed by funccache.c */

	char	   *fn_signature;
	Oid			fn_oid;
	UPLpgSQL_trigtype fn_is_trigger;
	Oid			fn_input_collation;
	MemoryContext fn_cxt;

	Oid			fn_rettype;
	int			fn_rettyplen;
	bool		fn_retbyval;
	bool		fn_retistuple;
	bool		fn_retisdomain;
	bool		fn_retset;
	bool		fn_readonly;
	char		fn_prokind;

	int			fn_nargs;
	int			fn_argvarnos[FUNC_MAX_ARGS];
	int			out_param_varno;
	int			found_varno;
	int			new_varno;
	int			old_varno;

	UPLpgSQL_resolve_option resolve_option;

	bool		print_strict_params;

	/* extra checks */
	int			extra_warnings;
	int			extra_errors;

	/* the datums representing the function's local variables */
	int			ndatums;
	UPLpgSQL_datum **datums;
	Size		copiable_size;	/* space for locally instantiated datums */

	/* function body parsetree */
	UPLpgSQL_stmt_block *action;

	/* data derived while parsing body */
	unsigned int nstatements;	/* counter for assigning stmtids */
	bool		requires_procedure_resowner;	/* contains CALL or DO? */
	bool		has_exception_block;	/* contains BEGIN...EXCEPTION? */

	/* this field changes when the function is used */
	struct UPLpgSQL_execstate *cur_estate;
} UPLpgSQL_function;

/*
 * Runtime execution data
 */
typedef struct UPLpgSQL_execstate
{
	UPLpgSQL_function *func;		/* function being executed */

	TriggerData *trigdata;		/* if regular trigger, data about firing */
	EventTriggerData *evtrigdata;	/* if event trigger, data about firing */

	Datum		retval;
	bool		retisnull;
	Oid			rettype;		/* type of current retval */

	Oid			fn_rettype;		/* info about declared function rettype */
	bool		retistuple;
	bool		retisset;

	bool		readonly_func;
	bool		atomic;

	char	   *exitlabel;		/* the "target" label of the current EXIT or
								 * CONTINUE stmt, if any */
	ErrorData  *cur_error;		/* current exception handler's error */

	Tuplestorestate *tuple_store;	/* SRFs accumulate results here */
	TupleDesc	tuple_store_desc;	/* descriptor for tuples in tuple_store */
	MemoryContext tuple_store_cxt;
	ResourceOwner tuple_store_owner;
	ReturnSetInfo *rsi;

	int			found_varno;

	/* SQL/PSM SQLSTATE variable tracking */
	int			sqlstate_varno;	/* dno of current SQLSTATE var, or -1 */
	int			sqlerrcode;		/* current SQLSTATE error code */

	/*
	 * The datums representing the function's local variables.  Some of these
	 * are local storage in this execstate, but some just point to the shared
	 * copy belonging to the UPLpgSQL_function, depending on whether or not we
	 * need any per-execution state for the datum's dtype.
	 */
	int			ndatums;
	UPLpgSQL_datum **datums;
	/* context containing variable values (same as func's SPI_proc context) */
	MemoryContext datum_context;

	/*
	 * paramLI is what we use to pass local variable values to the executor.
	 * It does not have a ParamExternData array; we just dynamically
	 * instantiate parameter data as needed.  By convention, PARAM_EXTERN
	 * Params have paramid equal to the dno of the referenced local variable.
	 */
	ParamListInfo paramLI;

	/* EState and resowner to use for "simple" expression evaluation */
	EState	   *simple_eval_estate;
	ResourceOwner simple_eval_resowner;

	/* if running nonatomic procedure or DO block, resowner to use for CALL */
	ResourceOwner procedure_resowner;

	/* lookup table to use for executing type casts */
	HTAB	   *cast_hash;

	/* memory context for statement-lifespan temporary values */
	MemoryContext stmt_mcontext;	/* current stmt context, or NULL if none */
	MemoryContext stmt_mcontext_parent; /* parent of current context */

	/* temporary state for results from evaluation of query or expr */
	SPITupleTable *eval_tuptable;
	uint64		eval_processed;
	ExprContext *eval_econtext; /* for executing simple expressions */

	/* status information for error context reporting */
	UPLpgSQL_stmt *err_stmt;		/* current stmt */
	UPLpgSQL_variable *err_var;	/* current variable, if in a DECLARE section */
	const char *err_text;		/* additional state info */

	void	   *plugin_info;	/* reserved for use by optional plugin */
} UPLpgSQL_execstate;

/*
 * A UPLpgSQL_plugin structure represents an instrumentation plugin.
 * To instrument PL/pgSQL, a plugin library must access the rendezvous
 * variable "UPLpgSQL_plugin" and set it to point to a UPLpgSQL_plugin struct.
 * Typically the struct could just be static data in the plugin library.
 * We expect that a plugin would do this at library load time (_PG_init()).
 *
 * This structure is basically a collection of function pointers --- at
 * various interesting points in pl_exec.c, we call these functions
 * (if the pointers are non-NULL) to give the plugin a chance to watch
 * what we are doing.
 *
 * func_setup is called when we start a function, before we've initialized
 * the local variables defined by the function.
 *
 * func_beg is called when we start a function, after we've initialized
 * the local variables.
 *
 * func_end is called at the end of a function.
 *
 * stmt_beg and stmt_end are called before and after (respectively) each
 * statement.
 *
 * Also, immediately before any call to func_setup, PL/pgSQL fills in the
 * remaining fields with pointers to some of its own functions, allowing the
 * plugin to invoke those functions conveniently.  The exposed functions are:
 *		uplpgsql_exec_error_callback
 *		exec_assign_expr
 *		exec_assign_value
 *		exec_eval_datum
 *		exec_cast_value
 * (uplpgsql_exec_error_callback is not actually meant to be called by the
 * plugin, but rather to allow it to identify PL/pgSQL error context stack
 * frames.  The others are useful for debugger-like plugins to examine and
 * set variables.)
 */
typedef struct UPLpgSQL_plugin
{
	/* Function pointers set up by the plugin */
	void		(*func_setup) (UPLpgSQL_execstate *estate, UPLpgSQL_function *func);
	void		(*func_beg) (UPLpgSQL_execstate *estate, UPLpgSQL_function *func);
	void		(*func_end) (UPLpgSQL_execstate *estate, UPLpgSQL_function *func);
	void		(*stmt_beg) (UPLpgSQL_execstate *estate, UPLpgSQL_stmt *stmt);
	void		(*stmt_end) (UPLpgSQL_execstate *estate, UPLpgSQL_stmt *stmt);

	/* Function pointers set by PL/pgSQL itself */
	void		(*error_callback) (void *arg);
	void		(*assign_expr) (UPLpgSQL_execstate *estate,
								UPLpgSQL_datum *target,
								UPLpgSQL_expr *expr);
	void		(*assign_value) (UPLpgSQL_execstate *estate,
								 UPLpgSQL_datum *target,
								 Datum value, bool isNull,
								 Oid valtype, int32 valtypmod);
	void		(*eval_datum) (UPLpgSQL_execstate *estate, UPLpgSQL_datum *datum,
							   Oid *typeId, int32 *typetypmod,
							   Datum *value, bool *isnull);
	Datum		(*cast_value) (UPLpgSQL_execstate *estate,
							   Datum value, bool *isnull,
							   Oid valtype, int32 valtypmod,
							   Oid reqtype, int32 reqtypmod);
} UPLpgSQL_plugin;

/*
 * Struct types used during parsing
 */

typedef struct PLword
{
	char	   *ident;			/* palloc'd converted identifier */
	bool		quoted;			/* Was it double-quoted? */
} PLword;

typedef struct PLcword
{
	List	   *idents;			/* composite identifiers (list of String) */
} PLcword;

typedef struct PLwdatum
{
	UPLpgSQL_datum *datum;		/* referenced variable */
	char	   *ident;			/* valid if simple name */
	bool		quoted;
	List	   *idents;			/* valid if composite name */
} PLwdatum;

/**********************************************************************
 * Global variable declarations
 **********************************************************************/

typedef enum
{
	IDENTIFIER_LOOKUP_NORMAL,	/* normal processing of var names */
	IDENTIFIER_LOOKUP_DECLARE,	/* In DECLARE --- don't look up names */
	IDENTIFIER_LOOKUP_EXPR,		/* In SQL expression --- special case */
} IdentifierLookup;

extern IdentifierLookup uplpgsql_IdentifierLookup;

extern int	uplpgsql_variable_conflict;

extern bool uplpgsql_print_strict_params;

extern bool uplpgsql_check_asserts;

/* extra compile-time and run-time checks */
#define UPLPGSQL_XCHECK_NONE						0
#define UPLPGSQL_XCHECK_SHADOWVAR				(1 << 1)
#define UPLPGSQL_XCHECK_TOOMANYROWS				(1 << 2)
#define UPLPGSQL_XCHECK_STRICTMULTIASSIGNMENT	(1 << 3)
#define UPLPGSQL_XCHECK_ALL						((int) ~0)

extern int	uplpgsql_extra_warnings;
extern int	uplpgsql_extra_errors;

extern bool uplpgsql_check_syntax;
extern bool uplpgsql_DumpExecTree;

extern int	uplpgsql_nDatums;
extern UPLpgSQL_datum **uplpgsql_Datums;

extern char *uplpgsql_error_funcname;

extern UPLpgSQL_function *uplpgsql_curr_compile;
extern MemoryContext uplpgsql_compile_tmp_cxt;

extern UPLpgSQL_plugin **uplpgsql_plugin_ptr;

/**********************************************************************
 * Function declarations
 **********************************************************************/

/*
 * Functions in pl_comp.c
 */
extern UPLpgSQL_function *uplpgsql_compile(FunctionCallInfo fcinfo,
													 bool forValidator);
extern UPLpgSQL_function *uplpgsql_compile_inline(char *proc_source);
extern void uplpgsql_parser_setup(struct ParseState *pstate,
											 UPLpgSQL_expr *expr);
extern bool uplpgsql_parse_word(char *word1, const char *yytxt, bool lookup,
							   PLwdatum *wdatum, PLword *word);
extern bool uplpgsql_parse_dblword(char *word1, char *word2,
								  PLwdatum *wdatum, PLcword *cword);
extern bool uplpgsql_parse_tripword(char *word1, char *word2, char *word3,
								   PLwdatum *wdatum, PLcword *cword);
extern UPLpgSQL_type *uplpgsql_parse_wordtype(char *ident);
extern UPLpgSQL_type *uplpgsql_parse_cwordtype(List *idents);
extern UPLpgSQL_type *uplpgsql_parse_wordrowtype(char *ident);
extern UPLpgSQL_type *uplpgsql_parse_cwordrowtype(List *idents);
extern UPLpgSQL_type *uplpgsql_build_datatype(Oid typeOid, int32 typmod,
														Oid collation,
														TypeName *origtypname);
extern UPLpgSQL_type *uplpgsql_build_datatype_arrayof(UPLpgSQL_type *dtype);
extern UPLpgSQL_variable *uplpgsql_build_variable(const char *refname, int lineno,
												UPLpgSQL_type *dtype,
												bool add2namespace);
extern UPLpgSQL_rec *uplpgsql_build_record(const char *refname, int lineno,
										 UPLpgSQL_type *dtype, Oid rectypeid,
										 bool add2namespace);
extern UPLpgSQL_recfield *uplpgsql_build_recfield(UPLpgSQL_rec *rec,
												const char *fldname);
extern int uplpgsql_recognize_err_condition(const char *condname,
													   bool allow_sqlstate);
extern UPLpgSQL_condition *uplpgsql_parse_err_condition(char *condname);
extern void uplpgsql_adddatum(UPLpgSQL_datum *newdatum);
extern int	uplpgsql_add_initdatums(int **varnos);

/*
 * Functions in pl_exec.c
 */
extern Datum uplpgsql_exec_function(UPLpgSQL_function *func,
								   FunctionCallInfo fcinfo,
								   EState *simple_eval_estate,
								   ResourceOwner simple_eval_resowner,
								   ResourceOwner procedure_resowner,
								   bool atomic);
extern HeapTuple uplpgsql_exec_trigger(UPLpgSQL_function *func,
									  TriggerData *trigdata);
extern void uplpgsql_exec_event_trigger(UPLpgSQL_function *func,
									   EventTriggerData *trigdata);
extern void uplpgsql_xact_cb(XactEvent event, void *arg);
extern void uplpgsql_subxact_cb(SubXactEvent event, SubTransactionId mySubid,
							   SubTransactionId parentSubid, void *arg);
extern Oid uplpgsql_exec_get_datum_type(UPLpgSQL_execstate *estate,
												   UPLpgSQL_datum *datum);
extern void uplpgsql_exec_get_datum_type_info(UPLpgSQL_execstate *estate,
											 UPLpgSQL_datum *datum,
											 Oid *typeId, int32 *typMod,
											 Oid *collation);

/*
 * Functions for namespace handling in pl_funcs.c
 */
extern void uplpgsql_ns_init(void);
extern void uplpgsql_ns_push(const char *label,
							UPLpgSQL_label_type label_type);
extern void uplpgsql_ns_pop(void);
extern UPLpgSQL_nsitem *uplpgsql_ns_top(void);
extern void uplpgsql_ns_additem(UPLpgSQL_nsitem_type itemtype, int itemno, const char *name);
extern UPLpgSQL_nsitem *uplpgsql_ns_lookup(UPLpgSQL_nsitem *ns_cur, bool localmode,
													 const char *name1, const char *name2,
													 const char *name3, int *names_used);
extern UPLpgSQL_nsitem *uplpgsql_ns_lookup_label(UPLpgSQL_nsitem *ns_cur,
											   const char *name);
extern UPLpgSQL_nsitem *uplpgsql_ns_find_nearest_loop(UPLpgSQL_nsitem *ns_cur);

/*
 * Other functions in pl_funcs.c
 */
extern const char *uplpgsql_stmt_typename(UPLpgSQL_stmt *stmt);
extern const char *uplpgsql_getdiag_kindname(UPLpgSQL_getdiag_kind kind);
extern void uplpgsql_mark_local_assignment_targets(UPLpgSQL_function *func);
extern void uplpgsql_free_function_memory(UPLpgSQL_function *func);
extern void uplpgsql_delete_callback(CachedFunction *cfunc);
extern void uplpgsql_dumptree(UPLpgSQL_function *func);

/*
 * Scanner functions in pl_scanner.c
 */
union YYSTYPE;
#define YYLTYPE int
typedef void *yyscan_t;
extern int	uplpgsql_yylex(union YYSTYPE *yylvalp, YYLTYPE *yyllocp, yyscan_t yyscanner);
extern int	uplpgsql_token_length(yyscan_t yyscanner);
extern void uplpgsql_push_back_token(int token, union YYSTYPE *yylvalp, YYLTYPE *yyllocp, yyscan_t yyscanner);
extern bool uplpgsql_token_is_unreserved_keyword(int token);
extern void uplpgsql_append_source_text(StringInfo buf,
									   int startlocation, int endlocation,
									   yyscan_t yyscanner);
extern int	uplpgsql_peek(yyscan_t yyscanner);
extern void uplpgsql_peek2(int *tok1_p, int *tok2_p, int *tok1_loc,
						  int *tok2_loc, yyscan_t yyscanner);
extern int	uplpgsql_scanner_errposition(int location, yyscan_t yyscanner);
pg_noreturn extern void uplpgsql_yyerror(YYLTYPE *yyllocp, UPLpgSQL_stmt_block **uplpgsql_parse_result_p, yyscan_t yyscanner, const char *message);
extern int	uplpgsql_location_to_lineno(int location, yyscan_t yyscanner);
extern int	uplpgsql_latest_lineno(yyscan_t yyscanner);
extern yyscan_t uplpgsql_scanner_init(const char *str);
extern void uplpgsql_scanner_finish(yyscan_t yyscanner);

/*
 * Externs in pl_gram.y
 */
extern int	uplpgsql_yyparse(UPLpgSQL_stmt_block **uplpgsql_parse_result_p, yyscan_t yyscanner);

/*
 * Executor internals exported for JIT runtime helpers.
 * These are normally static in pl_exec.c but we need them
 * callable from uplpgsql_runtime.c.
 */
extern void uplpgsql_estate_setup(UPLpgSQL_execstate *estate,
								  UPLpgSQL_function *func,
								  ReturnSetInfo *rsi,
								  EState *simple_eval_estate,
								  ResourceOwner simple_eval_resowner);
extern void uplpgsql_destroy_econtext(UPLpgSQL_execstate *estate);
extern Datum exec_eval_expr(UPLpgSQL_execstate *estate,
							UPLpgSQL_expr *expr,
							bool *isNull,
							Oid *rettype,
							int32 *rettypmod);
extern void exec_eval_cleanup(UPLpgSQL_execstate *estate);
extern int	exec_eval_integer(UPLpgSQL_execstate *estate,
							  UPLpgSQL_expr *expr,
							  bool *isNull);
extern bool exec_eval_boolean(UPLpgSQL_execstate *estate,
							  UPLpgSQL_expr *expr,
							  bool *isNull);
extern void exec_assign_expr(UPLpgSQL_execstate *estate,
							 UPLpgSQL_datum *target,
							 UPLpgSQL_expr *expr);
extern void exec_assign_value(UPLpgSQL_execstate *estate,
							  UPLpgSQL_datum *target,
							  Datum value, bool isNull,
							  Oid valtype, int32 valtypmod);
extern void exec_eval_datum(UPLpgSQL_execstate *estate,
							UPLpgSQL_datum *datum,
							Oid *typeid,
							int32 *typetypmod,
							Datum *value,
							bool *isnull);
extern void assign_simple_var(UPLpgSQL_execstate *estate, UPLpgSQL_var *var,
							  Datum newvalue, bool isnull, bool freeable);
extern void exec_set_found(UPLpgSQL_execstate *estate, bool state);
extern void uplpgsql_set_sqlstate(UPLpgSQL_execstate *estate);
extern int	exec_stmt_return(UPLpgSQL_execstate *estate,
							 UPLpgSQL_stmt_return *stmt);
extern int	exec_stmt_execsql(UPLpgSQL_execstate *estate,
							  UPLpgSQL_stmt_execsql *stmt);
extern int	exec_stmt_raise(UPLpgSQL_execstate *estate,
							UPLpgSQL_stmt_raise *stmt);
extern int	exec_stmt_perform(UPLpgSQL_execstate *estate,
							  UPLpgSQL_stmt_perform *stmt);
extern int	exec_stmt_fori(UPLpgSQL_execstate *estate,
						   UPLpgSQL_stmt_fori *stmt);
extern int	exec_stmt_assert(UPLpgSQL_execstate *estate,
							 UPLpgSQL_stmt_assert *stmt);
extern int	exec_run_select(UPLpgSQL_execstate *estate,
							UPLpgSQL_expr *expr, long maxtuples,
							Portal *portalP);
extern void exec_move_row(UPLpgSQL_execstate *estate,
						  UPLpgSQL_variable *target,
						  HeapTuple tup, TupleDesc tupdesc);
extern int	exec_stmt_open(UPLpgSQL_execstate *estate,
						   UPLpgSQL_stmt_open *stmt);
extern int	exec_stmt_fetch(UPLpgSQL_execstate *estate,
							UPLpgSQL_stmt_fetch *stmt);
extern int	exec_stmt_close(UPLpgSQL_execstate *estate,
							UPLpgSQL_stmt_close *stmt);
extern int	exec_stmt_block(UPLpgSQL_execstate *estate,
							UPLpgSQL_stmt_block *block);
extern int	exec_stmt_call(UPLpgSQL_execstate *estate,
						   UPLpgSQL_stmt_call *stmt);
extern int	exec_stmt_getdiag(UPLpgSQL_execstate *estate,
							  UPLpgSQL_stmt_getdiag *stmt);
extern int	exec_stmt_return_next(UPLpgSQL_execstate *estate,
								  UPLpgSQL_stmt_return_next *stmt);
extern int	exec_stmt_return_query(UPLpgSQL_execstate *estate,
								   UPLpgSQL_stmt_return_query *stmt);
extern int	exec_stmt_dynexecute(UPLpgSQL_execstate *estate,
								 UPLpgSQL_stmt_dynexecute *stmt);
extern int	exec_stmt_dynfors(UPLpgSQL_execstate *estate,
							  UPLpgSQL_stmt_dynfors *stmt);
extern int	exec_stmt_foreach_a(UPLpgSQL_execstate *estate,
								UPLpgSQL_stmt_foreach_a *stmt);
extern int	exec_stmt_commit(UPLpgSQL_execstate *estate,
							 UPLpgSQL_stmt_commit *stmt);
extern int	exec_stmt_rollback(UPLpgSQL_execstate *estate,
							   UPLpgSQL_stmt_rollback *stmt);
extern Portal exec_open_forc_cursor(UPLpgSQL_execstate *estate,
									UPLpgSQL_stmt_forc *stmt);
extern Portal exec_dynquery_with_params(UPLpgSQL_execstate *estate,
										UPLpgSQL_expr *dynquery,
										List *params,
										const char *portalname,
										int cursorOptions);
extern void exec_close_forc_cursor(UPLpgSQL_execstate *estate,
								   UPLpgSQL_stmt_forc *stmt,
								   Portal portal);

/* Exception handling support functions */
extern MemoryContext get_stmt_mcontext(UPLpgSQL_execstate *estate);
extern void pop_stmt_mcontext(UPLpgSQL_execstate *estate);
extern bool exception_matches_conditions(ErrorData *edata,
										 UPLpgSQL_condition *cond);
extern void uplpgsql_create_econtext(UPLpgSQL_execstate *estate);
extern void assign_text_var(UPLpgSQL_execstate *estate, UPLpgSQL_var *var,
							const char *str);

#endif							/* UPL_PLPGSQL_H */
