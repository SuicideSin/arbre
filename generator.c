/*
 * arbre
 *
 * (c) 2011-2012, Alexis Sellier
 *
 * generator.c
 *
 *     code generator
 *
 * +  generator
 * -  generator_free
 *
 * -> generate
 *
 */
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <limits.h>
#include <assert.h>

#include "arbre.h"
#include "op.h"
#include "runtime.h"
#include "vm.h"
#include "generator.h"
#include "error.h"

size_t strnlen(const char *, size_t);
char  *strndup(const char *, size_t);

#define REPORT_GEN
#include "report.h"
#undef  REPORT_GEN

static int    gen_block   (Generator *, struct node *);
static int    gen_match   (Generator *, struct node *);
static int    gen_bind    (Generator *, struct node *);
static int    gen_node    (Generator *, struct node *);
static int    gen_ident   (Generator *, struct node *);
static int    gen_tuple   (Generator *, struct node *);
static int    gen_list    (Generator *, struct node *);
static int    gen_cons    (Generator *, struct node *);
static int    gen_add     (Generator *, struct node *);
static int    gen_sub     (Generator *, struct node *);
static int    gen_gt      (Generator *, struct node *);
static int    gen_lt      (Generator *, struct node *);
static int    gen_num     (Generator *, struct node *);
static int    gen_atom    (Generator *, struct node *);
static int    gen_path    (Generator *, struct node *);
static int    gen_select  (Generator *, struct node *);
static int    gen_apply   (Generator *, struct node *);
static int    gen_access  (Generator *, struct node *);
static int    gen_clause  (Generator *, struct node *);
static int    gen         (Generator *, Instruction);

static void dump_path(PathEntry *p, FILE *out);
static void gen_locals(Generator *g, struct node *n);

int (*OP_GENERATORS[])(Generator *, struct node *) = {
	[OBLOCK]    =  gen_block,  [ODECL]     =  NULL,
	[OMATCH]    =  gen_match,  [OBIND]     =  gen_bind,
	[OMODULE]   =  NULL,       [OSELECT]   =  gen_select,
	[OWAIT]     =  NULL,       [OIDENT]    =  gen_ident,
	[OTYPE]     =  NULL,       [OADD]      =  gen_add,
	[OPATH]     =  gen_path,   [OMPATH]    =  NULL,
	[OSTRING]   =  NULL,       [OATOM]     =  gen_atom,
	[OCHAR]     =  NULL,       [ONUMBER]   =  gen_num,
	[OTUPLE]    =  gen_tuple,  [OLIST]     =  gen_list,
	[OACCESS]   =  gen_access, [OAPPLY]    =  gen_apply,
	[OSEND]     =  NULL,       [ORANGE]    =  NULL,
	[OCLAUSE]   =  gen_clause, [OPIPE]     =  NULL,
	[OSUB]      =  gen_sub,    [OLT]       =  gen_lt,
	[OGT]       =  gen_gt,     [OCONS]     =  gen_cons
};

static int define(Generator *g, char *ident, int reg)
{
	symtab_insert(g->tree->symbols, ident, symbol(ident, var(ident, reg)));

	return reg;
}

/*
 * Variable entry allocator
 */
Variable *var(char *name, Register reg)
{
	Variable *v = malloc(sizeof(*v));

	v->name = name;
	v->reg  = reg;
	v->type = NULL;

	return v;
}

struct module *source_module(struct source *src)
{
	char *sep  = strrchr(src->path, '.'),
	     *name = strndup(src->path, sep - src->path);

	return module(name, 0);
}

Generator *generator(Tree *tree, struct source *source)
{
	Generator *g = malloc(sizeof(*g));

	g->tree      = tree;
	g->source    = source;
	g->module    = source_module(source);
	g->block     = NULL;
	g->slot      = 1;
	g->path      = NULL;
	g->paths     = calloc(256, sizeof(PathEntry*));
	g->pathsn    = 0;

	return g;
}

void generate(Generator *g, FILE *out)
{
	printf("generating module '%s'..\n", g->module->name);

	gen_block(g, g->tree->root);

	if (out == NULL) return;

	/* Write magic number */
	fputc(167, out);

	/* Write compiler version */
	int version = 0xffffff;
	fwrite(&version, 3, 1, out);

	/* Write path entry count */
	fwrite(&g->pathsn, 4, 1, out);

	for (int i = 0; i < g->pathsn; i++) {
		dump_path(g->paths[i], out);
	}
}

static int gen_constant(Generator *g, const char *src, struct tvalue *tval)
{
	if (src) {
		Sym *k = symtab_lookup(g->path->clause->ktable, src);

		if (k)
			return k->e.tval->v.number;
	}

	int             index  = g->path->clause->kindex++;
	Value           v      = (Value){ .number = index };
	struct tvalue  *indexv = tvalue(-1, v);

	g->path->clause->kheader[index] = tval;

	if (src)
		symtab_insert(g->path->clause->ktable, src, tvsymbol(src, indexv));

	return index;
}

static unsigned nextreg(Generator *g)
{
	// TODO: Check limits
	return g->path->clause->nreg++;
}

static int gen_atom(Generator *g, struct node *n)
{
	Value v = (Value){ .atom = n->src };
	struct tvalue *tval = tvalue(TYPE_ATOM, v);

	return RKASK(gen_constant(g, n->src, tval));
}

static int gen_block(Generator *g, struct node *n)
{
	struct nodelist *ns  = n->o.block.body;
	int              reg = 0;

	g->block = n;

	while (ns) {
		reg = gen_node(g, ns->head);
		ns  = ns->tail;
	}
	g->block = n;

	return reg;
}

static int gen_access(Generator *g, struct node *n)
{
	struct node *lval = n->o.access.lval,
	            *rval = n->o.access.rval;

	/* TODO: This isn't used in all cases, move it. */
	int reg = nextreg(g),
	     rk = gen_node(g, rval);

	switch (lval->o.module.type) {
		case MODULE_CURRENT: {
			/* TODO: Factor this somehow */
			Value v = (Value){ .atom = g->module->name };
			int current = gen_constant(g, g->module->name, tvalue(TYPE_ATOM, v));

			switch (rval->op) {
				case OIDENT: {
					struct PathID *pid = malloc(sizeof(*pid));
					pid->module = g->module->name;
					pid->path = rval->src;
					Value v = (Value){ .pathid = pid };
					return RKASK(gen_constant(g, NULL, tvalue(TYPE_PATHID, v)));
				}
				default:
					assert(0);
					break;
			}
			break;
		}
		case MODULE_ROOT:
		case MODULE_NAMED:
		default:
			assert(0);
			break;
	}
	return reg;
}

static int gen_apply(Generator *g, struct node *n)
{
	int lval = gen_node(g, n->o.apply.lval),
	    rval = gen_node(g, n->o.apply.rval);

	int rr = nextreg(g);

	bool tailcall = false;

	char *name = n->o.apply.lval->o.path.name->src;

	for (struct node *b = g->block; n == b->o.block.body->end->head; b = b->o.block.parent) {
		if (name == g->path->name || !strcmp(name, g->path->name)) { /* Tail-call */
			tailcall = true;
			break;
		}
	}

	if (tailcall) { /* Tail-call */
		gen(g, iABC(OP_TAILCALL, rr, 0, rval));
	} else {
		gen(g, iABC(OP_CALL, rr, lval, rval));
	}

	return rr;
}

static int gen_clause(Generator *g, struct node *n)
{
	int reg = -1;
	int rega = 0;

	ClauseEntry *old = g->path->clause;

	int index = g->path->nclauses;

	g->path->clause = clauseentry(n, g->path->nclauses);
	g->path->clauses[index] = g->path->clause;

	g->path->nclauses ++;

	enterscope(g->tree);
	gen_locals(g, n->o.clause.lval);
	reg = gen_block(g, n->o.clause.rval);
	exitscope(g->tree);

	if (iOP(g->path->clause->code[g->path->clause->pc - 1]) != OP_TAILCALL) {
		if (ISK(reg)) {
			rega = nextreg(g);
			gen(g, iAD(OP_LOADK, rega, reg));
			reg = rega;
		}
		gen(g, iABC(OP_RETURN, reg, 0, 0));
	}
	gen(g, 0); /* Terminator */

	if (old)
		g->path->clause = old;

	return index;
}

static int gen_ident(Generator *g, struct node *n)
{
	Sym *ident = tree_lookup(g->tree, n->src);

	if (ident) {
		return ident->e.var->reg;
	} else {
		return -1;
	}
}

static int gen_defined(Generator *g, struct node *n)
{
	int  reg;

	if ((reg = gen_ident(g, n)) == -1) {
		nreportf(REPORT_ERROR, n, ERR_UNDEFINED, n->src);
	}
	return reg;
}

/* TODO: Implement a node2tval function */
static struct tvalue *gen_pattern(Generator *g, struct node *n)
{
	struct tvalue *pattern;

	if (! n)
		return NULL;

	switch (n->op) {
		case OTUPLE: {
			int i = 0;
			pattern = tuple(n->o.tuple.arity);

			for (struct nodelist *ns = n->o.tuple.members ; ns ; ns = ns->tail) {
				pattern->v.tuple->members[i++] = *gen_pattern(g, ns->head);
			}
			break;
		}
		case ORANGE: {
			n = n->o.range.lval;
			int reg = gen_ident(g, n);

			if (reg >= 0) {
				pattern = tvalue(TYPE_VAR | Q_RANGE, (Value){ .ident = reg });
			} else {
				g->path->clause->nlocals ++;
				reg = define(g, n->src, nextreg(g));
				pattern = tvalue(TYPE_ANY | Q_RANGE, (Value){ .ident = reg });
			}
			break;
		}
		case OIDENT: {
			int reg = gen_ident(g, n);

			if (reg >= 0) {
				pattern = tvalue(TYPE_VAR, (Value){ .ident = reg });
			} else {
				g->path->clause->nlocals ++;
				reg = define(g, n->src, nextreg(g));
				pattern = tvalue(TYPE_ANY, (Value){ .ident = reg });
			}
			break;
		}
		case OATOM:
			pattern = atom(n->src);
			break;
		case ONUMBER:
			pattern = number(n->src);
			break;
		case OSTRING:
			assert(0);
		case OCONS:
			assert(0);
		case OLIST: {
			/*
			 * []         = <list>
			 * [X]        = <list> <any>
			 * [X, XS..]  = <list> <any> <any..>
			 *
			 */
			List *l = list_cons(NULL, NULL);

			if (n->o.list.length > 0) {
				assert(n->o.list.items->end);

				for (struct nodelist *ns = n->o.list.items ; ns ; ns = ns->tail) {
					l = list_cons(l, gen_pattern(g, ns->head));
				}
			}
			pattern = tvalue(TYPE_LIST, (Value){ .list = l });
			break;
		}
		default:
			pp_node(n);
			assert(0);
			break;
	}
	return pattern;
}

static int gen_select(Generator *g, struct node *n)
{
	struct nodelist *ns;
	struct node *arg = n->o.select.arg;

	unsigned result = nextreg(g), ret;
	unsigned long savedpc = -1, offset;

	int nclauses = n->o.select.nclauses;
	int patches[nclauses - 1];

	ClauseEntry *clause = g->path->clause;

	ns = n->o.select.clauses;

	/* Denotes whether or not this `select` node is the last value
	 * in the parent function, in which case it can just return
	 * from inside its clauses, instead of jumping outside. */
	bool islast = g->block->o.block.body->end->head == n &&
	              g->block == clause->node->o.clause.rval;

	for (int i = 0; i < nclauses; i++) {
		struct node *c = ns->head;

		int nguards = c->o.clause.nguards;
		int gpatches[nguards];

		enterscope(g->tree);

		/* If we have a pattern and an argument to match
		 * against it. */
		if (c->o.clause.lval && arg) {
			unsigned reg = nextreg(g);

			struct tvalue *pat = gen_pattern(g, c->o.clause.lval);

			OpCode op;

			switch (pat->t) {
				case TYPE_NUMBER:   op = OP_EQ;       break;
				case TYPE_ATOM:
				case TYPE_STRING:
				default:            op = OP_MATCH;
			}
			gen(g, iABC(op, reg, RKASK(gen_constant(g, NULL, pat)), gen_node(g, arg)));

			savedpc = g->path->clause->pc;
			gen(g, 0); /* Patched in [1] */
		}

		{ /* Guards */
			struct nodelist *ns = c->o.clause.guards;

			for (int i = 0; i < nguards; i++) {
				gen_node(g, ns->head);
				gpatches[i] = clause->pc;
				gen(g, 0); /* Patched in [0] */
				ns = ns->tail;
			}
		}

		/* Gen clause */

		enterscope(g->tree);
		ret = gen_block(g, c->o.clause.rval);
		exitscope(g->tree);

		if (ISK(ret))
			gen(g, iAD(OP_LOADK, result, ret));
		else
			gen(g, iABC(OP_MOVE, result, ret, 0));

		/* Create patch for [2] */
		if (i < nclauses - 1) {
			patches[i] = clause->pc;
			gen(g, 0);
		}

		if (c->o.clause.lval && arg) {
			offset = clause->pc - savedpc - 1;
			clause->code[savedpc] = iAJ(OP_JUMP, 0, offset); /* 1 */
		}

		for (int i = 0; i < nguards; i++) {
			clause->code[gpatches[i]] = /* 2 */
				iAJ(OP_JUMP, 0, clause->pc - gpatches[i] - 1);
		}
		exitscope(g->tree);

		ns = ns->tail;
	}
	/* [2] Patch clauses to skip over following clauses */
	for (int i = 0; i < nclauses - 1; i++) {
		clause->code[patches[i]] = islast ? iABC(OP_RETURN, result, 0, 0)
		                                  : iAJ(OP_JUMP, 0, clause->pc - patches[i] - 1);
	}
	return result;
}

static int gen_add(Generator *g, struct node *n)
{
	int lval = gen_node(g, n->o.add.lval),
	    rval = gen_node(g, n->o.add.rval);

	int reg = nextreg(g);

	gen(g, iABC(OP_ADD, reg, lval, rval));

	return reg;
}

static int gen_sub(Generator *g, struct node *n)
{
	int lval = gen_node(g, n->o.add.lval),
	    rval = gen_node(g, n->o.add.rval);

	int reg = nextreg(g);

	gen(g, iABC(OP_SUB, reg, lval, rval));

	return reg;
}

static int gen_gt(Generator *g, struct node *n)
{
	int lval = gen_node(g, n->o.cmp.lval),
	    rval = gen_node(g, n->o.cmp.rval);

	gen(g, iABC(OP_GT, 0, lval, rval));

	return -1;
}

static int gen_lt(Generator *g, struct node *n)
{
	int lval = gen_node(g, n->o.cmp.lval),
	    rval = gen_node(g, n->o.cmp.rval);

	gen(g, iABC(OP_GT, 0, rval, lval));

	return -1;
}

static void gen_locals(Generator *g, struct node *n)
{
	switch (n->op) {
		case OTUPLE:
			for (struct nodelist *ns = n->o.tuple.members ; ns ; ns = ns->tail) {
				gen_locals(g, ns->head);
			}
			break;
		case OIDENT: {
			Sym  *ident = tree_lookup(g->tree, n->src);

			if (! ident) {
				g->path->clause->nlocals ++;
				define(g, n->src, nextreg(g));
			}
			break;
		}
		case ONUMBER:
		case OATOM:
		case OSTRING:
			gen_node(g, n);
			break;
		default:
			// Ignore
			break;
	}
}

static int gen_path(Generator *g, struct node *n)
{
	char *name = n->o.path.name->src;

	Sym *k = symtab_lookup(g->tree->psymbols, name);

	if (k) {
		nreportf(REPORT_ERROR, n, "path '%s' already defined.", name);
		exit(1);
	}

	g->path = g->paths[g->pathsn] = pathentry(name, n, g->pathsn);

	symtab_insert(g->tree->psymbols, name, psymbol(name, g->path));

	g->pathsn ++;

	return gen_clause(g, n->o.path.clause);
}

static int gen_num(Generator *g, struct node *n)
{
	// TODO: Use number(const char *)

	int number = atoi(n->src);

	Value v = (Value){ .number = number };

	struct tvalue *tval = tvalue(TYPE_NUMBER, v);

	// TODO: Think about inlining small numbers

	return RKASK(gen_constant(g, n->src, tval));
}

static int gen_tuple(Generator *g, struct node *n)
{
	struct nodelist *ns;

	unsigned reg = nextreg(g);

	gen(g, iABC(OP_TUPLE, reg, n->o.tuple.arity, 0));

	ns = n->o.tuple.members;
	for (int i = 0; i < n->o.tuple.arity; i++) {
		gen(g, iABC(OP_SETTUPLE, reg, i, gen_node(g, ns->head)));
		ns = ns->tail;
	}
	return reg;
}

static int gen_list(Generator *g, struct node *n)
{
	unsigned reg = nextreg(g);

	gen(g, iABC(OP_LIST, reg, 0, 0));

	return reg;
}

static int gen_cons(Generator *g, struct node *n)
{
	struct node *lval = n->o.cons.lval,
	            *rval = n->o.cons.rval;

	unsigned reg = -1;

	if (rval == NULL) {
		reg = nextreg(g);
		gen(g, iABC(OP_LIST, reg, 0, 0));
	} else {
		reg = gen_cons(g, rval);
	}

	if (lval)
		gen(g, iABC(OP_CONS, reg, reg, gen_node(g, lval)));

	return reg;
}

static int gen_bind(Generator *g, struct node *n)
{
	struct node *lval = n->o.match.lval,
	            *rval = n->o.match.rval;

	int lreg, rreg;

	switch (rval->op) {
		case OIDENT:
			rreg = gen_defined(g, rval);
			if (rreg == -1) {
				return rreg;
			}
			break;
		case OTUPLE:
			rreg = gen_tuple(g, rval);
			break;
		default:
			rreg = gen_node(g, rval);
			break;
	}

	if (lval->op == OIDENT) {
		Sym  *ident = tree_lookup(g->tree, lval->src);

		g->path->clause->nlocals ++;

		if (ident) {
			nreportf(REPORT_ERROR, n, ERR_REDEFINITION, lval->src);
		} else { /* Create variable binding */
			lreg = define(g, lval->src, nextreg(g));
			if (ISK(rreg)) {
				gen(g, iAD(OP_LOADK, lreg, rreg));
			} else {
				gen(g, iABC(OP_MOVE, lreg, rreg, 0));
			}
		}
	} else {
		// TODO
	}
	return 0;
}

static int gen_match(Generator *g, struct node *n)
{
	struct node *lval = n->o.match.lval,
	            *rval = n->o.match.rval;

	int larg = gen_node(g, lval),
	    rarg = gen_node(g, rval);

	gen(g, iABC(OP_MATCH, 0, larg, rarg));
	gen(g, iAJ(OP_JUMP, 0, 0));

	// TODO: gen error in case of bad-match

	return 0;
}

static int gen_node(Generator *g, struct node *n)
{
	return OP_GENERATORS[n->op](g, n);
}

static void dump_atom(struct node *n, FILE *out)
{
	fputc(strlen(n->o.atom) + 1, out);
	fwrite(n->o.atom, strlen(n->o.atom), 1, out);
	fputc('\0', out);
}

static void dump_number(struct node *n, FILE *out)
{
	int i = atoi(n->o.number);
	fwrite(&i, sizeof(i), 1, out);
}

static void dump_node(struct node *n, FILE *out)
{
	struct nodelist *ns;

	fputc(OP_TYPES[n->op], out);

	switch (n->op) {
		case OTUPLE:
			fputc(n->o.tuple.arity, out);
			for (ns = n->o.tuple.members ; ns ; ns = ns->tail) {
				dump_node(ns->head, out);
			}
			break;
		case OIDENT:
			/* The type is all that matters (TYPE_ANY) */

			/* TODO: May not be the case with:
			 *
			 *     fnord (X, X): ...
			 *
			 * Then should probably use TYPE_VAR. But
			 * if reduced to OSELECT, should be ok.
			 */
			break;
		case OATOM:
			dump_atom(n, out);
			break;
		case ONUMBER:
			dump_number(n, out);
			break;
		default:
			assert(0);
			break;
	}
}

static void dump_pattern(struct node *pattern, FILE *out)
{
	dump_node(pattern, out);
}

static void dump_constant(struct tvalue *tval, FILE *out)
{
	/* Write constant type */
	fputc(tval->t, out);

	/* Write constant value */
	switch (tval->t & TYPE_MASK) {
		case TYPE_PATHID:
			fwrite(tval->v.pathid->module, strlen(tval->v.pathid->module), 1, out);
			fputc('\0', out);
			fwrite(tval->v.pathid->path,   strlen(tval->v.pathid->path), 1, out);
			fputc('\0', out);
			break;
		case TYPE_BIN:
		case TYPE_STRING:
			assert(0);
			break;
		case TYPE_TUPLE: {
			uint8_t arity = tval->v.tuple->arity;
			fputc(arity, out);
			for (int i = 0; i < arity; i++) {
				dump_constant(&tval->v.tuple->members[i], out);
			}
			break;
		}
		case TYPE_LIST: {
			size_t len = 0;

			for (List *l = tval->v.list; l; l = l->tail)
				len ++;

			len --; // Account for empty list

			fwrite(&len, sizeof(len), 1, out);

			List *l = tval->v.list;
			for (int i = 0; i < len; i++) {
				dump_constant(l->head, out);
				l = l->tail;
			}
			break;
		}
		case TYPE_ATOM:
			fwrite(tval->v.atom, strlen(tval->v.atom) + 1, 1, out);
			break;
		case TYPE_NUMBER:
			fwrite(&tval->v.number, sizeof(int), 1, out);
			break;
		case TYPE_VAR:
		case TYPE_ANY:
			fwrite(&tval->v.ident, sizeof(tval->v.ident), 1, out);
			break;
		default:
			assert(0);
			break;
	}
}

static void dump_clause(ClauseEntry *c, FILE *out)
{
	struct tvalue *tval;

	/* Write clause pattern */
	dump_pattern(c->node->o.clause.lval, out);

	/* Write local variable count */
	fputc(c->nreg, out);

	/* Write table entry count */
	fputc(c->kindex, out);

	/* Write header */
	for (int i = 0; i < c->kindex; i++) {
		tval = c->kheader[i];

		/* Write constant */
		dump_constant(tval, out);
	}
	/* Write byte-code length */
	fwrite(&c->pc, sizeof(c->pc), 1, out);

	/* Write byte-code */
	fwrite(c->code, sizeof(Instruction), c->pc, out);

	for (int i = 0; i < c->pc; i++) {
		if (c->code[i])
			printf("%3d:\t", i), op_pp(c->code[i]), putchar('\n');
		else
			printf("%3d:\n", i);
	}
}

static void dump_path(PathEntry *p, FILE *out)
{
	/* Write path attributes */
	fputc(0xff, out);

	if (false) { /* TODO: Anonymous path */
		fputc(0, out);
	} else {
		/* Write path name */
		uint8_t len = (uint8_t)strnlen(p->name, UINT8_MAX);
		fputc(len, out);
		fwrite(p->name, len, 1, out);
	}

	/* Write clause entry count */
	fputc(p->nclauses, out);

	printf("/%s:\n", p->name);

	for (int i = 0; i < p->nclauses; i++) {
		dump_clause(p->clauses[i], out);
		puts("-");
	}
}

static int gen(Generator *g, Instruction i)
{
	ClauseEntry *clause = g->path->clause;

	if (clause->pc == clause->codesize - 1) {
		clause->codesize += 4096;
		clause->code      = realloc(clause->code, clause->codesize);
	}
	clause->code[clause->pc] = i;

	return clause->pc++;
}
