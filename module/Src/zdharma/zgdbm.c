/* -*- Mode: C; c-default-style: "linux"; c-basic-offset: 4; indent-tabs-mode: nil -*-
 * vim:sw=4:sts=4:et
 */

/*
 * zgdbm.c - bindings for gdbm
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 2008 Clint Adams
 * All rights reserved.
 *
 * Modifications copyright (c) 2017 Sebastian Gniazdowski
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Clint Adams or the Zsh Development
 * Group be liable to any party for direct, indirect, special, incidental, or
 * consequential damages arising out of the use of this software and its
 * documentation, even if Peter Stephenson, Sven Wischnowsky and the Zsh
 * Development Group have been advised of the possibility of such damage.
 *
 * Clint Adams and the Zsh Development Group
 * specifically disclaim any warranties, including, but not limited to, the
 * implied warranties of merchantability and fitness for a particular purpose.
 * The software provided hereunder is on an "as is" basis, and Peter
 * Stephenson, Sven Wischnowsky and the Zsh Development Group have no
 * obligation to provide maintenance, support, updates, enhancements, or
 * modifications.
 *
 */

#include "zgdbm.mdh"
#include "zgdbm.pro"
#include "db.epro"
#include "db.h"

/* MACROS {{{ */
#ifndef PM_UPTODATE
#define PM_UPTODATE     (1<<19) /* Parameter has up-to-date data (e.g. loaded from DB) */
#endif
/* }}} */
/* DECLARATIONS {{{ */
static Param createhash( char *name, int flags );
static int is_tied_cmd(char *pmname);
static int is_tied(Param pm);

static int no_database_action = 0;
/* }}} */

/*
 * Make sure we have all the bits I'm using for memory mapping, otherwise
 * I don't know what I'm doing.
 */
#if defined(HAVE_GDBM_H) && defined(HAVE_GDBM_OPEN)

#include <gdbm.h>

/* ARRAY: GSU {{{ */

/*
 * Longer GSU structure, to carry GDBM_FILE of owning
 * database. Every parameter (hash value) receives GSU
 * pointer and thus also receives GDBM_FILE - this way
 * parameters can access proper database.
 *
 * Main HashTable parameter has the same instance of
 * the custom GSU struct in u.hash->tmpdata field.
 * When database is closed, `dbf` field is set to NULL
 * and hash values know to not access database when
 * being unset (total purge at zuntie).
 *
 * When database closing is ended, custom GSU struct
 * is freed. Only new ztie creates new custom GSU
 * struct instance.
 */

struct gsu_scalar_ext {
    struct gsu_scalar std; /* Size of three pointers */
    int type;
    int use_cache;
    int is_lazy;
    char *dbfile_path;
    char *key;
    size_t key_len;
    char *password;
    int fdesc;
    GDBM_FILE dbf; /* a pointer */
};

/* Source structure - will be copied to allocated one,
 * with `dbf` filled. `dbf` allocation <-> gsu allocation. */
static const struct gsu_scalar_ext gdbm_gsu_ext =
    { { gdbmgetfn, gdbmsetfn, gdbmunsetfn }, 0, 0 };

/**/
static const struct gsu_hash gdbm_hash_gsu =
    { hashgetfn, gdbmhashsetfn, gdbmhashunsetfn };

#define ROARRPARAMDEF(name, var) \
    { name, PM_ARRAY | PM_READONLY, (void *) var, NULL,  NULL, NULL, NULL }

/* Holds names of all tied parameters */
char **zgdbm_tied;

static struct paramdef patab[] =
    { ROARRPARAMDEF( "zgdbm_tied", &zgdbm_tied ), };
/* }}} */

/* FUNCTION: gdbm_main_entry {{{ */
static int
gdbm_main_entry(VA_ALIST1(int cmd))
    VA_DCL
{
    char *address = NULL, *pass = NULL, *pfile = NULL, *pmname = NULL, *key = NULL, *lazy = NULL;
    int rdonly = 0, zcache = 0, pprompt = 0, rountie = 0;

    va_list ap;
    VA_DEF_ARG(int cmd);

    VA_START(ap, cmd);
    VA_GET_ARG(ap, cmd, int);

    switch (cmd) {
    case DB_TIE:
        /* Order is:
         * -a/f address, char *
         * -r read-only, int
         * -z zero-cache, int
         * -p password, char *
         * -P file with password, char *
         * -l prompt for password, int
         * -L lazy binding type, char *
         * parameter name, char *
         */
        address = va_arg(ap, char *);
        rdonly = va_arg(ap, int);
        zcache = va_arg(ap, int);
        pass = va_arg(ap, char *);
        pfile = va_arg(ap, char *);
        pprompt = va_arg(ap, int);
        pmname = va_arg(ap, char *);
        lazy = va_arg(ap, char *);
        return zgtie_cmd(address, rdonly, zcache, pass, pfile, pprompt, pmname, lazy);

    case DB_UNTIE:
        /* Order is:
         * -u untie read only parameter, int
         * parameter name, char *
         */
        rountie = va_arg(ap, int);
        pmname = va_arg(ap, char *);
        char *argv[2];
        argv[0] = pmname;
        argv[1] = NULL;
        return zguntie_cmd(rountie, argv);

    case DB_IS_TIED:
        /* Order is:
         * parameter name, char *
         */
        pmname = va_arg(ap, char *);
        return is_tied_cmd(pmname);

    case DB_GET_ADDRESS:
        /* Order is:
         * Parameter name, char *
         */
        pmname = va_arg(ap, char*);
        return zgdbmpath_cmd(pmname);

    case DB_CLEAR_CACHE:
        /* Order is:
         * Parameter name, char *
         * key name, char *
         */
        pmname = va_arg(ap, char*);
        key = va_arg(ap, char*);
        return zgdbmclear_cmd(pmname, key);

    default:
#ifdef DEBUG
        dputs("Bad command %d in redis_main_entry", cmd);
#endif
        break;
    }
    return 1;
}
/* }}} */
/* FUNCTION: zgtie_cmd {{{ */

/**/
static int
zgtie_cmd(char *address, int rdonly, int zcache, char *pass, char *pfile, int pprompt, char *pmname, char *lazy)
{
    GDBM_FILE dbf = NULL;
    int read_write = GDBM_SYNC, pmflags = PM_REMOVABLE;
    Param tied_param;

    if (!address) {
        zwarn("you must pass `-f' or '-a' path to the database", NULL);
        return 1;
    }

    if (!pmname) {
        zwarn("you must pass non-option argument - the target parameter to create, see -h");
        return 1;
    }

    if (rdonly) {
        read_write |= GDBM_READER;
        pmflags |= PM_READONLY;
    } else {
        read_write |= GDBM_WRCREAT;
    }

    if ((tied_param = (Param)paramtab->getnode(paramtab, pmname)) &&
        !(tied_param->node.flags & PM_UNSET)) {
        /*
         * Unset any existing parameter.  Note there's no implicit
         * "local" here, but if the existing parameter is local
         * then new parameter will be also local without following
         * unset.
         *
         * We need to do this before attempting to open the DB
         * in case this variable is already tied to a DB.
         *
         * This can fail if the variable is readonly or restricted.
         * We could call unsetparam() and check errflag instead
         * of the return status.
         */
        if (unsetparam_pm(tied_param, 0, 1))
            return 1;
    }

    gdbm_errno=0;
    dbf = gdbm_open(address, 0, read_write, 0666, 0);
    if(dbf == NULL) {
        zwarn("error opening database file %s (%s)", address, gdbm_strerror(gdbm_errno));
        return 1;
    }

    if (!(tied_param = createhash(pmname, pmflags))) {
        zwarn("cannot create the requested parameter %s", pmname);
        gdbm_close(dbf);
        return 1;
    }

    tied_param->gsu.h = &gdbm_hash_gsu;

    /* Allocate parameter sub-gsu, fill dbf field.
     * dbf allocation is 1 to 1 accompanied by
     * gsu_scalar_ext allocation. */

    struct gsu_scalar_ext *dbf_carrier = (struct gsu_scalar_ext *) zshcalloc(sizeof(struct gsu_scalar_ext));
    dbf_carrier->std = gdbm_gsu_ext.std;
    dbf_carrier->type = DB_KEY_TYPE_NO_KEY;
    dbf_carrier->fdesc = gdbm_fdesc(dbf);
    dbf_carrier->dbf = dbf;
    dbf_carrier->use_cache = 1;
    if (zcache) {
        dbf_carrier->use_cache = 0;
    }
    if (lazy) {
        dbf_carrier->is_lazy = 1;
    }
    tied_param->u.hash->tmpdata = (void *)dbf_carrier;

    /* Fill also file path field */
    if (*address != '/') {
        /* Code copied from check_autoload() */
        address = zhtricat(metafy(zgetcwd(), -1, META_HEAPDUP), "/", address);
        address = xsymlink(address, 1);
    }
    dbf_carrier->dbfile_path = ztrdup(address);

    addmodulefd(dbf_carrier->fdesc, FDT_INTERNAL);
    zsh_db_arr_append(&zgdbm_tied, pmname);

    return 0;
}
/* }}} */
/* FUNCTION: zguntie_cmd {{{ */

/**/
static int
zguntie_cmd(int rountie, char **args)
{
    Param pm;
    char *pmname;
    int ret = 0;

    for (pmname = *args; *args++; pmname = *args) {
        pm = (Param) paramtab->getnode(paramtab, pmname);
        if(!pm) {
            zwarn("cannot untie %s", pmname);
            ret = 1;
            continue;
        }
        if (pm->gsu.h != &gdbm_hash_gsu) {
            zwarn("not a tied gdbm hash: %s", pmname);
            ret = 1;
            continue;
        }

        queue_signals();
        if (rountie) {
            pm->node.flags &= ~PM_READONLY;
        }
        if (unsetparam_pm(pm, 0, 1)) {
            /* assume already reported */
            ret = 1;
        }
        unqueue_signals();
    }

    return ret;
}
/* }}} */
/* FUNCTION: zgdbmpath_cmd{{{ */

/**/
static int
zgdbmpath_cmd(char *pmname)
{
    Param pm;

    if (!pmname) {
        zwarn("parameter name (whose path is to be written to $REPLY) not given");
        return 1;
    }

    pm = (Param) paramtab->getnode(paramtab, pmname);
    if(!pm) {
        zwarn("no such parameter: %s", pmname);
        return 1;
    }

    if (pm->gsu.h != &gdbm_hash_gsu) {
        zwarn("not a tied gdbm parameter: %s", pmname);
        return 1;
    }

    /* Paranoia, it *will* be always set */
    if (((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->dbfile_path) {
        setsparam("REPLY", ztrdup(((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->dbfile_path));
    } else {
        setsparam("REPLY", ztrdup(""));
    }

    return 0;
}
/* }}} */
/* FUNCTION: bin_zgdbmclear {{{ */

/**/
static int
zgdbmclear_cmd(char *pmname, char *key)
{
    Param pm;

    if (!pmname) {
        zwarn("parameter name (whose read-cache is to be cleared) is required");
        return 1;
    }
    if (!key) {
        zwarn("hash-key (whose read-cache is to be cleared) is required");
        return 1;
    }

    pm = (Param) paramtab->getnode(paramtab, pmname);
    if(!pm) {
        zwarnnam("no such parameter: %s", pmname);
        return 1;
    }

    if (pm->gsu.h != &gdbm_hash_gsu) {
        zwarnnam("not a tied gdbm parameter: %s", pmname);
        return 1;
    }

    HashTable ht = pm->u.hash;
    HashNode hn = gethashnode2( ht, key );
    Param val_pm = (Param) hn;
    if (val_pm) {
        val_pm->node.flags &= ~(PM_UPTODATE);
    }

    return 0;
}
/* }}} */
/* FUNCTION: gdbmgetfn {{{ */

/*
 * The param is actual param in hash – always, because
 * getgdbmnode creates every new key seen. However, it
 * might be not PM_UPTODATE - which means that database
 * wasn't yet queried.
 *
 * It will be left in this state if database doesn't
 * contain such key.
 */

/**/
static char *
gdbmgetfn(Param pm)
{
    datum key, content;
    int ret;
    GDBM_FILE dbf;

    /* Key already retrieved? There is no sense of asking the
     * database again, because:
     * - there can be only multiple readers
     * - so, no writer + reader use is allowed
     *
     * Thus:
     * - if we are writers, we for sure have newest copy of data
     * - if we are readers, we for sure have newest copy of data
     */
    if ((pm->node.flags & PM_UPTODATE) && ((struct gsu_scalar_ext *)pm->gsu.s)->use_cache) {
        return pm->u.str ? pm->u.str : "";
    }

    /* Unmetafy key. GDBM fits nice into this
     * process, as it uses length of data */
    int umlen = 0;
    char *umkey = zsh_db_unmetafy_zalloc(pm->node.nam,&umlen);

    key.dptr = umkey;
    key.dsize = umlen;

    dbf = ((struct gsu_scalar_ext *)pm->gsu.s)->dbf;

    if((ret = gdbm_exists(dbf, key))) {
        /* We have data – store it, return it */
        pm->node.flags |= PM_UPTODATE;

        content = gdbm_fetch(dbf, key);

        /* Ensure there's no leak */
        if (pm->u.str) {
            zsfree(pm->u.str);
        }

        /* Metafy returned data. All fits - metafy
         * can obtain data length to avoid using \0 */
        pm->u.str = metafy(content.dptr, content.dsize, META_DUP);
        /* gdbm allocates with malloc */
        free(content.dptr);

        /* Free key, restoring its original length */
        zsh_db_set_length(umkey, umlen);
        zsfree(umkey);

        /* Can return pointer, correctly saved inside hash */
        return pm->u.str;
    }

    /* Free key, restoring its original length */
    zsh_db_set_length(umkey, umlen);
    zsfree(umkey);

    return "";
}
/* }}} */
/* FUNCTION: gdbmsetfn {{{ */

/**/
static void
gdbmsetfn(Param pm, char *val)
{
    datum key, content;
    GDBM_FILE dbf;

    /* Set is done on parameter and on database.
     * See the allowed workers / readers comment
     * at gdbmgetfn() */

    /* Parameter */
    if (pm->u.str) {
        zsfree(pm->u.str);
        pm->u.str = NULL;
        pm->node.flags &= ~(PM_UPTODATE);
    }

    if (val) {
        pm->u.str = ztrdup(val);
        pm->node.flags |= PM_UPTODATE;
    }

    /* Database */
    dbf = ((struct gsu_scalar_ext *)pm->gsu.s)->dbf;
    if (dbf && no_database_action == 0) {
        int umlen = 0;
        char *umkey = zsh_db_unmetafy_zalloc(pm->node.nam,&umlen);

        key.dptr = umkey;
        key.dsize = umlen;

        if (val) {
            /* Unmetafy with exact zalloc size */
            char *umval = zsh_db_unmetafy_zalloc(val,&umlen);

            /* Store */
            content.dptr = umval;
            content.dsize = umlen;
            (void)gdbm_store(dbf, key, content, GDBM_REPLACE);

            /* Free */
            zsh_db_set_length(umval, umlen);
            zsfree(umval);
        } else {
            (void)gdbm_delete(dbf, key);
        }

        /* Free key */
        zsh_db_set_length(umkey, key.dsize);
        zsfree(umkey);
    }
}
/* }}} */
/* FUNCTION: gdbmunsetfn {{{ */
/**/
static void
gdbmunsetfn(Param pm, UNUSED(int um))
{
    /* Set with NULL */
    gdbmsetfn(pm, NULL);
}
/* }}} */
/* FUNCTION: getgdbmnode {{{ */

/**/
static HashNode
getgdbmnode(HashTable ht, const char *name)
{
    HashNode hn = gethashnode2( ht, name );
    Param val_pm = (Param) hn;

    /* Entry for key doesn't exist? Create it now,
     * it will be interfacing between the database
     * and Zsh - through special gdbm_gsu. So, any
     * seen key results in new interfacing parameter.
     *
     * Previous code was returning heap arena Param
     * that wasn't actually added to the hash. It was
     * plainly name / database-key holder. Here we
     * add the Param to its hash, it is not PM_UPTODATE.
     * It will be loaded from database *and filled*
     * or left in that state if the database doesn't
     * contain it.
     *
     * No heap arena memory is used, memory usage is
     * now limited - by number of distinct keys seen,
     * not by number of key *uses*.
     * */

    if ( ! val_pm ) {
        val_pm = (Param) zshcalloc( sizeof (*val_pm) );
        val_pm->node.flags = PM_SCALAR | PM_HASHELEM; /* no PM_UPTODATE */
        val_pm->gsu.s = (GsuScalar) ht->tmpdata;
        ht->addnode( ht, ztrdup( name ), val_pm ); // sets pm->node.nam
    }

    return (HashNode) val_pm;
}
/* }}} */
/* FUNCTION: scangdbmkeys {{{ */

/**/
static void
scangdbmkeys(HashTable ht, ScanFunc func, int flags)
{
    datum key, prev_key;
    GDBM_FILE dbf = ((struct gsu_scalar_ext *)ht->tmpdata)->dbf;

    /* Iterate keys adding them to hash, so
     * we have Param to use in `func` */
    key = gdbm_firstkey(dbf);

    while(key.dptr) {
        /* This returns database-interfacing Param,
         * it will return u.str or first fetch data
         * if not PM_UPTODATE (newly created) */
        char *zkey = metafy(key.dptr, key.dsize, META_DUP);
        HashNode hn = getgdbmnode(ht, zkey);
        zsfree( zkey );

        func(hn, flags);

        /* Iterate - no problem as interfacing Param
         * will do at most only fetches, not stores */
        prev_key = key;
        key = gdbm_nextkey(dbf, key);
        free(prev_key.dptr);
    }

}
/* }}} */
/* FUNCTION: gdbmhashsetfn {{{ */

/*
 * Replace database with new hash
 */

/**/
static void
gdbmhashsetfn(Param pm, HashTable ht)
{
    int i;
    HashNode hn;
    GDBM_FILE dbf;
    datum key, content;

    if (!pm->u.hash || pm->u.hash == ht)
        return;

    if (!(dbf = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->dbf))
        return;

    key = gdbm_firstkey(dbf);
    while (key.dptr) {
        queue_signals();
        (void)gdbm_delete(dbf, key);
        free(key.dptr);
        unqueue_signals();
        key = gdbm_firstkey(dbf);
    }

    /* just deleted everything, clean up */
    if (GDBM_VERSION_MAJOR > 1 || (GDBM_VERSION_MAJOR == 1 && GDBM_VERSION_MINOR > 11) ) {
        /* dbf gets corrupted on 1.11 */
        (void)gdbm_reorganize(dbf);
    }

    no_database_action = 1;
    emptyhashtable(pm->u.hash);
    no_database_action = 0;

    if (!ht)
        return;

    /* Put new strings into database, waiting
     * for their interfacing-Params to be created */

    for (i = 0; i < ht->hsize; i++) {
        for (hn = ht->nodes[i]; hn; hn = hn->next) {
            struct value v;

            v.isarr = v.flags = v.start = 0;
            v.end = -1;
            v.arr = NULL;
            v.pm = (Param) hn;

            /* Unmetafy key */
            int umlen = 0;
            char *umkey = zsh_db_unmetafy_zalloc(v.pm->node.nam,&umlen);

            key.dptr = umkey;
            key.dsize = umlen;

            queue_signals();

            /* Unmetafy */
            char *umval = zsh_db_unmetafy_zalloc(getstrvalue(&v),&umlen);

            /* Store */
            content.dptr = umval;
            content.dsize = umlen;
            (void)gdbm_store(dbf, key, content, GDBM_REPLACE);

            /* Free - zsh_db_unmetafy_zalloc allocates exact required
             * space, however unmetafied string can have zeros
             * in content, so we must first fill with non-0 bytes */
            zsh_db_set_length(umval, content.dsize);
            zsfree(umval);
            zsh_db_set_length(umkey, key.dsize);
            zsfree(umkey);

            unqueue_signals();
        }
    }
    /* We reuse our hash, the input is to be deleted */
    deleteparamtable(ht);
}
/* }}} */
/* FUNCTION: gdbmuntie {{{*/

/**/
static void
gdbmuntie(Param pm)
{
    struct gsu_scalar_ext *gsu_ext = (struct gsu_scalar_ext *)pm->u.hash->tmpdata;
    GDBM_FILE dbf = gsu_ext->dbf;
    HashTable ht = pm->u.hash;

    if (dbf) { /* paranoia */
        fdtable[gsu_ext->fdesc] = FDT_UNUSED;
        gdbm_close(dbf);

        /* Let hash fields know there's no backend */
        ((struct gsu_scalar_ext *)ht->tmpdata)->dbf = NULL;

        /* Remove from list of tied parameters */
        zsh_db_filter_arr(&zgdbm_tied, pm->node.nam);
    }

    /* for completeness ... createspecialhash() should have an inverse */
    ht->getnode = ht->getnode2 = gethashnode2;
    ht->scantab = NULL;

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.h = &stdhash_gsu;
}
/* }}} */
/* FUNCTION: gdbmhashunsetfn {{{ */
/**/
static void
gdbmhashunsetfn(Param pm, UNUSED(int exp))
{
    gdbmuntie(pm);

    /* Remember custom GSU structure assigned to
     * u.hash->tmpdata before hash gets deleted */
    struct gsu_scalar_ext * gsu_ext = pm->u.hash->tmpdata;

    /* Uses normal unsetter (because gdbmuntie is called above).
     * Will delete all owned field-parameters and also hashtable. */
    pm->gsu.h->setfn(pm, NULL);

    /* Don't need custom GSU structure with its
     * GDBM_FILE pointer anymore */
    zsfree( gsu_ext->dbfile_path );
    zfree( gsu_ext, sizeof(struct gsu_scalar_ext));

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* ARRAY: module_features {{{ */
static struct features module_features =
    {
     NULL, 0,
     NULL, 0,
     NULL, 0,
     patab, sizeof(patab)/sizeof(*patab),
     0
    };
/* }}} */

/* FUNCTION: setup_ {{{ */

/**/
int
setup_(UNUSED(Module m))
{
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
    zgdbm_tied = zshcalloc((1) * sizeof(char *));
    zsh_db_register_backend("db/gdbm", gdbm_main_entry);
    return 0;
}
/* }}} */
/* FUNCTION: cleanup_ {{{ */

/**/
int
cleanup_(Module m)
{
    zsh_db_unregister_backend("db/gdbm");

    /* This frees `zgdbm_tied` */
    return setfeatureenables(m, &module_features, NULL);
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

/*********************
 * Utility functions *
 *********************/

/* FUNCTION: createhash {{{ */

static Param createhash( char *name, int flags ) {
    Param pm;
    HashTable ht;

    pm = createparam(name, flags | PM_SPECIAL | PM_HASHED);
    if (!pm) {
        return NULL;
    }

    if (pm->old)
        pm->level = locallevel;

    /* This creates standard hash. */
    ht = pm->u.hash = newparamtable(17, name);
    if (!pm->u.hash) {
        paramtab->removenode(paramtab, name);
        paramtab->freenode(&pm->node);
        zwarnnam(name, "Out of memory when allocating hash");
        return NULL;
    }

    /* Does free Param (unsetfn is called) */
    ht->freenode = zsh_db_freeparamnode;

    /* These provide special features */
    ht->getnode = ht->getnode2 = getgdbmnode;
    ht->scantab = scangdbmkeys;

    return pm;
}
/* }}} */
/* FUNCTION: is_tied_cmd {{{ */
static int
is_tied_cmd(char *pmname)
{
    Param pm = (Param) paramtab->getnode(paramtab, pmname);
    if(!pm) {
        return 1; /* false */
    }

    return 1 - is_tied(pm); /* negation for shell-code */
}
/* }}} */
/* FUNCTION: is_tied {{{ */

static int
is_tied(Param pm)
{
    if (pm->gsu.h == &gdbm_hash_gsu ) {
        return 1;
    }

    return 0;
}
/* }}} */

#else
# error no gdbm
#endif /* have gdbm */
