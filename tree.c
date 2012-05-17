/*
 * arbre
 *
 * (c) 2011, Alexis Sellier
 *
 * tree.c
 *
 *   abstract syntax tree
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>

#include "arbre.h"
#include "color.h"
#include "util.h"

/*
 * Tree allocator/initializer
 */
Tree *tree(void)
{
    Tree *t = malloc(sizeof(*t));
    Node *b = calloc(1, sizeof(*b));

    b->o.block.body  = nodelist(NULL);

    t->root      = b;
    t->symbols   = symtab(1024);
    t->psymbols  = symtab(512);
    t->tsymbols  = symtab(512);

    return t;
}

/*
 * Tree deallocator
 */
void tree_free(Tree *t)
{
    node_free(t->root);
    symtab_free(t->symbols);
    symtab_free(t->tsymbols);
    symtab_free(t->psymbols);
    free(t);
}


Sym *tree_lookup(Tree *t, char *k)
{
    return symtab_lookup(t->symbols, k);
}

/*
 * Enter a lexical scope
 */
void enterscope(Tree *t)
{
    SymTable *tab  = symtab(1024);

    tab->parent  = t->symbols;
    t->symbols   = tab;
}

/*
 * Exit the current lexical scope
 */
void exitscope(Tree *t)
{
    t->symbols  = t->symbols->parent;
    /* TODO: free tables */
}

/*
 * Print tree
 */
void pp_tree(Tree *t)
{
    pp_node(t->root);
}

/*
 * Path-entry allocator
 */
PathEntry *pathentry(char *name, Node *n, uint8_t index)
{
    PathEntry  *p = malloc(sizeof(*p));
                p->name      = name;
                p->node      = n;
                p->index     = index;
                p->kheader   = calloc(128,  sizeof(TValue*));
                p->ktable    = symtab(128);
                p->kindex    = 0;
                p->nlocals   = 0;
                p->pc        = 0;
                p->code      = calloc(4096, sizeof(uint32_t));
                p->codesize  = 4096;
    return      p;
}

