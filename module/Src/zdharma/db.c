/* -*- Mode: C; c-basic-offset: 4 -*-
 * vim:sw=4:sts=4:et
 */

/*
 * db.c - general database module, forwards to backends
 *
 * Copyright (c) 2017 Sebastian Gniazdowski
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 */

#include "db.mdh"
#include "db.pro"

/* MACROS {{{ */
#ifndef PM_UPTODATE
#define PM_UPTODATE     (1<<19) /* Parameter has up-to-date data (e.g. loaded from DB) */
#endif

/* Backend commands */
#define DB_TIE 1
#define DB_UNTIE 2
#define DB_IS_TIED 3
#define DB_GET_ADDRESS 4
#define DB_CLEAR_CACHE 5

#define RESET         "\033[m"
#define BOLD          "\033[1m"
#define RED           "\033[31m"
#define GREEN         "\033[32m"
#define YELLOW        "\033[33m"
#define BLUE          "\033[34m"
#define MAGENTA       "\033[35m"
#define CYAN          "\033[36m"
#define BOLD_RED      "\033[1;31m"
#define BOLD_GREEN    "\033[1;32m"
#define BOLD_YELLOW   "\033[1;33m"
#define BOLD_BLUE     "\033[1;34m"
#define BOLD_MAGENTA  "\033[1;35m"
#define BOLD_CYAN     "\033[1;36m"
#define BG_RED        "\033[41m"
#define BG_GREEN      "\033[42m"
#define BG_YELLOW     "\033[43m"
#define BG_BLUE       "\033[44m"
#define BG_MAGENTA    "\033[45m"
#define BG_CYAN       "\033[46m"
/* }}} */
/* DECLARATIONS {{{ */
static char *unmetafy_zalloc(const char *to_copy, int *new_len);
static void set_length(char *buf, int size);

static void ztie_usage();
static void zuntie_usage();
static void ztaddress_usage();
static void ztclear_usage();

static HashTable createhashtable(char *name);
static void freebackendnode(HashNode hn);
static void backend_scan_fun(HashNode hn, int unused);

/* Type of provided (by backend module) entry-point */
typedef int (*DbBackendEntryPoint)(VA_ALIST1(int cmd));

struct backend_node {
    struct hashnode node;
    DbBackendEntryPoint main_entry;
};

typedef struct backend_node *BackendNode;

/* Maps db/dbtype onto BackendNode */
static HashTable backends_hash = NULL;

/* For searching with scanhashtable */
char *In_ParamName = NULL;
DbBackendEntryPoint Out_FoundBe = NULL;
/* }}} */
/* ARRAY: builtin {{{ */
static struct builtin bintab[] = {
                                  /* h - help, d - backend type, r - read-only, a/f - address/file,
                                   * l - load password from terminal, p - password as argument,
                                   * P - password from file, z - zero read-cache */
                                  BUILTIN("ztie", 0, bin_ztie, 0, -1, 0, "hrlzf:d:a:p:P:", NULL),
                                  BUILTIN("zuntie", 0, bin_zuntie, 0, -1, 0, "uh", NULL),
                                  BUILTIN("ztaddress", 0, bin_ztaddress, 0, -1, 0, "h", NULL),
                                  BUILTIN("ztclear", 0, bin_ztclear, 0, -1, 0, "h", NULL),
};
/* }}} */
/* ARRAY: other {{{ */
#define ROARRPARAMDEF(name, var)                                        \
    { name, PM_ARRAY | PM_READONLY, (void *) var, NULL,  NULL, NULL, NULL }
/* }}} */

/* FUNCTION: zsh_db_register_backend {{{ */

/**/
void
zsh_db_register_backend(char *id, void *entry_point) {
    BackendNode bn = (BackendNode)zshcalloc(sizeof(struct backend_node));
    if (bn) {
        bn->main_entry = entry_point;
        addhashnode(backends_hash, ztrdup(id), (void *)bn);
    } else {
        zwarn("Out of memory when allocating backend entry");
    }
}
/* }}} */
/* FUNCTION: zsh_db_unregister_backend {{{ */

/**/
void
zsh_db_unregister_backend(char *id) {
    HashNode bn = backends_hash->removenode(backends_hash, id);
    if (bn) {
        freebackendnode(bn);
    }
}
/* }}} */
/* FUNCTION: bin_ztie {{{ */

/**/
static int
bin_ztie(char *nam, char **args, Options ops, UNUSED(int func))
{
    char *pmname;

    /* Check options */

    if (OPT_ISSET(ops,'h')) {
        ztie_usage();
        return 0;
    }

    if (!OPT_ISSET(ops,'d')) {
        zwarnnam(nam, "you must pass e.g. `-d db/redis', see `-h'");
        return 1;
    }

    if (!OPT_ISSET(ops,'f') && !OPT_ISSET(ops,'a')) {
        zwarnnam(nam, "you must pass `-f' (file, or `-a', address) with e.g. {host}[:port][/[db_idx][/key]], see `-h'");
        return 1;
    }

    if (0 != strcmp(OPT_ARG(ops, 'd'), "db/redis") && 0 != strcmp(OPT_ARG(ops, 'd'), "db/gdbm") ) {
        zwarnnam(nam, "Unsupported backend type `%s', see `-h'", OPT_ARG(ops, 'd'));
        return 1;
    }

    /* Check argument */
    pmname = *args;

    if (!pmname) {
        zwarnnam(nam, "You must pass non-option argument - the target parameter to create, see -h");
        return 1;
    }

    /* Prepare arguments for backend */

    char *address = NULL, *pass = NULL, *pfile = NULL;
    int rdonly = 0, zcache = 0, pprompt = 0;

    /* Address */
    if (OPT_ISSET(ops,'f')) {
        address = OPT_ARG(ops,'f');
    } else {
        address = OPT_ARG(ops,'a');
    }

    /* Read-only */
    if (OPT_ISSET(ops,'r')) {
        rdonly = 1;
    } else {
        rdonly = 0;
    }

    /* Zero-cache */
    if (OPT_ISSET(ops,'z')) {
        zcache = 1;
    } else {
        zcache = 0;
    }

    /* Password */
    if (OPT_ISSET(ops,'p')) {
        pass = OPT_ARG(ops,'p');
    }

    /* Password file */
    if (OPT_ISSET(ops,'P')) {
        pfile = OPT_ARG(ops,'P');
    }

    /* Password load request */
    if (OPT_ISSET(ops,'l')) {
        pprompt = 1;
    } else {
        pprompt = 0;
    }

    BackendNode node = NULL;
    DbBackendEntryPoint be = NULL;

    if(!(node = (BackendNode) gethashnode2(backends_hash, OPT_ARG(ops, 'd')))) {
        zwarnnam(nam, "Backend module for %s not loaded (or loaded before the main `db' module)", OPT_ARG(ops, 'd'));
        return 1;
    }

    be = node->main_entry;
    if (!be) {
        zwarnnam(nam, "Backend for %s is uninitialized", OPT_ARG(ops, 'd'));
        return 1;
    }

    return be(DB_TIE, address, rdonly, zcache, pass, pfile, pprompt, pmname);
}
/* }}} */
/* FUNCTION: bin_zuntie {{{ */

/**/
static int
bin_zuntie(char *nam, char **args, Options ops, UNUSED(int func))
{
    char *pmname;
    int ret = 0;

    if (OPT_ISSET(ops,'h')) {
        zuntie_usage();
        return 0;
    }

    if (!*args) {
        zwarnnam(nam, "At least one variable name is needed, see -h");
        return 1;
    }

    for (pmname = *args; *args++; pmname = *args) {
        In_ParamName = pmname;
        Out_FoundBe = NULL;

        scanhashtable(backends_hash, 0, 0, 0, backend_scan_fun, 0);

        if (!Out_FoundBe) {
            zwarnnam(nam, "Didn't recognize `%s' as a tied parameter", pmname);
            continue;
        }

        int rountie = 0;

        if (OPT_ISSET(ops,'u')) {
            rountie = 1;
        }

        ret = Out_FoundBe(DB_UNTIE, rountie, pmname) ? 1 : ret;
    }

    return ret;
}
/* }}} */
/* FUNCTION: bin_ztaddress {{{ */

/**/
static int
bin_ztaddress(char *nam, char **args, Options ops, UNUSED(int func))
{
    char *pmname;
    int ret = 0;

    if (OPT_ISSET(ops,'h')) {
        ztaddress_usage();
        return 0;
    }

    if (!*args) {
        zwarnnam(nam, "One parameter name is needed, see -h");
        return 1;
    }

    pmname = *args;
    In_ParamName = pmname;
    Out_FoundBe = NULL;

    scanhashtable(backends_hash, 0, 0, 0, backend_scan_fun, 0);

    if (!Out_FoundBe) {
        zwarnnam(nam, "Didn't recognize `%s' as a tied parameter", pmname);
        return 1;
    }

    ret = Out_FoundBe(DB_GET_ADDRESS, pmname) ? 1 : ret;

    return ret;
}
/* }}} */
/* FUNCTION: bin_ztclear {{{ */

/**/
static int
bin_ztclear(char *nam, char **args, Options ops, UNUSED(int func))
{
    char *pmname, *key;
    int ret = 0;

    if (OPT_ISSET(ops,'h')) {
        ztclear_usage();
        return 0;
    }

    if (!*args) {
        zwarnnam(nam, "One-to-two parameters' names are needed, see -h");
        return 1;
    }

    pmname = *args;
    key = *(args+1);

    In_ParamName = pmname;
    Out_FoundBe = NULL;

    scanhashtable(backends_hash, 0, 0, 0, backend_scan_fun, 0);

    if (!Out_FoundBe) {
        zwarnnam(nam, "Didn't recognize `%s' as a tied parameter", pmname);
        return 1;
    }

    ret = Out_FoundBe(DB_CLEAR_CACHE, pmname, key) ? 1 : ret;

    return ret;
}
/* }}} */

/* FUNCTION: ztaddress_usage {{{ */

static void
ztaddress_usage()
{
    fprintf(stdout, YELLOW "Usage:" RESET " ztaddress {tied-parameter-name}\n");
    fprintf(stdout, YELLOW "Description:" RESET " stores address used by given parameter to $REPLY\n");
    fflush(stdout);
}
/* }}} */
/* FUNCTION: ztclear_usage {{{ */

static void
ztclear_usage()
{
    fprintf(stdout, YELLOW "Usage:" RESET " ztclear {tied-parameter-name} [key name]\n");
    fprintf(stdout, YELLOW "Description:" RESET " clears cache of given hash/key or of given plain\n");
    fprintf(stdout, YELLOW "            " RESET " parameter: set (array), list (array), string (scalar);\n");
    fprintf(stdout, YELLOW "            " RESET " pass `-z' to ztie to globally disable cache for parameter\n");
    fflush(stdout);
}
/* }}} */

/*************** MAIN CODE ***************/

/* ARRAY features {{{ */
static struct features module_features =
{
    bintab, sizeof(bintab)/sizeof(*bintab),
    NULL, 0,
    NULL, 0,
    NULL, 0,
    0
};
/* }}} */

/* FUNCTION: setup_ {{{ */

/**/
int
setup_(UNUSED(Module m))
{
    /* Create hash */
    if (!(backends_hash = createhashtable("ZSH_BACKENDS"))) {
        zwarn("Cannot create backend-register hash");
        return 1;
    }
    return 0;
}
/* }}} */
/* FUNCTION: features_ {{{ */

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}
/* }}} */
/* FUNCTION: enables_ {{{ */

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}
/* }}} */
/* FUNCTION: boot_ {{{ */

/**/
int
boot_(UNUSED(Module m))
{
    return 0;
}
/* }}} */
/* FUNCTION: cleanup_ {{{ */

/**/
int
cleanup_(Module m)
{
    /* This frees `zredis_tied` */
    return setfeatureenables(m, &module_features, NULL);

    if (backends_hash) {
        deletehashtable(backends_hash);
        backends_hash = NULL;
    }
}
/* }}} */
/* FUNCTION: finish_ {{{ */

/**/
int
finish_(UNUSED(Module m))
{
    return 0;
}
/* }}} */

/*************** UTILITIES ***************/

/* FUNCTION: unmetafy_zalloc {{{ */

/*
 * Unmetafy that:
 * - duplicates bufer to work on it,
 * - does zalloc of exact size for the new string,
 * - restores work buffer to original content, to restore strlen
 *
 * No zsfree()-confusing string will be produced.
 */

static char *
unmetafy_zalloc(const char *to_copy, int *new_len)
{
    char *work, *to_return;
    int my_new_len = 0;

    work = ztrdup(to_copy);
    work = unmetafy(work, &my_new_len);

    if (new_len)
        *new_len = my_new_len;

    /* This string can be correctly zsfree()-d */
    to_return = (char *) zalloc((my_new_len+1)*sizeof(char));
    memcpy(to_return, work, sizeof(char)*my_new_len); // memcpy handles $'\0'
    to_return[my_new_len]='\0';

    /* Restore original strlen and correctly free */
    strcpy(work, to_copy);
    zsfree(work);

    return to_return;
}
/* }}} */
/* FUNCTION: set_length {{{ */

/* For zsh-allocator, rest of Zsh seems to use
 * free() instead of zsfree(), and such length
 * restoration causes slowdown, but all is this
 * way strict - correct */

static void
set_length(char *buf, int size)
{
    buf[size]='\0';
    while (-- size >= 0) {
        buf[size]=' ';
    }
}
/* }}} */
/* FUNCTION: createhash {{{ */
static HashTable
createhashtable(char *name)
{
    HashTable ht;

    ht = newhashtable(8, name, NULL);

    ht->hash        = hasher;
    ht->emptytable  = emptyhashtable;
    ht->filltable   = NULL;
    ht->cmpnodes    = strcmp;
    ht->addnode     = addhashnode;
    ht->getnode     = gethashnode2;
    ht->getnode2    = gethashnode2;
    ht->removenode  = removehashnode;
    ht->disablenode = NULL;
    ht->enablenode  = NULL;
    ht->freenode    = freebackendnode;
    ht->printnode   = NULL;

    return ht;
}
/* }}} */
/* FUNCTION: freebackendnode {{{ */
static void
freebackendnode(HashNode hn)
{
    zsfree(hn->nam);
    zfree(hn, sizeof(struct backend_node));
}
/* }}} */
/* FUNCTION: backend_scan_fun {{{ */
static void
backend_scan_fun(HashNode hn, int unused)
{
    BackendNode bn = (BackendNode)hn;
    DbBackendEntryPoint be = bn->main_entry;
    if (!be) {
        zwarn("Backend %s registered but uninitialized", hn->nam);
        return;
    }
    /* 0 - shell true value */
    if(0 == be(DB_IS_TIED, In_ParamName)) {
        Out_FoundBe = be;
    }
}
/* }}} */

/***************** USAGE *****************/

/* FUNCTION: ztie_usage {{{ */

static void
ztie_usage()
{
    fprintf(stdout, YELLOW "Usage:" RESET " ztie -d db/... [-z] [-r] [-p password] [-P password_file] "
            MAGENTA "-f/-a {db_address}" RESET " " RED "{parameter_name}" RESET "\n");
    fprintf(stdout, YELLOW "Options:" RESET "\n");
    fprintf(stdout, GREEN " -d" RESET ":       select database type: \"db/gdbm\", \"db/redis\"\n");
    fprintf(stdout, GREEN " -z" RESET ":       zero-cache for read operations (always access database)\n");
    fprintf(stdout, GREEN " -r" RESET ":       create read-only parameter\n" );
    fprintf(stdout, GREEN " -f or -a" RESET ": database-address in format {host}[:port][/[db_idx][/key]] or a file path\n");
    fprintf(stdout, GREEN " -p" RESET ":       database-password to be used for authentication\n");
    fprintf(stdout, GREEN " -P" RESET ":       path to file with database-password\n");
    fprintf(stdout, "The " RED "{parameter_name}" RESET " - choose name for the created database-bound parameter\n");
    fflush(stdout);
}
/* }}} */
/* FUNCTION: zuntie_usage {{{ */

static void
zuntie_usage()
{
    fprintf(stdout, YELLOW "Usage:" RESET " zuntie [-u] {tied-variable-name} [tied-variable-name] ...\n");
    fprintf(stdout, YELLOW "Options:" RESET "\n");
    fprintf(stdout, GREEN " -u" RESET ": Allow to untie read-only parameter\n");
    fprintf(stdout, YELLOW "Description:" RESET " detaches variable from its database and removes the variable;\n");
    fprintf(stdout, YELLOW "            " RESET " database is not cleared (unlike when unset)\n");
    fflush(stdout);
}
/* }}} */

