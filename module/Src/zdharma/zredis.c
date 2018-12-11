/* -*- Mode: C; c-default-style: "linux"; c-basic-offset: 4; indent-tabs-mode: nil -*-
 * vim:sw=4:sts=4:et
 */

/*
 * zredis.c - bindings for (hi)redis
 *
 * Based on db_gdbm.c from Zsh distribution, Copyright (c) 2008 Clint Adams
 * All rights reserved.
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

#include "zredis.mdh"
#include "zredis.pro"
#include "db.epro"
#include "db.h"

/* MACROS {{{ */
#ifndef PM_UPTODATE
#define PM_UPTODATE     (PM_LOADDIR) /* Parameter has up-to-date data (e.g. loaded from DB) */
#endif
/* }}} */

#if defined(HAVE_HIREDIS_HIREDIS_H) && defined(HAVE_REDISCONNECT)

/* DECLARATIONS {{{ */
static char *type_names[10] = { "none", "invalid", "no-key (main hash)", "string", "list", "set", "sorted-set", "hash", "error", NULL };

#include <hiredis/hiredis.h>

static Param createhash(char *name, int flags, int which);
static void parse_host_string(const char *input, char *buffer, int size,
                                char **host, int *port, int *db_index, char **key);
static int connect(redisContext **rc, const char* password, const char *host, int port, int db_index, const char *address);
static int type(redisContext **rc, int *fdesc, const char *redis_host_port, const char *password, char *key, size_t key_len);
static int type_from_string(const char *string, int len);
static int is_tied(Param pm);
static void zrzset_usage();
static int reconnect(redisContext **rc, int *fdesc, const char *hostspec, const char *password);
static int auth(redisContext **rc, const char *password);
static int is_tied_cmd(char *pmname);
static void deletehashparam(Param tied_param, const char *pmname);


static char *my_nullarray = NULL;
static int no_database_action = 0;
static int yes_unsetting = 0;
static int doing_untie = 0;
/* }}} */
/* ARRAY: GSU {{{ */

/*
 * Longer GSU structure, to carry redisContext of owning
 * database. Every parameter (hash value) receives GSU
 * pointer and thus also receives redisContext - this way
 * parameters can access proper database.
 *
 * Main HashTable parameter has the same instance of
 * the custom GSU struct in u.hash->tmpdata field.
 * When database is closed, `rc` field is set to NULL
 * and hash values know to not access database when
 * being unset (total purge at zuntie).
 *
 * When database closing is ended, the custom GSU struct
 * is freed. Only new ztie creates new custom GSU struct
 * instance.
 *
 * This is also for hashes, and is called *scalar*, because
 * it's for hash *elements*, PM_SCALAR | PM_HASHELEM. Also
 * for strings.
 */
struct gsu_scalar_ext {
    struct gsu_scalar std; /* Size of three pointers */
    int type;
    int use_cache;
    int is_lazy;
    char *redis_host_port;
    char *key;
    size_t key_len;
    char *password;
    int fdesc;
    redisContext *rc;
    int unset_deletes;
};

/* Used by sets */
struct gsu_array_ext {
    struct gsu_array std; /* Size of three pointers */
    int type;
    int use_cache;
    int is_lazy;
    char *redis_host_port;
    char *key;
    size_t key_len;
    char *password;
    int fdesc;
    redisContext *rc;
    int unset_deletes;
};

/* Source structure - will be copied to allocated one,
 * with `rc` filled. `rc` allocation <-> gsu allocation. */
static const struct gsu_scalar_ext hashel_gsu_ext =
    { { redis_getfn, redis_setfn, redis_unsetfn }, 0, 0, 0, 0, 0 };

/* Hash GSU, normal one */
static const struct gsu_hash redis_hash_gsu =
    { hashgetfn, redis_hash_setfn, redis_hash_unsetfn };

/* String (scalar to key) mapping */
static const struct gsu_scalar_ext string_gsu_ext =
    { { redis_str_getfn, redis_str_setfn, redis_str_unsetfn }, 0, 0, 0, 0, 0 };

/* Array to set mapping */
static const struct gsu_array_ext arrset_gsu_ext =
    { { redis_arrset_getfn, redis_arrset_setfn, redis_arrset_unsetfn }, 0, 0, 0, 0, 0 };

/* PM_HASHELEM GSU for zset */
static const struct gsu_scalar_ext hashel_zset_gsu_ext =
    { { redis_zset_getfn, redis_zset_setfn, redis_zset_unsetfn }, 0, 0, 0, 0, 0 };

/* Hash GSU for zset, normal one */
static const struct gsu_hash hash_zset_gsu =
    { hashgetfn, redis_hash_zset_setfn, redis_hash_zset_unsetfn };

/* PM_HASHELEM GSU for hset */
static const struct gsu_scalar_ext hashel_hset_gsu_ext =
    { { redis_hset_getfn, redis_hset_setfn, redis_hset_unsetfn }, 0, 0, 0, 0, 0 };

/* Hash GSU for hset, normal one */
static const struct gsu_hash hash_hset_gsu =
    { hashgetfn, redis_hash_hset_setfn, redis_hash_hset_unsetfn };

/* Array to list mapping */
static const struct gsu_array_ext arrlist_gsu_ext =
    { { redis_arrlist_getfn, redis_arrlist_setfn, redis_arrlist_unsetfn }, 0, 0, 0, 0, 0 };

/* }}} */
/* ARRAY: builtin {{{ */
static struct builtin bintab[] = {
    BUILTIN("zrzset", 0, bin_zrzset, 0, 1, 0, "h", NULL),
};
/* }}} */
/* ARRAY: other {{{ */
#define ROARRPARAMDEF(name, var) \
    { name, PM_ARRAY | PM_READONLY, (void *) var, NULL,  NULL, NULL, NULL }

/* Holds names of all tied parameters */
char **zredis_tied;

static struct paramdef patab[] = {
    ROARRPARAMDEF("zredis_tied", &zredis_tied),
};
/* }}} */

/* FUNCTION: redis_main_entry {{{ */
static int
redis_main_entry(VA_ALIST1(int cmd))
VA_DCL
{
    char *address = NULL, *pass = NULL, *pfile = NULL, *pmname = NULL, *key = NULL, *lazy = NULL;
    int flags = 0, rountie = 0;

    va_list ap;
    VA_DEF_ARG(int cmd);

    VA_START(ap, cmd);
    VA_GET_ARG(ap, cmd, int);

    switch (cmd) {
    case DB_TIE:
        /* Order is:
         * flags -r, -z, -l, -D, -S
         * -a/f address, char *
         * -p password, char *
         * -P file with password, char *
         * parameter name, char *
         * -L lazy type, char *
         */
        flags = va_arg(ap, int);
        address = va_arg(ap, char *);
        pass = va_arg(ap, char *);
        pfile = va_arg(ap, char *);
        pmname = va_arg(ap, char *);
        lazy = va_arg(ap, char *);
        return zrtie_cmd(flags, address, pass, pfile, pmname, lazy);

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
        return zruntie_cmd(rountie, argv);

    case DB_IS_TIED:
        /* Order is:
         * parameter name, char *
         */
        pmname = va_arg(ap, char *);
        return is_tied_cmd(pmname);

    case DB_GET_ADDRESS:
        /* Order is:
         * parameter name, char *
         */
        pmname = va_arg(ap, char *);
        return zredishost_cmd(pmname);

    case DB_CLEAR_CACHE:
        /* Order is:
         * parameter name, char *
         * key name, char *
         */
        pmname = va_arg(ap, char *);
        key = va_arg(ap, char *);
        return zredisclear_cmd(pmname, key);

    default:
#ifdef DEBUG
        dputs("Bad command %d in redis_main_entry", cmd);
#endif
        break;
    }
    return 1;
}
/* }}} */
/* FUNCTION: zrtie_cmd {{{ */

/**/
static int
zrtie_cmd(int flags, char *address, char *pass, char *pfile, char *pmname, char *lazy)
{
    redisContext *rc = NULL;
    int pmflags = PM_REMOVABLE;
    Param tied_param;

    if (!address) {
        zwarn("you must pass `-f' or '-a' with {host}[:port][/[db_idx][/key]], see `-h'", NULL);
        return 1;
    }

    if (!pmname) {
        zwarn("you must pass non-option argument - the target parameter to create, see -h");
        return 1;
    }

    if (flags & DB_FLAG_RONLY) {
        pmflags |= PM_READONLY;
    }

    /* Parse host data */

    char resource_name[192];
    char *host="127.0.0.1", *key="";
    int port = 6379, db_index = 0;

    parse_host_string(address, resource_name, 192, &host, &port, &db_index, &key);

    /* Unset existing parameter */

    if ((tied_param = (Param)paramtab->getnode(paramtab, pmname)) && !(tied_param->node.flags & PM_UNSET)) {
        if (is_tied(tied_param)) {
            zwarn("Refusing to re-tie already tied parameter `%s'", pmname);
            zwarn("The involved `unset' could clear the database-part handled by `%s'", pmname);
            return 1;
        }

        /*
         * Unset any existing parameter. Note there's no implicit
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

    /* Establish password */

    char buf[1025];
    if (!pass && pfile) {
        FILE *txtFILE = fopen(pfile, "r");
        if (txtFILE) {
            int size = fread(buf, 1, 1024, txtFILE);
            fclose(txtFILE);
            if (size > 0) {
                if (size > 1024)
                    size = 1024;
                while (buf[size-1] == '\r' || buf[size-1] == '\n') {
                    --size;
                    if (size <= 0) {
                        zwarn("Couldn't read password file: `%s', it contains only newlines, aborting", pfile);
                        return 1;
                    }
                }
                buf[size] = '\0';
                pass = buf;
            } else {
                zwarn("Couldn't read password file: `%s', aborting", pfile);
                return 1;
            }
        } else {
            zwarn("Couldn't open password file: `%s', aborting", pfile);
            return 1;
        }
    }

    /* Connect */

    if (!lazy || (flags & DB_FLAG_NOCONNECT) == 0) {
        if (!connect(&rc, pass, host, port, db_index, address)) {
            return 1;
        } else {
            addmodulefd(rc->fd, FDT_INTERNAL);
        }
    }

    /* Main string storage? */
    if (0 == strcmp(key,"")) {
        /* Create hash */
        if (!(tied_param = createhash(pmname, pmflags, 0))) {
            zwarn("cannot create the requested hash parameter: %s", pmname);
            if (rc)
                redisFree(rc);
            return 1;
        }

        /* Allocate parameter sub-gsu, fill rc field.
         * rc allocation is 1 to 1 accompanied by
         * gsu_scalar_ext allocation. */

        struct gsu_scalar_ext *rc_carrier = NULL;
        rc_carrier = (struct gsu_scalar_ext *) zshcalloc(sizeof(struct gsu_scalar_ext));
        if (!rc_carrier) {
            if (rc)
                redisFree(rc);
            deletehashparam(tied_param, pmname);
            zwarn("Out of memory when allocating common data structure (1)");
            return 1;
        }

        rc_carrier->std = hashel_gsu_ext.std;
        rc_carrier->type = DB_KEY_TYPE_NO_KEY;

        rc_carrier->use_cache = 1;
        if (flags & DB_FLAG_ZERO)
            rc_carrier->use_cache = 0;
        if (lazy)
            rc_carrier->is_lazy = 1;
        if (flags & DB_FLAG_DELETE)
            rc_carrier->unset_deletes = 1;

        if (rc) {
            rc_carrier->rc = rc;
            rc_carrier->fdesc = rc->fd;
        }

        /* Fill also host:port// and password fields */
        rc_carrier->redis_host_port = ztrdup(address);
        if (pass)
            rc_carrier->password = ztrdup(pass);
        else
            rc_carrier->password = NULL;

        tied_param->u.hash->tmpdata = (void *)rc_carrier;
        tied_param->gsu.h = &redis_hash_gsu;
    } else {
        int tpe, tpe2, dummy_fd = 0;
        if (lazy) {
            tpe = type_from_string(lazy, strlen(lazy));
            if (rc) {
                tpe2 = type(&rc, &dummy_fd, address, pass, key, (size_t) strlen(key));
                if (tpe != tpe2 && tpe2 != DB_KEY_TYPE_NONE) {
                    zwarn("Key `%s' already exists and is of type: `%s', aborting",
                        key, (tpe2 >= 0 && tpe2 <= 8) ? type_names[tpe2] : "error");
                    if (rc) {
                        redisFree(rc);
                    }
                    return 1;
                }
            }
        } else {
            tpe = type(&rc, &dummy_fd, address, pass, key, (size_t) strlen(key));
        }
        if (tpe == DB_KEY_TYPE_STRING) {
            if (!(tied_param = createparam(pmname, pmflags | PM_SPECIAL))) {
                zwarn("cannot create the requested scalar parameter: %s", pmname);
                if (rc)
                    redisFree(rc);
                return 1;
            }
            struct gsu_scalar_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_scalar_ext *) zshcalloc(sizeof(struct gsu_scalar_ext));
            rc_carrier->std = string_gsu_ext.std;
            rc_carrier->type = DB_KEY_TYPE_STRING;
            rc_carrier->use_cache = 1;

            if (flags & DB_FLAG_ZERO)
                rc_carrier->use_cache = 0;
            if (lazy)
                rc_carrier->is_lazy = 1;
            if (flags & DB_FLAG_DELETE)
                rc_carrier->unset_deletes = 1;

            if (rc) {
                rc_carrier->rc = rc;
                rc_carrier->fdesc = rc->fd;
            }

            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// and password fields */
            rc_carrier->redis_host_port = ztrdup(address);
            if (pass)
                rc_carrier->password = ztrdup(pass);
            else
                rc_carrier->password = NULL;

            tied_param->gsu.s = (GsuScalar) rc_carrier;
        } else if (tpe == DB_KEY_TYPE_SET) {
            if (!(tied_param = createparam(pmname, pmflags | PM_ARRAY | PM_SPECIAL))) {
                zwarn("cannot create the requested array (for set) parameter: %s", pmname);
                if (rc)
                    redisFree(rc);
                return 1;
            }
            struct gsu_array_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_array_ext *) zshcalloc(sizeof(struct gsu_array_ext));
            rc_carrier->std = arrset_gsu_ext.std;
            rc_carrier->type = DB_KEY_TYPE_SET;
            rc_carrier->use_cache = 1;

            if (flags & DB_FLAG_ZERO)
                rc_carrier->use_cache = 0;
            if (lazy)
                rc_carrier->is_lazy = 1;
            if (flags & DB_FLAG_DELETE)
                rc_carrier->unset_deletes = 1;

            if (rc) {
                rc_carrier->rc = rc;
                rc_carrier->fdesc = rc->fd;
            }

            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// and password fields */
            rc_carrier->redis_host_port = ztrdup(address);
            if (pass)
                rc_carrier->password = ztrdup(pass);
            else
                rc_carrier->password = NULL;

            tied_param->gsu.s = (GsuScalar) rc_carrier;
        } else if (tpe == DB_KEY_TYPE_ZSET) {
            /* Create hash */
            if (!(tied_param = createhash(pmname, pmflags, 1))) {
                zwarn("cannot create the requested hash (for zset) parameter: %s", pmname);
                if (rc)
                    redisFree(rc);
                return 1;
            }

            struct gsu_scalar_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_scalar_ext *) zshcalloc(sizeof(struct gsu_scalar_ext));
            if (!rc_carrier) {
                if (rc)
                    redisFree(rc);
                deletehashparam(tied_param, pmname);
                zwarn("Out of memory when allocating common data structure (2)");
                return 1;
            }

            rc_carrier->std = hashel_zset_gsu_ext.std;
            rc_carrier->type = DB_KEY_TYPE_ZSET;

            rc_carrier->use_cache = 1;
            if (flags & DB_FLAG_ZERO)
                rc_carrier->use_cache = 0;
            if (lazy)
                rc_carrier->is_lazy = 1;
            if (flags & DB_FLAG_DELETE)
                rc_carrier->unset_deletes = 1;

            if (rc) {
                rc_carrier->rc = rc;
                rc_carrier->fdesc = rc->fd;
            }

            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// and password fields */
            rc_carrier->redis_host_port = ztrdup(address);
            if (pass)
                rc_carrier->password = ztrdup(pass);
            else
                rc_carrier->password = NULL;

            tied_param->u.hash->tmpdata = (void *)rc_carrier;
            tied_param->gsu.h = &hash_zset_gsu;
        } else if (tpe == DB_KEY_TYPE_HASH) {
            /* Create hash */
            if (!(tied_param = createhash(pmname, pmflags, 2))) {
                zwarn("cannot create the requested hash (for hset) parameter: %s", pmname);
                if (rc)
                    redisFree(rc);
                return 1;
            }

            struct gsu_scalar_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_scalar_ext *) zshcalloc(sizeof(struct gsu_scalar_ext));
            if (!rc_carrier) {
                if (rc)
                    redisFree(rc);
                deletehashparam(tied_param, pmname);
                zwarn("Out of memory when allocating common data structure (3)");
                return 1;
            }

            rc_carrier->std = hashel_hset_gsu_ext.std;
            rc_carrier->type = DB_KEY_TYPE_HASH;

            rc_carrier->use_cache = 1;
            if (flags & DB_FLAG_ZERO)
                rc_carrier->use_cache = 0;
            if (lazy)
                rc_carrier->is_lazy = 1;
            if (flags & DB_FLAG_DELETE)
                rc_carrier->unset_deletes = 1;

            if (rc) {
                rc_carrier->rc = rc;
                rc_carrier->fdesc = rc->fd;
            }

            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// and password fields */
            rc_carrier->redis_host_port = ztrdup(address);
            if (pass)
                rc_carrier->password = ztrdup(pass);
            else
                rc_carrier->password = NULL;

            tied_param->u.hash->tmpdata = (void *)rc_carrier;
            tied_param->gsu.h = &hash_hset_gsu;
        } else if (tpe == DB_KEY_TYPE_LIST) {
            if (!(tied_param = createparam(pmname, pmflags | PM_ARRAY | PM_SPECIAL))) {
                zwarn("cannot create the requested array (for list) parameter: %s", pmname);
                if (rc)
                    redisFree(rc);
                return 1;
            }
            struct gsu_array_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_array_ext *) zshcalloc(sizeof(struct gsu_array_ext));
            rc_carrier->std = arrlist_gsu_ext.std;
            rc_carrier->type = DB_KEY_TYPE_LIST;
            rc_carrier->use_cache = 1;

            if (flags & DB_FLAG_ZERO)
                rc_carrier->use_cache = 0;
            if (lazy)
                rc_carrier->is_lazy = 1;
            if (flags & DB_FLAG_DELETE)
                rc_carrier->unset_deletes = 1;

            if (rc) {
                rc_carrier->rc = rc;
                rc_carrier->fdesc = rc->fd;
            }

            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// and password fields */
            rc_carrier->redis_host_port = ztrdup(address);
            if (pass)
                rc_carrier->password = ztrdup(pass);
            else
                rc_carrier->password = NULL;

            tied_param->gsu.s = (GsuScalar) rc_carrier;
        } else if (tpe == DB_KEY_TYPE_NONE) {
            if (rc)
                redisFree(rc);
            if (lazy) {
                zwarn("`none' disallowed as key-type, aborting");
            } else {
                zwarn("Key doesn't exist: `%s', use -L {type} for lazy binding", key);
            }
            return 1;
        } else {
            if (rc)
                redisFree(rc);
            zwarn("Unknown key type: %s", (tpe >= 0 && tpe <= 8) ? type_names[tpe] : "error");
            return 1;
        }
    }

    /* Save in tied-enumeration array */
    zsh_db_arr_append(&zredis_tied, pmname);

    return 0;
}
/* }}} */
/* FUNCTION: zruntie_cmd {{{ */

/**/
static int
zruntie_cmd(int rountie, char **pmnames)
{
    Param pm;
    char *pmname;
    int ret = 0;

    if (!*pmnames) {
        zwarn("At least one variable name is needed, see -h");
        return 1;
    }

    for (pmname = *pmnames; *pmnames++; pmname = *pmnames) {
        /* Get param */
        pm = (Param) paramtab->getnode(paramtab, pmname);
        if(!pm) {
            zwarn("cannot untie `%s', parameter not found", pmname);
            ret = 1;
            continue;
        }

        if (pm->gsu.h == &redis_hash_gsu) {
            queue_signals();
            if (rountie) {
                pm->node.flags &= ~PM_READONLY;
            }
            if (unsetparam_pm(pm, 0, 1)) {
                /* assume already reported */
                ret = 1;
            }
            unqueue_signals();
        } else if (pm->gsu.s->getfn == &redis_str_getfn) {
            if (pm->node.flags & PM_READONLY && !rountie) {
                zwarn("cannot untie `%s', parameter is read only, use -u option", pmname);
                continue;
            }
            pm->node.flags &= ~PM_READONLY;

            queue_signals();
            /* Detach from database, untie doesn't clear the database */
            redis_str_untie(pm);

            if (unsetparam_pm(pm, 0, 1)) {
                /* assume already reported */
                ret = 1;
            }
            unqueue_signals();
        } else if (pm->gsu.a->getfn == &redis_arrset_getfn) {
            if (pm->node.flags & PM_READONLY && !rountie) {
                zwarn("cannot untie array `%s', the set-bound parameter is read only, use -u option", pmname);
                continue;
            }
            pm->node.flags &= ~PM_READONLY;
            queue_signals();
            /* Detach from database, untie doesn't clear the database */
            redis_arrset_untie(pm);

            if (unsetparam_pm(pm, 0, 1)) {
                ret = 1;
            }
            unqueue_signals();
        } else if (pm->gsu.h == &hash_zset_gsu) {
            queue_signals();
            if (rountie) {
                pm->node.flags &= ~PM_READONLY;
            }
            doing_untie = 1;
            if (unsetparam_pm(pm, 0, 1)) {
                ret = 1;
            }
            doing_untie = 0;
            unqueue_signals();
        } else if (pm->gsu.h == &hash_hset_gsu) {
            queue_signals();
            if (rountie) {
                pm->node.flags &= ~PM_READONLY;
            }
            doing_untie = 1;
            if (unsetparam_pm(pm, 0, 1)) {
                /* assume already reported */
                ret = 1;
            }
            doing_untie = 0;
            unqueue_signals();
        } else if (pm->gsu.a->getfn == &redis_arrlist_getfn) {
            if (pm->node.flags & PM_READONLY && !rountie) {
                zwarn("cannot untie array `%s', the list-bound parameter is read only, use -u option", pmname);
                continue;
            }
            pm->node.flags &= ~PM_READONLY;
            queue_signals();
            /* Detach from database, untie doesn't clear the database */
            redis_arrset_untie(pm);

            if (unsetparam_pm(pm, 0, 1)) {
                ret = 1;
            }
            unqueue_signals();
        } else {
            zwarn("not a tied redis parameter: `%s'", pmname);
            ret = 1;
            continue;
        }
    }

    return ret;
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
/* FUNCTION: zredishost_cmd {{{ */

/**/
static int
zredishost_cmd(char *pmname)
{
    Param pm;

    if (!pmname) {
        zwarn("parameter name (whose host-spec is to be written to $REPLY) is not given, see -h");
        return 1;
    }

    pm = (Param) paramtab->getnode(paramtab, pmname);
    if(!pm) {
        zwarn("no such parameter: %s", pmname);
        return 1;
    }

    const char *hostspec = NULL;

    if (pm->gsu.h == &redis_hash_gsu) {
        /* Paranoia, it *will* be always set */
        hostspec = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->redis_host_port;
    } else if(pm->gsu.s->getfn == &redis_str_getfn) {
        hostspec = ((struct gsu_scalar_ext *)pm->gsu.s)->redis_host_port;
    } else if(pm->gsu.a->getfn == &redis_arrset_getfn) {
        hostspec = ((struct gsu_array_ext *)pm->gsu.a)->redis_host_port;
    } else if(pm->gsu.h == &hash_zset_gsu) {
        hostspec = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->redis_host_port;
    } else if(pm->gsu.h == &hash_hset_gsu) {
        hostspec = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->redis_host_port;
    } else if(pm->gsu.a->getfn == &redis_arrlist_getfn) {
        hostspec = ((struct gsu_array_ext *)pm->gsu.a)->redis_host_port;
    } else {
        zwarn("not a tied zredis parameter: `%s', REPLY unchanged", pmname);
        return 1;
    }

    if (hostspec) {
        setsparam("REPLY", ztrdup(hostspec));
    } else {
        setsparam("REPLY", ztrdup(""));
    }
    return 0;
}
/* }}} */
/* FUNCTION: zredisclear_cmd {{{ */

/**/
static int
zredisclear_cmd(char *pmname, char *key)
{
    Param pm;

    if (!pmname) {
        zwarn("parameter name (whose cache is to be cleared) is required, see -h");
        return 1;
    }

    pm = (Param) paramtab->getnode(paramtab, pmname);
    if (!pm) {
        zwarn("no such parameter: %s", pmname);
        return 1;
    }

    if (pm->gsu.h == &redis_hash_gsu) {
        if (!key) {
            zwarn("Key name, which is to be cache-cleared in hash `%s', is required", pmname);
            return 1;
        }
        HashTable ht = pm->u.hash;
        HashNode hn = gethashnode2(ht, key);
        Param val_pm = (Param) hn;
        if (val_pm) {
            val_pm->node.flags &= ~(PM_UPTODATE);
        }
    } else if (pm->gsu.s->getfn == &redis_str_getfn) {
        pm->node.flags &= ~(PM_UPTODATE);
        if (key)
            zwarn("Ignored argument `%s'", key);
    } else if (pm->gsu.a->getfn == &redis_arrset_getfn) {
        pm->node.flags &= ~(PM_UPTODATE);
        if (key)
            zwarn("Ignored argument `%s'", key);
    } else if(pm->gsu.h == &hash_zset_gsu) {
        if (!key) {
            zwarn("Key name, which is to be cache-cleared in hash/zset `%s', is required", pmname);
            return 1;
        }
        HashTable ht = pm->u.hash;
        HashNode hn = gethashnode2(ht, key);
        Param val_pm = (Param) hn;
        if (val_pm) {
            val_pm->node.flags &= ~(PM_UPTODATE);
        }
    } else if(pm->gsu.h == &hash_hset_gsu) {
        if (!key) {
            zwarn("Key name, which is to be cache-cleared in hash/hset `%s', is required", pmname);
            return 1;
        }
        HashTable ht = pm->u.hash;
        HashNode hn = gethashnode2(ht, key);
        Param val_pm = (Param) hn;
        if (val_pm) {
            val_pm->node.flags &= ~(PM_UPTODATE);
        }
    } else if (pm->gsu.a->getfn == &redis_arrlist_getfn) {
        pm->node.flags &= ~(PM_UPTODATE);
        if (key)
            zwarn("Ignored argument `%s'", key);
    } else {
        zwarn("not a tied zredis parameter: %s", pmname);
        return 1;
    }

    return 0;
}
/* }}} */

/*************** HASH ELEM ***************/

/* FUNCTION: redis_getfn {{{ */

/*
 * The param is actual param in hash – always, because
 * redis_get_node creates every new key seen. However, it
 * might be not PM_UPTODATE - which means that database
 * wasn't yet queried.
 *
 * It will be left in this state if database doesn't
 * contain such key.
 */

/**/
static char *
redis_getfn(Param pm)
{
    struct gsu_scalar_ext *gsu_ext;
    char *key, *umkey;
    size_t key_len;
    redisContext *rc;
    redisReply *reply = NULL;
    int retry, umlen;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : "";
    }

    /* Unmetafy key. Redis fits nice into this
     * process, as it can use length of data */
    umlen = 0;
    umkey = zsh_db_unmetafy_zalloc(pm->node.nam, &umlen);

    key = umkey;
    key_len = umlen;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if (rc) {
        reply = redisCommand(rc, "EXISTS %b", key, (size_t) key_len);
        if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
            freeReplyObject(reply);

            reply = redisCommand(rc, "GET %b", key, (size_t) key_len);
            if (reply && reply->type == REDIS_REPLY_STRING) {
                /* We have data – store it, return it */
                pm->node.flags |= PM_UPTODATE;

                /* Ensure there's no leak */
                if (pm->u.str) {
                    zsfree(pm->u.str);
                    pm->u.str = NULL;
                }

                /* Metafy returned data. All fits - metafy
                 * can obtain data length to avoid using \0 */
                pm->u.str = metafy(reply->str, reply->len, META_DUP);
                freeReplyObject(reply);

                /* Free key, restoring its original length */
                zsh_db_set_length(umkey, key_len);
                zsfree(umkey);

                /* Can return pointer, correctly saved inside hash */
                return pm->u.str;
            } else if (reply) {
                freeReplyObject(reply);
            }
        } else if (reply) {
            freeReplyObject(reply);
        }
    }

    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
    }

    /* Free key, restoring its original length */
    zsh_db_set_length(umkey, key_len);
    zsfree(umkey);

    return "";
}
/* }}} */
/* FUNCTION: redis_setfn {{{ */

/**/
static void
redis_setfn(Param pm, char *val)
{
    char *key, *content;
    size_t key_len, content_len;
    redisContext *rc;
    redisReply *reply = NULL;
    struct gsu_scalar_ext *gsu_ext;
    int retry;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    /* Set is done on parameter and on database. */

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

    retry = 0;
 retry:

    /* Database */
    rc = gsu_ext->rc;

    /* Can be NULL, when calling unset after untie */
    if (no_database_action == 0) {
        if (rc) {
            int umlen = 0;
            char *umkey = zsh_db_unmetafy_zalloc(pm->node.nam, &umlen);

            key = umkey;
            key_len = umlen;

            if (val) {
                /* Unmetafy with exact zalloc size */
                char *umval = zsh_db_unmetafy_zalloc(val, &umlen);

                /* Store */
                content = umval;
                content_len = umlen;
                reply = redisCommand(rc, "SET %b %b", key, (size_t) key_len, content, (size_t) content_len);
                if (reply)
                    freeReplyObject(reply);

                /* Free */
                zsh_db_set_length(umval, content_len);
                zsfree(umval);
            } else {
                reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
                if (reply)
                    freeReplyObject(reply);
            }

            /* Free key */
            zsh_db_set_length(umkey, key_len);
            zsfree(umkey);
        }

        if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
        } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            zwarn("Aborting (no connection)");
        }
    }
}
/* }}} */
/* FUNCTION: redis_unsetfn {{{ */

/**/
static void
redis_unsetfn(Param pm, UNUSED(int um))
{
    /* Set with NULL */
    redis_setfn(pm, NULL);
}
/* }}} */

/***************** HASH ******************/

/* FUNCTION: redis_get_node {{{ */

/**/
static HashNode
redis_get_node(HashTable ht, const char *name)
{
    HashNode hn = gethashnode2(ht, name);
    Param val_pm = (Param) hn;

    /* Entry for key doesn't exist? Create it now,
     * it will be interfacing between the database
     * and Zsh - through special gsu. So, any seen
     * key results in new interfacing parameter.
     *
     * Add the Param to its hash, it is not PM_UPTODATE.
     * It will be loaded from database *and filled*
     * or left in that state if the database doesn't
     * contain it.
     */

    if (!val_pm) {
        val_pm = (Param) zshcalloc(sizeof (*val_pm));
        val_pm->node.flags = PM_SCALAR | PM_HASHELEM; /* no PM_UPTODATE */
        val_pm->gsu.s = (GsuScalar) ht->tmpdata;
        ht->addnode(ht, ztrdup(name), val_pm); // sets pm->node.nam
    }

    return (HashNode) val_pm;
}
/* }}} */
/* FUNCTION: scan_keys {{{ */

/**/
static void
scan_keys(HashTable ht, ScanFunc func, int flags)
{
    char *key;
    size_t key_len, j;
    unsigned long long cursor = 0;
    redisContext *rc;
    redisReply *reply = NULL, *reply2 = NULL;
    struct gsu_scalar_ext *gsu_ext;

    gsu_ext = (struct gsu_scalar_ext *)ht->tmpdata;

    do {
        int retry = 0;
    retry:
        rc = gsu_ext->rc;

        if (rc) {
            /* Iterate keys adding them to hash, so
            * we have Param to use in `func` */
            reply = redisCommand(rc, "SCAN %llu", cursor);
        }

        /* Disconnect detection */
        if (!rc || rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            if (reply) {
                freeReplyObject(reply);
            }
            if (retry) {
                zwarn("Aborting (no connection)");
                return;
            }
            retry = 1;
            if (reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
            else
                return;
        }

        if (!reply || reply->type != REDIS_REPLY_ARRAY) {
            zwarn("Incorrect reply from redis command SCAN, aborting");
            if (reply)
                freeReplyObject(reply);
            return;
        }

        /* Get new cursor */
        if (reply->element[0]->type == REDIS_REPLY_STRING) {
            cursor = strtoull(reply->element[0]->str, NULL, 10);
        } else {
            zwarn("Error 14 occured during SCAN");
            break;
        }

        reply2 = reply->element[1];
        for (j = 0; j < reply2->elements; j++) {
            redisReply *entry = reply2->element[j];
            if (entry == NULL || entry->type != REDIS_REPLY_STRING) {
                continue;
            }

            key = entry->str;
            key_len = entry->len;

            /* Only scan string keys, ignore the rest (hashes, sets, etc.) */
            if (DB_KEY_TYPE_STRING != type(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password, key, (size_t) key_len)) {
                rc = gsu_ext->rc;
                continue;
            }
            rc = gsu_ext->rc;

            /* This returns database-interfacing Param,
             * it will return u.str or first fetch data
             * if not PM_UPTODATE (newly created) */
            char *zkey = metafy(key, key_len, META_DUP);
            HashNode hn = redis_get_node(ht, zkey);
            zsfree(zkey);

            func(hn, flags);
        }

        freeReplyObject(reply);
    } while (cursor != 0);
}
/* }}} */
/* FUNCTION: redis_hash_setfn {{{ */

/*
 * Replace database with new hash
 */

/**/
static void
redis_hash_setfn(Param pm, HashTable ht)
{
    size_t i, j;
    HashNode hn;
    char *key, *content;
    size_t key_len, content_len;
    redisContext *rc;
    redisReply *reply = NULL, *entry = NULL, *reply2 = NULL;
    struct gsu_scalar_ext *gsu_ext;
    int retry;

    if (!pm->u.hash || pm->u.hash == ht)
        return;

    gsu_ext = (struct gsu_scalar_ext *)pm->u.hash->tmpdata;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if (rc) {
        /* KEYS */
        reply = redisCommand(rc, "KEYS *");
        if (reply == NULL || reply->type != REDIS_REPLY_ARRAY) {
            if (reply)
                freeReplyObject(reply);
            goto do_retry;
        }

        for (j = 0; j < reply->elements; j++) {
            entry = reply->element[j];
            if (entry == NULL || entry->type != REDIS_REPLY_STRING) {
                continue;
            }
            key = entry->str;
            key_len = entry->len;

            /* Only scan string keys, ignore the rest (hashes, sets, etc.) */
            if (DB_KEY_TYPE_STRING != type(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password, key, (size_t) key_len)) {
                rc = gsu_ext->rc;
                continue;
            }
            rc = gsu_ext->rc;

            queue_signals();

            /* DEL */
            reply2 = redisCommand(rc, "DEL %b", key, (size_t) key_len);
            if (reply2) {
                freeReplyObject(reply2);
                reply2 = NULL;
            }
            if (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF))
                break;

            unqueue_signals();
        }
        freeReplyObject(reply);
    }

 do_retry:
    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
        else
            return;
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
        return;
    }

    no_database_action = 1;
    emptyhashtable(pm->u.hash);
    no_database_action = 0;

    if (!ht)
        return;

     /* Put new strings into database, having
      * their interfacing-Params created */

    retry = 0;
 retry2:
    rc = gsu_ext->rc;

    if (rc) {
        for (i = 0; i < ht->hsize; i++) {
            for (hn = ht->nodes[i]; hn; hn = hn->next) {
                struct value v;
                int umlen;
                char *umkey, *umval;

                v.isarr = v.flags = v.start = 0;
                v.end = -1;
                v.arr = NULL;
                v.pm = (Param) hn;

                /* Unmetafy key */
                umlen = 0;
                umkey = zsh_db_unmetafy_zalloc(v.pm->node.nam, &umlen);

                key = umkey;
                key_len = umlen;

                queue_signals();

                /* Unmetafy data */
                umval = zsh_db_unmetafy_zalloc(getstrvalue(&v), &umlen);

                content = umval;
                content_len = umlen;

                /* SET */
                reply = redisCommand(rc, "SET %b %b", key, (size_t) key_len, content, (size_t) content_len);
                if (reply)
                    freeReplyObject(reply);

                /* Free, restoring original length */
                zsh_db_set_length(umval, content_len);
                zsfree(umval);
                zsh_db_set_length(umkey, key_len);
                zsfree(umkey);

                unqueue_signals();
            }
        }

        /* Disconnect detection */
        if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry2;
        } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            zwarn("Aborting (no connection)");
        }
    }
    /* We reuse our hash, the input is to be deleted */
    deleteparamtable(ht);
}
/* }}} */
/* FUNCTION: redis_hash_unsetfn {{{ */

/**/
static void
redis_hash_unsetfn(Param pm, UNUSED(int exp))
{
    /* This will make database contents survive the
     * unset, as standard GSU will be put in place */
    redis_hash_untie(pm);

    /* Remember custom GSU structure assigned to
     * u.hash->tmpdata before hash gets deleted */
    struct gsu_scalar_ext * gsu_ext = pm->u.hash->tmpdata;

    no_database_action = 1;
    /* Uses normal unsetter (because gdbmuntie is called above).
     * Will delete all owned field-parameters and also hashtable. */
    pm->gsu.h->setfn(pm, NULL);
    no_database_action = 0;

    /* Don't need custom GSU structure with its
     * redisContext pointer anymore */
    zsfree(gsu_ext->redis_host_port);
    if (gsu_ext->password)
        zsfree(gsu_ext->password);
    zfree(gsu_ext, sizeof(struct gsu_scalar_ext));

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* FUNCTION: redis_hash_untie {{{ */

/**/
static void
redis_hash_untie(Param pm)
{
    struct gsu_scalar_ext *gsu_ext = (struct gsu_scalar_ext *)pm->u.hash->tmpdata;
    redisContext *rc = gsu_ext->rc;
    HashTable ht = pm->u.hash;

    if (rc) { /* <- paranoia ... actually not! used by the new no-connect lazy mode */
        redisFree(rc);
        fdtable[gsu_ext->fdesc] = FDT_UNUSED;

        /* Let hash fields know there's no backend */
        ((struct gsu_scalar_ext *)ht->tmpdata)->rc = NULL;
    }

    /* Remove from list of tied parameters */
    zsh_db_filter_arr(&zredis_tied, pm->node.nam);

    /* for completeness ... createspecialhash() should have an inverse */
    ht->getnode = ht->getnode2 = gethashnode2;
    ht->scantab = NULL;

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.h = &stdhash_gsu;
}
/* }}} */

/**************** STRING *****************/

/* FUNCTION: redis_str_getfn {{{ */

/**/
static char *
redis_str_getfn(Param pm)
{
    struct gsu_scalar_ext *gsu_ext;
    char *key;
    size_t key_len;
    redisContext *rc;
    redisReply *reply = NULL;
    int retry;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;
    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : "";
    }

    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if (rc) {
        reply = redisCommand(rc, "EXISTS %b", key, (size_t) key_len);
        if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
            freeReplyObject(reply);

            reply = redisCommand(rc, "GET %b", key, (size_t) key_len);
            if (reply && reply->type == REDIS_REPLY_STRING) {
                /* We have data – store it and return it */
                pm->node.flags |= PM_UPTODATE;

                /* Ensure there's no leak */
                if (pm->u.str) {
                    zsfree(pm->u.str);
                    pm->u.str = NULL;
                }

                /* Metafy returned data. All fits - metafy
                * can obtain data length to avoid using \0 */
                pm->u.str = metafy(reply->str, reply->len, META_DUP);
                freeReplyObject(reply);

                /* Can return pointer, correctly saved inside Param */
                return pm->u.str;
            } else if (reply) {
                freeReplyObject(reply);
            }
        } else if (reply) {
            freeReplyObject(reply);
        }
    }

    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
    }

    return "";
}
/* }}} */
/* FUNCTION: redis_str_setfn {{{ */

/**/
static void
redis_str_setfn(Param pm, char *val)
{
    char *key, *content;
    size_t key_len, content_len;
    redisContext *rc;
    redisReply *reply = NULL;
    struct gsu_scalar_ext *gsu_ext;
    int retry;

    /* Set is done on parameter and on database. */

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
    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if (rc) {
        if (val) {
            /* Unmetafy with exact zalloc size */
            int umlen = 0;
            char *umval = zsh_db_unmetafy_zalloc(val, &umlen);

            /* Store */
            content = umval;
            content_len = umlen;
            reply = redisCommand(rc, "SET %b %b", key, (size_t) key_len, content, (size_t) content_len);
            if (reply)
                freeReplyObject(reply);

            /* Free */
            zsh_db_set_length(umval, content_len);
            zsfree(umval);
        } else if (!yes_unsetting || gsu_ext->unset_deletes) {
            reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
            if (reply)
                freeReplyObject(reply);
        }
    }

    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if (val || !yes_unsetting || gsu_ext->unset_deletes) {
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
        }
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
    }
}
/* }}} */
/* FUNCTION: redis_str_unsetfn {{{ */

/**/
static void
redis_str_unsetfn(Param pm, UNUSED(int um))
{
    yes_unsetting = 1;
    /* Will clear the database */
    redis_str_setfn(pm, NULL);
    yes_unsetting = 0;

    /* Will detach from database and free custom memory */
    redis_str_untie(pm);

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* FUNCTION: redis_str_untie {{{ */

/**/
static void
redis_str_untie(Param pm)
{
    struct gsu_scalar_ext *gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    if (gsu_ext->rc) { /* <- paranoia ... actually not! used by the new no-connect lazy mode */
        redisFree(gsu_ext->rc);
        gsu_ext->rc = NULL;
        fdtable[gsu_ext->fdesc] = FDT_UNUSED;
    }

    /* Remove from list of tied parameters */
    zsh_db_filter_arr(&zredis_tied, pm->node.nam);

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.s = &stdscalar_gsu;

    /* Free gsu_ext */
    zsfree(gsu_ext->redis_host_port);
    if (gsu_ext->password)
        zsfree(gsu_ext->password);
    zsfree(gsu_ext->key);
    zfree(gsu_ext, sizeof(struct gsu_scalar_ext));
}
/* }}} */

/****************** SET ******************/

/* FUNCTION: redis_arrset_getfn {{{ */

/**/
char **
redis_arrset_getfn(Param pm)
{
    struct gsu_array_ext *gsu_ext;
    char *key;
    size_t key_len;
    redisContext *rc;
    redisReply *reply = NULL;
    int j, retry;

    gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.arr ? pm->u.arr : &my_nullarray;
    }

    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if (rc) {
        reply = redisCommand(rc, "EXISTS %b", key, (size_t) key_len);
        if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
            freeReplyObject(reply);
            reply = NULL;

            reply = redisCommand(rc, "SMEMBERS %b", key, (size_t) key_len);
            if (reply && reply->type == REDIS_REPLY_ARRAY) {
                /* We have data – store it and return it */
                pm->node.flags |= PM_UPTODATE;

                /* Ensure there's no leak */
                if (pm->u.arr) {
                    freearray(pm->u.arr);
                    pm->u.arr = NULL;
                }

                pm->u.arr = zalloc((reply->elements + 1) * sizeof(char*));

                for (j = 0; j < reply->elements; j++) {
                    if (NULL == reply->element[j]) {
                        pm->u.arr[j] = ztrdup("");
                        zwarn("Error 10 when fetching set elements");
                        continue;
                    } else if (reply->element[j]->type != REDIS_REPLY_STRING) {
                        pm->u.arr[j] = ztrdup("");
                        if (NULL != reply->element[j]->str && reply->element[j]->len > 0) {
                            zwarn("Error 11 when fetching set elements (message: %s)", reply->element[j]->str);
                        } else {
                            zwarn("Error 11 when fetching set elements");
                        }
                        continue;
                    }
                    /* Metafy returned data. All fits - metafy
                    * can obtain data length to avoid using \0 */
                    pm->u.arr[j] = metafy(reply->element[j]->str,
                                        reply->element[j]->len,
                                        META_DUP);
                }
                pm->u.arr[reply->elements] = NULL;

                freeReplyObject(reply);
                reply = NULL;

                /* Can return pointer, correctly saved inside Param */
                return pm->u.arr;
            } else if (reply) {
                freeReplyObject(reply);
                reply = NULL;
            }
        } else if (reply) {
            freeReplyObject(reply);
            reply = NULL;
        }
    }

    /* Disconnect detection */
    if (!rc || rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
        if (reply) {
            freeReplyObject(reply);
            reply = NULL;
        }
        if (retry) {
            zwarn("Aborting (no connection)");
            return &my_nullarray;
        }
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    }

    /* Array with 0 elements */
    return &my_nullarray;
}
/* }}} */
/* FUNCTION: redis_arrset_setfn {{{ */

/**/
void
redis_arrset_setfn(Param pm, char **val)
{
    char *key, *content;
    size_t key_len, content_len;
    int alen = 0, j;
    redisContext *rc;
    redisReply *reply = NULL;
    struct gsu_array_ext *gsu_ext;

    /* Set is done on parameter and on database. */

    /* Parameter */
    if (pm->u.arr && pm->u.arr != val) {
        freearray(pm->u.arr);
        pm->u.arr = NULL;
        pm->node.flags &= ~(PM_UPTODATE);
    }

    if (val) {
        uniqarray(val);
        alen = arrlen(val);
        pm->u.arr = val;
        pm->node.flags |= PM_UPTODATE;
    }

    /* Database */
    gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    if (!val && yes_unsetting && !gsu_ext->unset_deletes)
        return;


    int retry = 0;
retry:
    rc = gsu_ext->rc;
    if (rc) {
        reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
        if (reply) {
            freeReplyObject(reply);
            reply = NULL;
        }

        /* Disconnect detection */
        if (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            if (retry) {
                zwarn("Aborting (no connection)");
                return;
            }
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
            else
                return;
        }

        if (val) {
            for (j=0; j<alen; j ++) {
                /* Unmetafy with exact zalloc size */
                int umlen = 0;
                char *umval = zsh_db_unmetafy_zalloc(val[j], &umlen);

                /* Store */
                content = umval;
                content_len = umlen;
                reply = redisCommand(rc, "SADD %b %b", key, (size_t) key_len, content, (size_t) content_len);
                if (reply) {
                    freeReplyObject(reply);
                    reply = NULL;
                }
                if (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
                    break;
                }

                /* Free */
                zsh_db_set_length(umval, umlen);
                zsfree(umval);
            }
        }
    }

    /* Disconnect detection */
    if (!rc || rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
        if (retry) {
            zwarn("Aborting (no connection)");
            return;
        }
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    }

    if (pm->ename && val)
        arrfixenv(pm->ename, val);
}
/* }}} */
/* FUNCTION: redis_arrset_unsetfn {{{ */

/**/
void
redis_arrset_unsetfn(Param pm, UNUSED(int exp))
{
    yes_unsetting = 1;
    /* Will clear the database */
    redis_arrset_setfn(pm, NULL);
    yes_unsetting = 0;

    /* Will detach from database and free custom memory */
    redis_arrset_untie(pm);

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* FUNCTION: redis_arrset_untie {{{ */

/**/
static void
redis_arrset_untie(Param pm)
{
    struct gsu_array_ext *gsu_ext = (struct gsu_array_ext *) pm->gsu.a;

    if (gsu_ext->rc) { /* <- paranoia ... actually not! used by the new no-connect lazy mode */
        redisFree(gsu_ext->rc);
        gsu_ext->rc = NULL;
        fdtable[gsu_ext->fdesc] = FDT_UNUSED;
    }

    /* Remove from list of tied parameters */
    zsh_db_filter_arr(&zredis_tied, pm->node.nam);

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.a = &stdarray_gsu;

    /* Free gsu_ext */
    zsfree(gsu_ext->redis_host_port);
    if (gsu_ext->password)
        zsfree(gsu_ext->password);
    zsfree(gsu_ext->key);
    zfree(gsu_ext, sizeof(struct gsu_array_ext));
}
/* }}} */

/************ ZSET HASH ELEM *************/

/* FUNCTION: redis_zset_getfn {{{ */

/*
 * The param is actual param in hash – always, because
 * redis_zset_get_node creates every new key seen. However,
 * it might be not PM_UPTODATE - which means that database
 * wasn't yet queried.
 *
 * It will be left in this state if database doesn't
 * contain such key. That might be a drawback, maybe
 * setting to empty value has sense.
 */

/**/
static char *
redis_zset_getfn(Param pm)
{
    struct gsu_scalar_ext *gsu_ext;
    char *main_key, *key, *umkey;
    size_t main_key_len, key_len;
    redisContext *rc;
    redisReply *reply = NULL;
    int retry, umlen;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : "";
    }

    /* Unmetafy key. Redis fits nice into this
     * process, as it can use length of data */
    umlen = 0;
    umkey = zsh_db_unmetafy_zalloc(pm->node.nam, &umlen);

    key = umkey;
    key_len = umlen;

    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if(rc) {
        reply = redisCommand(rc, "ZSCORE %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
        if (reply && reply->type == REDIS_REPLY_STRING) {
            /* We have data – store it and return it */
            pm->node.flags |= PM_UPTODATE;

            /* Ensure there's no leak */
            if (pm->u.str) {
                zsfree(pm->u.str);
                pm->u.str = NULL;
            }

            /* Metafy returned data. All fits - metafy
             * can obtain data length to avoid using \0 */
            pm->u.str = metafy(reply->str, reply->len, META_DUP);
            freeReplyObject(reply);

            /* Free key, restoring its original length */
            zsh_db_set_length(umkey, key_len);
            zsfree(umkey);

            /* Can return pointer, correctly saved inside hash */
            return pm->u.str;
        } else if (reply) {
            freeReplyObject(reply);
        }
    }

    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
    }

    /* Free key, restoring its original length */
    zsh_db_set_length(umkey, key_len);
    zsfree(umkey);

    return "";
}
/* }}} */
/* FUNCTION: redis_zset_setfn {{{ */

/**/
static void
redis_zset_setfn(Param pm, char *val)
{
    char *main_key, *key, *content;
    size_t main_key_len, key_len, content_len;
    struct gsu_scalar_ext *gsu_ext;
    redisContext *rc;
    redisReply *reply = NULL;
    int retry;

    /* Set is done on parameter and on database. */

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
    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    /* Can be NULL, when calling unset after untie */
    if (no_database_action == 0) {
        if (rc) {
            int umlen = 0;
            char *umkey = zsh_db_unmetafy_zalloc(pm->node.nam, &umlen);

            key = umkey;
            key_len = umlen;

            main_key = gsu_ext->key;
            main_key_len = gsu_ext->key_len;

            if (val) {
                /* Unmetafy with exact zalloc size */
                char *umval = zsh_db_unmetafy_zalloc(val, &umlen);

                content = umval;
                content_len = umlen;

                /* ZADD myzset 1.0 element1 */
                reply = redisCommand(rc, "ZADD %b %b %b",
                                    main_key, (size_t) main_key_len,
                                    content, (size_t) content_len,
                                    key, (size_t) key_len );
                if (reply)
                    freeReplyObject(reply);

                /* Free */
                zsh_db_set_length(umval, content_len);
                zsfree(umval);
            } else {
                reply = redisCommand(rc, "ZREM %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
                if (reply)
                    freeReplyObject(reply);
            }

            /* Free key */
            zsh_db_set_length(umkey, key_len);
            zsfree(umkey);
        }

        if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
        } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            zwarn("Aborting (no connection)");
        }
    }
}
/* }}} */
/* FUNCTION: redis_zset_unsetfn {{{ */

/**/
static void
redis_zset_unsetfn(Param pm, UNUSED(int um))
{
    /* Set with NULL */
    redis_zset_setfn(pm, NULL);
}
/* }}} */

/*************** ZSET HASH ***************/

/* FUNCTION: redis_zset_get_node {{{ */

/**/
static HashNode
redis_zset_get_node(HashTable ht, const char *name)
{
    HashNode hn = gethashnode2(ht, name);
    Param val_pm = (Param) hn;

    /* Entry for key doesn't exist? Create it now,
     * it will be interfacing between the database
     * and Zsh - through special gsu. So, any seen
     * key results in new interfacing parameter.
     *
     * Add the Param to its hash, it is not PM_UPTODATE.
     * It will be loaded from database *and filled*
     * or left in that state if the database doesn't
     * contain it.
     */

    if (!val_pm) {
        val_pm = (Param) zshcalloc(sizeof (*val_pm));
        val_pm->node.flags = PM_SCALAR | PM_HASHELEM; /* no PM_UPTODATE */
        val_pm->gsu.s = (GsuScalar) ht->tmpdata;
        ht->addnode(ht, ztrdup(name), val_pm); // sets pm->node.nam
    }

    return (HashNode) val_pm;
}
/* }}} */
/* FUNCTION: zset_scan_keys {{{ */

/**/
static void
zset_scan_keys(HashTable ht, ScanFunc func, int flags)
{
    char *main_key, *key;
    size_t main_key_len, key_len, j;
    unsigned long long cursor = 0;
    redisContext *rc;
    redisReply *reply = NULL, *reply2 = NULL;
    struct gsu_scalar_ext *gsu_ext;

    gsu_ext = (struct gsu_scalar_ext *) ht->tmpdata;
    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* Iterate keys adding them to hash, so we have Param to use in `func` */
    do {
        int retry = 0;
    retry:
        rc = gsu_ext->rc;

        if (rc)
            reply = redisCommand(rc, "ZSCAN %b %llu", main_key, (size_t) main_key_len, cursor);

        /* Disconnect detection */
        if (!rc || rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            if (reply)
                freeReplyObject(reply);
            if (retry) {
                zwarn("Aborting (not connected)");
                break;
            }
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
            else
                break;
        }

        if (reply == NULL || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply && reply->type == REDIS_REPLY_ERROR) {
                zwarn("Aborting, problem occured during ZSCAN: %s", reply->str);
            } else {
                zwarn("Problem occured during ZSCAN, no error message available, aborting");
            }
            if (reply)
                freeReplyObject(reply);
            break;
        }

        /* Get new cursor */
        if (reply->element[0]->type == REDIS_REPLY_STRING) {
            cursor = strtoull(reply->element[0]->str, NULL, 10);
        } else {
            zwarn("Error 2 occured during ZSCAN");
            break;
        }

        reply2 = reply->element[1];
        if (reply2 == NULL || reply2->type != REDIS_REPLY_ARRAY) {
            zwarn("Error 3 occured during ZSCAN");
            break;
        }

        for (j = 0; j < reply2->elements; j+= 2) {
            redisReply *entry = reply2->element[j];
            if (entry == NULL || entry->type != REDIS_REPLY_STRING) {
                continue;
            }

            key = entry->str;
            key_len = entry->len;

            /* This returns database-interfacing Param,
            * it will return u.str or first fetch data
            * if not PM_UPTODATE (newly created) */
            char *zkey = metafy(key, key_len, META_DUP);
            HashNode hn = redis_zset_get_node(ht, zkey);
            zsfree(zkey);

            func(hn, flags);
        }
        freeReplyObject(reply);
    } while (cursor != 0);
}
/* }}} */
/* FUNCTION: redis_hash_zset_setfn {{{ */

/*
 * Replace database with new hash
 */

/**/
static void
redis_hash_zset_setfn(Param pm, HashTable ht)
{
    HashNode hn;
    char *main_key, *key, *content;
    size_t main_key_len, key_len, content_len, i;
    redisContext *rc;
    redisReply *reply = NULL;
    struct gsu_scalar_ext *gsu_ext;
    int retry;

    if (!pm->u.hash || pm->u.hash == ht)
        return;

    gsu_ext = (struct gsu_scalar_ext *) pm->u.hash->tmpdata;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* PRUNE */
    if (rc) {
        reply = redisCommand(rc, "ZREMRANGEBYSCORE %b -inf +inf", main_key, (size_t) main_key_len);
        if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
            zwarn("Error 4 occured, database not updated (no purge of zset)");
            if (reply)
                freeReplyObject(reply);
            goto do_retry;
        }
        freeReplyObject(reply);
    }

 do_retry:
    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
        else
            return;
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
        return;
    }

    no_database_action = 1;
    emptyhashtable(pm->u.hash);
    no_database_action = 0;

    if (!ht)
        return;

     /* Put new strings into database, having
      * their interfacing-Params created */

    retry = 0;
 retry2:
    rc = gsu_ext->rc;

    if (rc) {
        for (i = 0; i < ht->hsize; i++) {
            for (hn = ht->nodes[i]; hn; hn = hn->next) {
                struct value v;
                int umlen;
                char *umkey, *umval;

                v.isarr = v.flags = v.start = 0;
                v.end = -1;
                v.arr = NULL;
                v.pm = (Param) hn;

                /* Unmetafy key */
                umlen = 0;
                umkey = zsh_db_unmetafy_zalloc(v.pm->node.nam, &umlen);

                key = umkey;
                key_len = umlen;

                queue_signals();

                /* Unmetafy data */
                umval = zsh_db_unmetafy_zalloc(getstrvalue(&v), &umlen);

                content = umval;
                content_len = umlen;

                /* ZADD myzset 1.0 element1 */
                reply = redisCommand(rc, "ZADD %b %b %b", main_key, (size_t) main_key_len,
                                    content, (size_t) content_len,
                                    key, (size_t) key_len);
                if (reply) {
                    freeReplyObject(reply);
                    reply = NULL;
                }
                if (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF))
                    break;

                /* Free, restoring original length */
                zsh_db_set_length(umval, content_len);
                zsfree(umval);
                zsh_db_set_length(umkey, key_len);
                zsfree(umkey);

                unqueue_signals();
            }
        }

        /* Disconnect detection */
        if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry2;
        } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            zwarn("Aborting (no connection)");
        }
    }
    /* We reuse our hash, the input is to be deleted */
    deleteparamtable(ht);
}
/* }}} */
/* FUNCTION: redis_hash_zset_unsetfn {{{ */

/**/
static void
redis_hash_zset_unsetfn(Param pm, UNUSED(int exp))
{
    /* Remember custom GSU structure assigned to
     * u.hash->tmpdata before hash gets deleted */
    struct gsu_scalar_ext * gsu_ext = pm->u.hash->tmpdata;

    if (!gsu_ext->unset_deletes || doing_untie) {
        /* This will make database contents survive the
        * unset, as standard GSU will be put in place */
        redis_hash_zset_untie(pm);
        no_database_action = 1;
    }

    /* Uses normal unsetter (because gdbmuntie is called above).
     * Will delete all owned field-parameters and also hashtable. */
    pm->gsu.h->setfn(pm, NULL);

    if (gsu_ext->unset_deletes && !doing_untie) {
        redis_hash_zset_untie(pm);
    } else {
        no_database_action = 0;
    }

    /* Don't need custom GSU structure with its
     * redisContext pointer anymore */
    zsfree(gsu_ext->redis_host_port);
    if (gsu_ext->password)
        zsfree(gsu_ext->password);
    zsfree(gsu_ext->key);
    zfree(gsu_ext, sizeof(struct gsu_scalar_ext));

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* FUNCTION: redis_hash_zset_untie {{{ */

/**/
static void
redis_hash_zset_untie(Param pm)
{
    struct gsu_scalar_ext *gsu_ext = (struct gsu_scalar_ext *)pm->u.hash->tmpdata;
    redisContext *rc = gsu_ext->rc;
    HashTable ht = pm->u.hash;

    if (rc) { /* <- paranoia ... actually not! used by the new no-connect lazy mode */
        redisFree(rc);
        fdtable[gsu_ext->fdesc] = FDT_UNUSED;

        /* Let hash fields know there's no backend */
        ((struct gsu_scalar_ext *)ht->tmpdata)->rc = NULL;
    }

    /* Remove from list of tied parameters */
    zsh_db_filter_arr(&zredis_tied, pm->node.nam);

    /* for completeness ... createspecialhash() should have an inverse */
    ht->getnode = ht->getnode2 = gethashnode2;
    ht->scantab = NULL;

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.h = &stdhash_gsu;
}
/* }}} */

/*************** ZSET UTIL ***************/

/* FUNCTION: bin_zrzset {{{ */

/**/
static int
bin_zrzset(char *nam, char **args, Options ops, UNUSED(int func))
{
    Param pm;
    int i;
    const char *pmname;

    if (OPT_ISSET(ops,'h')) {
        zrzset_usage();
        return 0;
    }

    pmname = *args;

    if (!pmname) {
        zwarnnam(nam, "zset parameter name (to be copied to $reply) is required");
        return 1;
    }

    pm = (Param) paramtab->getnode(paramtab, pmname);
    if(!pm) {
        zwarnnam(nam, "no such parameter: %s", pmname);
        return 1;
    }

    if (pm->gsu.h == &redis_hash_gsu) {
        zwarnnam(nam, "`%s' is a main-storage hash, aborting", pmname);
    } else if(pm->gsu.s->getfn == &redis_str_getfn) {
        zwarnnam(nam, "`%s' is a string parameter, aborting", pmname);
    } else if(pm->gsu.a->getfn == &redis_arrset_getfn) {
        zwarnnam(nam, "`%s' is a set (array) parameter, aborting", pmname);
    } else if(pm->gsu.h == &hash_zset_gsu) {
        char **arr, *main_key;
        size_t main_key_len;
        redisContext *rc;
        redisReply *reply = NULL, *entry = NULL;
        struct gsu_scalar_ext *gsu_ext;

        gsu_ext = (struct gsu_scalar_ext *) pm->u.hash->tmpdata;
        rc = gsu_ext->rc;
        main_key = gsu_ext->key;
        main_key_len = gsu_ext->key_len;

        if (rc) {
            reply = redisCommand(rc, "ZRANGE %b 0 -1", main_key, (size_t) main_key_len);
            if (reply == NULL || reply->type != REDIS_REPLY_ARRAY) {
                zwarn("Error 12 occured (ZRANGE call), aborting");
                if (reply)
                    freeReplyObject(reply);
                return 1;
            }

            arr = zshcalloc((reply->elements+1) * sizeof(char *));
            for (i = 0; i < reply->elements; i++) {
                entry = reply->element[i];
                arr[i] = metafy(entry->str, entry->len, META_DUP);
            }
            arr[reply->elements] = NULL;
            freeReplyObject(reply);

            pm = assignaparam("reply", arr, 0);
            return 0;
        } else {
            zwarn("Error 13 occured: no database connection");
            return 1;
        }
    } else if(pm->gsu.h == &hash_hset_gsu) {
        zwarnnam(nam, "`%s' is a hset (hash) parameter, aborting", pmname);
    } else if(pm->gsu.a->getfn == &redis_arrlist_getfn) {
        zwarnnam(nam, "`%s' is a list (array) parameter, aborting", pmname);
    } else {
        zwarnnam(nam, "not a tied zredis parameter: `%s', $reply array unchanged", pmname);
    }

    return 1;
}
/* }}} */

/************ HSET HASH ELEM *************/

/* FUNCTION: redis_hset_getfn {{{ */

/*
 * The param is actual param in hash – always, because
 * redis_hset_get_node creates every new key seen. However,
 * it might be not PM_UPTODATE - which means that database
 * wasn't yet queried.
 *
 * It will be left in this state if database doesn't
 * contain such key. That might be a drawback, maybe
 * setting to empty value has sense.
 */

/**/
static char *
redis_hset_getfn(Param pm)
{
    struct gsu_scalar_ext *gsu_ext;
    char *main_key, *key, *umkey;
    size_t main_key_len, key_len;
    redisContext *rc;
    redisReply *reply = NULL;
    int retry, umlen;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : "";
    }

    /* Unmetafy key. Redis fits nice into this
     * process, as it can use length of data */
    umlen = 0;
    umkey = zsh_db_unmetafy_zalloc(pm->node.nam, &umlen);

    key = umkey;
    key_len = umlen;

    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if (rc) {
        reply = redisCommand(rc, "HGET %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
        if (reply && reply->type == REDIS_REPLY_STRING) {
            /* We have data – store it and return it */
            pm->node.flags |= PM_UPTODATE;

            /* Ensure there's no leak */
            if (pm->u.str) {
                zsfree(pm->u.str);
                pm->u.str = NULL;
            }

            /* Metafy returned data. All fits - metafy
             * can obtain data length to avoid using \0 */
            pm->u.str = metafy(reply->str, reply->len, META_DUP);
            freeReplyObject(reply);

            /* Free key, restoring its original length */
            zsh_db_set_length(umkey, key_len);
            zsfree(umkey);

            /* Can return pointer, correctly saved inside hash */
            return pm->u.str;
        } else if (reply) {
            freeReplyObject(reply);
        }
    }

    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
    }

    /* Free key, restoring its original length */
    zsh_db_set_length(umkey, key_len);
    zsfree(umkey);

    return "";
}
/* }}} */
/* FUNCTION: redis_hset_setfn {{{ */

/**/
static void
redis_hset_setfn(Param pm, char *val)
{
    char *main_key, *key, *content;
    size_t main_key_len, key_len, content_len;
    struct gsu_scalar_ext *gsu_ext;
    redisContext *rc;
    redisReply *reply = NULL;
    int retry;

    /* Set is done on parameter and on database. */

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
    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    /* Can be NULL, when calling unset after untie */
    if (no_database_action == 0) {
        if (rc) {
            int umlen = 0;
            char *umkey = zsh_db_unmetafy_zalloc(pm->node.nam, &umlen);

            key = umkey;
            key_len = umlen;

            main_key = gsu_ext->key;
            main_key_len = gsu_ext->key_len;

            if (val) {
                /* Unmetafy with exact zalloc size */
                char *umval = zsh_db_unmetafy_zalloc(val, &umlen);

                content = umval;
                content_len = umlen;

                /* HSET myhset field1 value */
                reply = redisCommand(rc, "HSET %b %b %b",
                                    main_key, (size_t) main_key_len,
                                    key, (size_t) key_len,
                                    content, (size_t) content_len);
                if (reply)
                    freeReplyObject(reply);

                /* Free */
                zsh_db_set_length(umval, content_len);
                zsfree(umval);
            } else {
                reply = redisCommand(rc, "HDEL %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
                if (reply)
                    freeReplyObject(reply);
            }

            /* Free key */
            zsh_db_set_length(umkey, key_len);
            zsfree(umkey);
        }

        if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
        } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            zwarn("Aborting (no connection)");
        }
    }
}
/* }}} */
/* FUNCTION: redis_hset_unsetfn {{{ */

/**/
static void
redis_hset_unsetfn(Param pm, UNUSED(int um))
{
    /* Set with NULL */
    redis_hset_setfn(pm, NULL);
}
/* }}} */

/*************** HSET HASH ***************/

/* FUNCTION: redis_hset_get_node {{{ */

/**/
static HashNode
redis_hset_get_node(HashTable ht, const char *name)
{
    HashNode hn = gethashnode2(ht, name);
    Param val_pm = (Param) hn;

    /* Entry for key doesn't exist? Create it now,
     * it will be interfacing between the database
     * and Zsh - through special gsu. So, any seen
     * key results in new interfacing parameter.
     *
     * Add the Param to its hash, it is not PM_UPTODATE.
     * It will be loaded from database *and filled*
     * or left in that state if the database doesn't
     * contain it.
     */

    if (!val_pm) {
        val_pm = (Param) zshcalloc(sizeof (*val_pm));
        val_pm->node.flags = PM_SCALAR | PM_HASHELEM; /* no PM_UPTODATE */
        val_pm->gsu.s = (GsuScalar) ht->tmpdata;
        ht->addnode(ht, ztrdup(name), val_pm); // sets pm->node.nam
    }

    return (HashNode) val_pm;
}
/* }}} */
/* FUNCTION: hset_scan_keys {{{ */

/**/
static void
hset_scan_keys(HashTable ht, ScanFunc func, int flags)
{
    char *main_key, *key;
    size_t main_key_len, key_len, j;
    unsigned long long cursor = 0;
    redisContext *rc;
    redisReply *reply = NULL, *reply2 = NULL;
    struct gsu_scalar_ext *gsu_ext;

    gsu_ext = (struct gsu_scalar_ext *) ht->tmpdata;
    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* Iterate keys adding them to hash, so we have Param to use in `func` */
    do {
        int retry = 0;
    retry:
        rc = gsu_ext->rc;

        if (rc)
            reply = redisCommand(rc, "HSCAN %b %llu", main_key, (size_t) main_key_len, cursor);

        /* Disconnect detection */
        if (!rc || rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            if (reply)
                freeReplyObject(reply);
            if (retry) {
                zwarn("Aborting (not connected)");
                break;
            }
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                // The same cursor
                goto retry;
            else
                break;
        }

        if (reply == NULL || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
            if (reply && reply->type == REDIS_REPLY_ERROR) {
                zwarn("Aborting, problem occured during HSCAN: %s", reply->str);
            } else {
                zwarn("Problem occured during HSCAN, no error message available, aborting");
            }
            if (reply)
                freeReplyObject(reply);
            break;
        }

        /* Get new cursor */
        if (reply->element[0]->type == REDIS_REPLY_STRING) {
            cursor = strtoull(reply->element[0]->str, NULL, 10);
        } else {
            zwarn("Error 2 occured during HSCAN");
            break;
        }

        reply2 = reply->element[1];
        if (reply2 == NULL || reply2->type != REDIS_REPLY_ARRAY) {
            zwarn("Error 3 occured during HSCAN");
            break;
        }

        for (j = 0; j < reply2->elements; j+= 2) {
            redisReply *entry = reply2->element[j];
            if (entry == NULL || entry->type != REDIS_REPLY_STRING) {
                continue;
            }

            key = entry->str;
            key_len = entry->len;

            /* This returns database-interfacing Param,
             * it will return u.str or first fetch data
             * if not PM_UPTODATE (newly created) */
            char *zkey = metafy(key, key_len, META_DUP);
            HashNode hn = redis_hset_get_node(ht, zkey);
            zsfree(zkey);

            func(hn, flags);
        }
        freeReplyObject(reply);
    } while (cursor != 0);
}
/* }}} */
/* FUNCTION: redis_hash_hset_setfn {{{ */

/*
 * Replace database with new hash
 */

/**/
static void
redis_hash_hset_setfn(Param pm, HashTable ht)
{
    HashNode hn;
    char *main_key, *key, *content;
    size_t main_key_len, key_len, content_len, i;
    redisContext *rc;
    redisReply *reply = NULL, *reply2 = NULL, *entry = NULL;
    struct gsu_scalar_ext *gsu_ext;
    int retry;

    if (!pm->u.hash || pm->u.hash == ht)
        return;

    gsu_ext = (struct gsu_scalar_ext *) pm->u.hash->tmpdata;

    retry = 0;
 retry:
    rc = gsu_ext->rc;
    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* PRUNE */
    if (rc) {
        reply = redisCommand(rc, "HKEYS %b", main_key, (size_t) main_key_len);
        if (reply == NULL || reply->type != REDIS_REPLY_ARRAY) {
            zwarn("Error 5 occured (redis communication), database and tied hash not updated");
            if (reply) {
                freeReplyObject(reply);
                reply = NULL;
            }
            goto do_retry;
        }

        for (i = 0; i < reply->elements; i++) {
            entry = reply->element[i];
            if (NULL == entry || entry->type != REDIS_REPLY_STRING) {
                if (entry && entry->str && entry->len > 0) {
                    zwarn("Error 6 when replacing database (element %d, message: %s)", i+1, entry->str);
                } else {
                    zwarn("Error 6 when replacing database (element %d)", i+1);
                }
                continue;
            }

            /* HDEL myhset myfield */
            reply2 = redisCommand(rc, "HDEL %b %b", main_key, (size_t) main_key_len, entry->str, (size_t) entry->len);
            if (NULL == reply2 || reply2->type != REDIS_REPLY_INTEGER) {
                zwarn("Error 7 when replacing database");
                continue;
            }
            freeReplyObject(reply2);
            reply2 = NULL;
            if (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF))
                break;
        }
    }

 do_retry:

    if (reply) {
        freeReplyObject(reply);
        reply = NULL;
    }

    if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
        else
            return;
    } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
        zwarn("Aborting (no connection)");
        return;
    }

    no_database_action = 1;
    emptyhashtable(pm->u.hash);
    no_database_action = 0;

    if (!ht)
        return;

     /* Put new strings into database, having
      * their interfacing-Params created */

    retry = 0;
 retry2:
    rc = gsu_ext->rc;

    if (rc) {
        for (i = 0; i < ht->hsize; i++) {
            for (hn = ht->nodes[i]; hn; hn = hn->next) {
                struct value v;
                int umlen;
                char *umkey, *umval;

                v.isarr = v.flags = v.start = 0;
                v.end = -1;
                v.arr = NULL;
                v.pm = (Param) hn;

                /* Unmetafy key */
                umlen = 0;
                umkey = zsh_db_unmetafy_zalloc(v.pm->node.nam, &umlen);

                key = umkey;
                key_len = umlen;

                queue_signals();

                /* Unmetafy data */
                umval = zsh_db_unmetafy_zalloc(getstrvalue(&v), &umlen);

                content = umval;
                content_len = umlen;

                /* HSET myhset element1 value */
                reply = redisCommand(rc, "HSET %b %b %b", main_key, (size_t) main_key_len,
                                    key, (size_t) key_len,
                                    content, (size_t) content_len);
                if (reply)
                    freeReplyObject(reply);

                /* Free, restoring original length */
                zsh_db_set_length(umval, content_len);
                zsfree(umval);
                zsh_db_set_length(umkey, key_len);
                zsfree(umkey);

                unqueue_signals();
            }
        }

        /* Disconnect detection */
        if (!retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry2;
        } else if (retry && (!rc || (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)))) {
            zwarn("Aborting (no connection)");
        }
    }
    /* We reuse our hash, the input is to be deleted */
    deleteparamtable(ht);
}
/* }}} */
/* FUNCTION: redis_hash_hset_unsetfn {{{ */

/**/
static void
redis_hash_hset_unsetfn(Param pm, UNUSED(int exp))
{
    /* Remember custom GSU structure assigned to
     * u.hash->tmpdata before hash gets deleted */
    struct gsu_scalar_ext * gsu_ext = pm->u.hash->tmpdata;

    if (!gsu_ext->unset_deletes || doing_untie) {
        /* This will make database contents survive the
        * unset, as standard GSU will be put in place */
        redis_hash_hset_untie(pm);
        no_database_action = 1;
    }

    /* Uses normal unsetter (because gdbmuntie is called above).
     * Will delete all owned field-parameters and also hashtable. */
    pm->gsu.h->setfn(pm, NULL);

    if (gsu_ext->unset_deletes && !doing_untie) {
        redis_hash_hset_untie(pm);
    } else {
        no_database_action = 0;
    }

    /* Don't need custom GSU structure with its
     * redisContext pointer anymore */
    zsfree(gsu_ext->redis_host_port);
    if (gsu_ext->password)
        zsfree(gsu_ext->password);
    zsfree(gsu_ext->key);
    zfree(gsu_ext, sizeof(struct gsu_scalar_ext));

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* FUNCTION: redis_hash_hset_untie {{{ */

/**/
static void
redis_hash_hset_untie(Param pm)
{
    struct gsu_scalar_ext *gsu_ext = (struct gsu_scalar_ext *)pm->u.hash->tmpdata;
    redisContext *rc = gsu_ext->rc;
    HashTable ht = pm->u.hash;

    if (rc) { /* <- paranoia ... actually not! used by the new no-connect lazy mode */
        redisFree(rc);
        fdtable[gsu_ext->fdesc] = FDT_UNUSED;

        /* Let hash fields know there's no backend */
        ((struct gsu_scalar_ext *)ht->tmpdata)->rc = NULL;
    }

    /* Remove from list of tied parameters */
    zsh_db_filter_arr(&zredis_tied, pm->node.nam);

    /* for completeness ... createspecialhash() should have an inverse */
    ht->getnode = ht->getnode2 = gethashnode2;
    ht->scantab = NULL;

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.h = &stdhash_gsu;
}
/* }}} */

/****************** LIST *****************/

/* FUNCTION: redis_arrlist_getfn {{{ */

/**/
char **
redis_arrlist_getfn(Param pm)
{
    struct gsu_array_ext *gsu_ext;
    char *key;
    size_t key_len;
    redisContext *rc;
    redisReply *reply = NULL;
    int j, retry;

    gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.arr ? pm->u.arr : &my_nullarray;
    }

    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    retry = 0;
 retry:
    rc = gsu_ext->rc;

    if (rc) {
        reply = redisCommand(rc, "EXISTS %b", key, (size_t) key_len);
        if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
            freeReplyObject(reply);
            reply = NULL;

            reply = redisCommand(rc, "LRANGE %b 0 -1", key, (size_t) key_len);
            if (reply && reply->type == REDIS_REPLY_ARRAY) {
                /* We have data – store it and return it */
                pm->node.flags |= PM_UPTODATE;

                /* Ensure there's no leak */
                if (pm->u.arr) {
                    freearray(pm->u.arr);
                    pm->u.arr = NULL;
                }

                pm->u.arr = zalloc((reply->elements + 1) * sizeof(char*));

                for (j = 0; j < reply->elements; j++) {
                    if (NULL == reply->element[j]) {
                        pm->u.arr[j] = ztrdup("");
                        zwarn("Error 8 when fetching elements");
                        continue;
                    } else if (reply->element[j]->type != REDIS_REPLY_STRING) {
                        pm->u.arr[j] = ztrdup("");
                        if (NULL != reply->element[j]->str && reply->element[j]->len > 0) {
                            zwarn("Error 9 when fetching elements (message: %s)", reply->element[j]->str);
                        } else {
                            zwarn("Error 9 when fetching elements");
                        }
                        continue;
                    }
                    /* Metafy returned data. All fits - metafy
                    * can obtain data length to avoid using \0 */
                    pm->u.arr[j] = metafy(reply->element[j]->str,
                                        reply->element[j]->len,
                                        META_DUP);
                }
                pm->u.arr[reply->elements] = NULL;

                freeReplyObject(reply);
                reply = NULL;

                /* Can return pointer, correctly saved inside Param */
                return pm->u.arr;
            } else if (reply) {
                freeReplyObject(reply);
                reply = NULL;
            }
        } else if (reply) {
            freeReplyObject(reply);
            reply = NULL;
        }
    }

    /* Disconnect detection */
    if (!rc || rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
        if (reply) {
            freeReplyObject(reply);
            reply = NULL;
        }
        if (retry) {
            zwarn("Aborting (no connection)");
            return &my_nullarray;
        }
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    }

    /* Array with 0 elements */
    return &my_nullarray;
}
/* }}} */
/* FUNCTION: redis_arrlist_setfn {{{ */

/**/
void
redis_arrlist_setfn(Param pm, char **val)
{
    char *key, *content;
    size_t key_len, content_len;
    int alen = 0, j;
    redisContext *rc;
    redisReply *reply = NULL;
    struct gsu_array_ext *gsu_ext;

    /* Set is done on parameter and on database. */

    /* Parameter */
    if (pm->u.arr && pm->u.arr != val) {
        freearray(pm->u.arr);
        pm->u.arr = NULL;
        pm->node.flags &= ~(PM_UPTODATE);
    }

    if (val) {
        alen = arrlen(val);
        pm->u.arr = val;
        pm->node.flags |= PM_UPTODATE;
    }

    /* Database */
    gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    if (!val && yes_unsetting && !gsu_ext->unset_deletes)
        return;

    int retry = 0;
retry:
    rc = gsu_ext->rc;
    if (rc) {
        reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
        if (reply) {
            freeReplyObject(reply);
            reply = NULL;
        }

        /* Disconnect detection */
        if (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
            if (retry) {
                zwarn("Aborting (no connection)");
                return;
            }
            retry = 1;
            if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
                goto retry;
            else
                return;
        }

        if (val) {
            for (j=0; j<alen; j ++) {
                /* Unmetafy with exact zalloc size */
                int umlen = 0;
                char *umval = zsh_db_unmetafy_zalloc(val[j], &umlen);

                /* Store */
                content = umval;
                content_len = umlen;
                reply = redisCommand(rc, "RPUSH %b %b", key, (size_t) key_len, content, (size_t) content_len);
                if (reply) {
                    freeReplyObject(reply);
                    reply = NULL;
                }
                if (rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
                    break;
                }

                /* Free */
                zsh_db_set_length(umval, umlen);
                zsfree(umval);
            }
        }
    }

    /* Disconnect detection */
    if (!rc || rc->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
        if (retry) {
            zwarn("Aborting (no connection)");
            return;
        }
        retry = 1;
        if(reconnect(&gsu_ext->rc, &gsu_ext->fdesc, gsu_ext->redis_host_port, gsu_ext->password))
            goto retry;
    }

    if (pm->ename && val)
        arrfixenv(pm->ename, val);
}
/* }}} */
/* FUNCTION: redis_arrlist_unsetfn {{{ */

/**/
void
redis_arrlist_unsetfn(Param pm, UNUSED(int exp))
{
    yes_unsetting = 1;
    /* Will clear the database */
    redis_arrlist_setfn(pm, NULL);
    yes_unsetting = 0;

    /* Will detach from database and free custom memory */
    redis_arrlist_untie(pm);

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* FUNCTION: redis_arrlist_untie {{{ */

/**/
static void
redis_arrlist_untie(Param pm)
{
    struct gsu_array_ext *gsu_ext = (struct gsu_array_ext *) pm->gsu.a;

    if (gsu_ext->rc) { /* <- paranoia ... actually not! used by the new no-connect lazy mode */
        redisFree(gsu_ext->rc);
        gsu_ext->rc = NULL;
        fdtable[gsu_ext->fdesc] = FDT_UNUSED;
    }

    /* Remove from list of tied parameters */
    zsh_db_filter_arr(&zredis_tied, pm->node.nam);

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.a = &stdarray_gsu;

    /* Free gsu_ext */
    zsfree(gsu_ext->redis_host_port);
    if (gsu_ext->password)
        zsfree(gsu_ext->password);
    zsfree(gsu_ext->key);
    zfree(gsu_ext, sizeof(struct gsu_array_ext));
}
/* }}} */

/*************** MAIN CODE ***************/

/* ARRAY features {{{ */
static struct features module_features = {
    bintab, sizeof(bintab)/sizeof(*bintab),
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
    zredis_tied = zshcalloc((1) * sizeof(char *));
    zsh_db_register_backend("db/redis", redis_main_entry);
    return 0;
}
/* }}} */
/* FUNCTION: cleanup_ {{{ */

/**/
int
cleanup_(Module m)
{
    zsh_db_unregister_backend("db/redis");

    /* This frees `zredis_tied` */
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

/**************** UTILITY ****************/

/* FUNCTION: createhash {{{ */
static Param
createhash(char *name, int flags, int which)
{
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
    if ( which == 0 ) {
        ht->getnode = ht->getnode2 = redis_get_node;
        ht->scantab = scan_keys;
    } else if ( which == 1 ) {
        ht->getnode = ht->getnode2 = redis_zset_get_node;
        ht->scantab = zset_scan_keys;
    } else if ( which == 2 ) {
        ht->getnode = ht->getnode2 = redis_hset_get_node;
        ht->scantab = hset_scan_keys;
    } else {
        return NULL;
    }

    return pm;
}
/* }}} */
/* FUNCTION: deletehashparam {{{ */
static void
deletehashparam(Param tied_param, const char *pmname) {
    zsh_db_standarize_hash(tied_param);
    paramtab->removenode(paramtab, pmname);
    deletehashtable(tied_param->u.hash);
    tied_param->u.hash = NULL;
    paramtab->freenode(&tied_param->node);
}
/* }}} */
/* FUNCTION: parse_host_string {{{ */

static void
parse_host_string(const char *input, char *resource_name, int size, char **host, int *port, int *db_index, char **key)
{
    strncpy(resource_name, input, size-1);
    resource_name[size-1] = '\0';

    /* Parse -f argument */
    char *processed = resource_name;
    char *port_start, *key_start, *needle;
    if ((port_start = strchr(processed, ':'))) {
        if (port_start[1] != '\0') {
            if ((needle = strchr(port_start+1, '/'))) {
                /* Port with following database index */
                *needle = '\0';
                if (port_start[1] != '\0')
                    *port = atoi(port_start + 1);
                processed = needle+1;
            } else {
                /* Port alone */
                *port = atoi(port_start + 1);
                processed = NULL;
            }
        } else {
            /* Empty port, nothing follows */
            processed = NULL;
        }

        /* Process host name */
        *port_start = '\0';
        if (resource_name[0] != '\0')
            *host = resource_name;
    } else {
        /* No-port track */
        if ((needle = strchr(processed, '/'))) {
            *needle = '\0';
            *host = resource_name;
            processed = needle+1;
        }
    }

    /* In this place host name and port are already parsed */

    /* Database index */
    if (processed) {
        if ((key_start = strchr(processed, '/'))) {
            /* With key-following track */
            *key_start = '\0';
            if (processed[0] != '\0')
                *db_index = atoi(processed);
            processed = key_start + 1;
        } else {
            /* Without key-following track */
            if (processed[0] != '\0')
                *db_index = atoi(processed);
            processed = NULL;
        }
    }

    /* Key */
    if (processed) {
        if (processed[0] != '\0')
            *key = processed;
    }
}
/* }}} */
/* FUNCTION: connect {{{ */
static int
connect(redisContext **rc, const char* password, const char *host, int port, int db_index, const char *address)
{
    redisReply *reply = NULL;

    /* Connect */
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    *rc = redisConnectWithTimeout(host, port, timeout);

    if(*rc == NULL || (*rc)->err != 0) {
        if(rc) {
            zwarn("error opening database %s:%d/%d (%s)", host, port, db_index, (*rc)->errstr);
            // redisFree(*rc);
            // *rc = NULL;
        } else {
            zwarn("error opening database %s (insufficient memory)", address);
        }
        return 0;
    }

    /* Authenticate */
    if (password) {
        if (!auth(rc, password)) {
            // redisFree(*rc);
            // *rc = NULL;
            return 0;
        }
    }

    /* Select database */
    if (db_index) {
        reply = redisCommand(*rc, "SELECT %d", db_index);
        if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
            if (reply) {
                zwarn("error selecting database #%d (host: %s:%d, message: %s)", db_index, host, port, reply->str);
                freeReplyObject(reply);
            } else {
                zwarn("IO error selecting database #%d (host: %s:%d)", db_index, host, port);
            }
            // redisFree(*rc);
            // *rc = NULL;
            return 0;
        }
        freeReplyObject(reply);
    }

    return 1;
}
/* }}} */
/* FUNCTION: type {{{ */

static int
type(redisContext **rc, int *fdesc, const char *redis_host_port, const char *password, char *key, size_t key_len)
{
    redisReply *reply = NULL;
    int tpe;

    int retry = 0;
 retry:
    if (*rc) {
        reply = redisCommand(*rc, "TYPE %b", key, (size_t) key_len);
        if (reply == NULL || reply->type != REDIS_REPLY_STATUS) {
            if (reply) {
                freeReplyObject(reply);
                reply = NULL;
            }
            if ((*rc)->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
                goto do_retry;
            } else {
                return DB_KEY_TYPE_UNKNOWN;
            }
        }
    }

 do_retry:
    /* Disconnect detection */
    if (!*rc || (*rc)->err & (REDIS_ERR_IO | REDIS_ERR_EOF)) {
        if (retry) {
            zwarn("Aborting (no connection)");
            return DB_KEY_TYPE_UNKNOWN;
        }
        retry = 1;
        if(reconnect(rc, fdesc, redis_host_port, password))
            goto retry;
        else
            return DB_KEY_TYPE_UNKNOWN;
    }

    /* Holds necessary strncmp() invocations */
    tpe = type_from_string(reply->str, reply->len);

    freeReplyObject(reply);

    return tpe;
}
/* }}} */
/* FUNCTION: type_from_string {{{ */

static int
type_from_string(const char *string, int len)
{
    if (0 == strncmp("string", string, 7)) {
        return DB_KEY_TYPE_STRING;
    }
    if (0 == strncmp("list", string, 4)) {
        return DB_KEY_TYPE_LIST;
    }
    if (0 == strncmp("set", string, 3)) {
        return DB_KEY_TYPE_SET;
    }
    if (0 == strncmp("zset", string, 4)) {
        return DB_KEY_TYPE_ZSET;
    }
    if (0 == strncmp("hash", string, 4)) {
        return DB_KEY_TYPE_HASH;
    }
    if (0 == strncmp("none", string, 4)) {
        return DB_KEY_TYPE_NONE;
    }
    return DB_KEY_TYPE_UNKNOWN;
}
/* }}} */
/* FUNCTION: auth {{{ */

static int
auth(redisContext **rc, const char *password)
{
    redisReply *reply = NULL;

    if (!password || password[0] == '\0')
        return 1;

    if (!*rc)
        return 1;

    reply = redisCommand(*rc, "AUTH %b", password, (size_t)strlen(password));
    if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
        if (reply) {
            zwarn("Error when authenticating (%s)", reply->str);
            freeReplyObject(reply);
        } else {
            zwarn("Error when authenticating (no connection?)");
        }
        return 0;
    }
    freeReplyObject(reply);
    return 1;
}
/* }}} */
/* FUNCTION: is_tied {{{ */

static int
is_tied(Param pm)
{
    if (pm->gsu.h == &redis_hash_gsu) {
        return 1;
    } else if (pm->gsu.s->getfn == &redis_str_getfn) {
        return 1;
    } else if (pm->gsu.a->getfn == &redis_arrset_getfn) {
        return 1;
    } else if (pm->gsu.h == &hash_zset_gsu) {
        return 1;
    } else if (pm->gsu.h == &hash_hset_gsu) {
        return 1;
    } else if (pm->gsu.a->getfn == &redis_arrlist_getfn) {
        return 1;
    }

    return 0;
}
/* }}} */
/* FUNCTION: zrzset_usage {{{ */

static void
zrzset_usage()
{
    fprintf(stdout, "Usage: zrzset {tied-param-name}\n");
    fprintf(stdout, "Output: $reply array, to hold elements of the sorted set\n");
    fflush(stdout);
}
/* }}} */
/* FUNCTION: reconnect {{{ */

static int
reconnect(redisContext **rc, int *fdesc, const char *hostspec_in, const char *password)
{
    char hostspec[192];
    char *host="127.0.0.1", *key="";
    int port = 6379, db_index = 0;

    parse_host_string(hostspec_in, hostspec, 192, &host, &port, &db_index, &key);

    if (*rc)
        redisFree(*rc);
    *rc = NULL;

    fdtable[*fdesc] = FDT_UNUSED;

    if(!connect(rc, password, host, port, db_index, hostspec_in)) {
        *rc = NULL;
        zwarn("Not connected, retrying... Failed, aborting");
        return 0;
    } else {
        *fdesc = (*rc)->fd;
        addmodulefd(*fdesc, FDT_INTERNAL);
        zwarn("Not connected, retrying... Success");
        return 1;
    }
}
/* }}} */
/* FUNCTION: get_from_hash {{{ */

char *
get_from_hash(Param pm, const char *key)
{
    HashTable ht = pm->u.hash;
    if (!ht) {
        return NULL;
    }

    /* Get hash element */
    HashNode hn = gethashnode2(ht, key);
    Param val_pm = (Param) hn;

    if (!val_pm) {
        return NULL;
    }

    /* Fill hash element */
    return val_pm->gsu.s->getfn(val_pm);
}
/* }}} */

#else
# error no hiredis library after it was correctly detected (by configure script)
#endif /* have hiredis */
