/* -*- Mode: C; c-basic-offset: 4 -*-
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

/* MACROS {{{ */
#ifndef PM_UPTODATE
#define PM_UPTODATE     (1<<19) /* Parameter has up-to-date data (e.g. loaded from DB) */
#endif

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

#define RD_TYPE_UNKNOWN 0
#define RD_TYPE_STRING 1
#define RD_TYPE_LIST 2
#define RD_TYPE_SET 3
#define RD_TYPE_ZSET 4
#define RD_TYPE_HASH 5
/* }}} */

#if defined(HAVE_HIREDIS_HIREDIS_H) && defined(HAVE_REDISCONNECT)

/* DECLARATIONS {{{ */
static char *type_names[7] = { "invalid", "string", "list", "set", "sorted-set", "hash", "error" };

#include <hiredis/hiredis.h>

static Param createhash(char *name, int flags, int which);
static int append_tied_name(const char *name);
static int remove_tied_name(const char *name);
static char *unmetafy_zalloc(const char *to_copy, int *new_len);
static void set_length(char *buf, int size);
static void parse_host_string(const char *input, char *buffer, int size,
                                char **host, int *port, int *db_index, char **key);
static int connect(char *nam, redisContext **rc, const char *host, int port, int db_index,
                    const char *resource_name_in);
static int type(redisContext *rc, char *key, size_t key_len);
static int is_tied(Param pm);
static void zrtie_usage();

static char *backtype = "db/redis";
static char *my_nullarray = NULL;
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
    struct gsu_scalar std;
    int use_cache;
    redisContext *rc;
    char *redis_host_port;
    char *key;
    size_t key_len;
};

/* Used by sets */
struct gsu_array_ext {
    struct gsu_array std;
    int use_cache;
    redisContext *rc;
    char *redis_host_port;
    char *key;
    size_t key_len;
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
    BUILTIN("zrtie", 0, bin_zrtie, 1, -1, 0, "d:f:rph", NULL),
    BUILTIN("zruntie", 0, bin_zruntie, 1, -1, 0, "u", NULL),
    BUILTIN("zredishost", 0, bin_zredishost, 1, -1, 0, "", NULL),
    BUILTIN("zredisclear", 0, bin_zredisclear, 1, 2, 0, "", NULL),
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

/* FUNCTION: bin_zrtie {{{ */
/**/
static int
bin_zrtie(char *nam, char **args, Options ops, UNUSED(int func))
{
    redisContext *rc = NULL;
    int read_write = 1, pmflags = PM_REMOVABLE;
    Param tied_param;

    /* Check options */

    if (OPT_ISSET(ops,'h')) {
        zrtie_usage();
        return 0;
    }

    if (!OPT_ISSET(ops,'d')) {
        zwarnnam(nam, "you must pass `-d %s', see `-h help'", backtype);
        return 1;
    }
    if (!OPT_ISSET(ops,'f')) {
        zwarnnam(nam, "you must pass `-f' with {host}[:port][/[db_idx][/key]], see `-h help'", NULL);
        return 1;
    }
    if (OPT_ISSET(ops,'r')) {
        read_write = 0;
        pmflags |= PM_READONLY;
    }

    if (strcmp(OPT_ARG(ops, 'd'), backtype) != 0) {
        zwarnnam(nam, "unsupported backend type `%s', see `-h help'", OPT_ARG(ops, 'd'));
        return 1;
    }

    /* Parse host data */

    char *pmname, *resource_name_in, resource_name[192];
    char *host="127.0.0.1", *key="";
    int port = 6379, db_index = 0;

    resource_name_in = OPT_ARG(ops, 'f');
    pmname = *args;

    parse_host_string(resource_name_in, resource_name, 192, &host, &port, &db_index, &key);

    /* Unset existing parameter */

    if ((tied_param = (Param)paramtab->getnode(paramtab, pmname)) && !(tied_param->node.flags & PM_UNSET)) {
        if (is_tied(tied_param)) {
            zwarnnam(nam, "Refusing to re-tie already tied parameter `%s'", pmname);
            zwarnnam(nam, "The involved `unset' could clear the database-part handled by `%s'", pmname);
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

    /* Connect */

    if(!connect(nam, &rc, host, port, db_index, resource_name_in)) {
        return 1;
    }

    /* Main string storage? */
    if (0 == strcmp(key,"")) {
        /* Create hash */
        if (!(tied_param = createhash(pmname, pmflags, 0))) {
            zwarnnam(nam, "cannot create the requested hash parameter: %s", pmname);
            redisFree(rc);
            return 1;
        }

        /* Allocate parameter sub-gsu, fill rc field.
         * rc allocation is 1 to 1 accompanied by
         * gsu_scalar_ext allocation. */

        struct gsu_scalar_ext *rc_carrier = NULL;
        rc_carrier = (struct gsu_scalar_ext *) zalloc(sizeof(struct gsu_scalar_ext));
        rc_carrier->std = hashel_gsu_ext.std;
        rc_carrier->use_cache = 1;
        if (OPT_ISSET(ops,'p'))
            rc_carrier->use_cache = 0;
        rc_carrier->rc = rc;

        /* Fill also host:port// field */
        rc_carrier->redis_host_port = ztrdup(resource_name_in);

        tied_param->u.hash->tmpdata = (void *)rc_carrier;
        tied_param->gsu.h = &redis_hash_gsu;
    } else {
        int tpe = type(rc, key, (size_t) strlen(key));
        if (tpe == RD_TYPE_STRING) {
            if (!(tied_param = createparam(pmname, pmflags | PM_SPECIAL))) {
                zwarnnam(nam, "cannot create the requested scalar parameter: %s", pmname);
                redisFree(rc);
                return 1;
            }
            struct gsu_scalar_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_scalar_ext *) zalloc(sizeof(struct gsu_scalar_ext));
            rc_carrier->std = string_gsu_ext.std;
            rc_carrier->use_cache = 1;
            if (OPT_ISSET(ops,'p'))
                rc_carrier->use_cache = 0;
            rc_carrier->rc = rc;
            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// field */
            rc_carrier->redis_host_port = ztrdup(resource_name_in);

            tied_param->gsu.s = (GsuScalar) rc_carrier;
        } else if (tpe == RD_TYPE_SET) {
            if (!(tied_param = createparam(pmname, pmflags | PM_ARRAY | PM_SPECIAL))) {
                zwarnnam(nam, "cannot create the requested array (for set) parameter: %s", pmname);
                redisFree(rc);
                return 1;
            }
            struct gsu_array_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_array_ext *) zalloc(sizeof(struct gsu_array_ext));
            rc_carrier->std = arrset_gsu_ext.std;
            rc_carrier->use_cache = 1;
            if (OPT_ISSET(ops,'p'))
                rc_carrier->use_cache = 0;
            rc_carrier->rc = rc;
            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// field */
            rc_carrier->redis_host_port = ztrdup(resource_name_in);

            tied_param->gsu.s = (GsuScalar) rc_carrier;
        } else if (tpe == RD_TYPE_ZSET) {
            /* Create hash */
            if (!(tied_param = createhash(pmname, pmflags, 1))) {
                zwarnnam(nam, "cannot create the requested hash (for zset) parameter: %s", pmname);
                redisFree(rc);
                return 1;
            }

            struct gsu_scalar_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_scalar_ext *) zalloc(sizeof(struct gsu_scalar_ext));
            rc_carrier->std = hashel_zset_gsu_ext.std;
            rc_carrier->use_cache = 1;
            if (OPT_ISSET(ops,'p'))
                rc_carrier->use_cache = 0;
            rc_carrier->rc = rc;
            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// field */
            rc_carrier->redis_host_port = ztrdup(resource_name_in);

            tied_param->u.hash->tmpdata = (void *)rc_carrier;
            tied_param->gsu.h = &hash_zset_gsu;
        } else if (tpe == RD_TYPE_HASH) {
            /* Create hash */
            if (!(tied_param = createhash(pmname, pmflags, 2))) {
                zwarnnam(nam, "cannot create the requested hash (for hset) parameter: %s", pmname);
                redisFree(rc);
                return 1;
            }

            struct gsu_scalar_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_scalar_ext *) zalloc(sizeof(struct gsu_scalar_ext));
            rc_carrier->std = hashel_hset_gsu_ext.std;
            rc_carrier->use_cache = 1;
            if (OPT_ISSET(ops,'p'))
                rc_carrier->use_cache = 0;
            rc_carrier->rc = rc;
            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// field */
            rc_carrier->redis_host_port = ztrdup(resource_name_in);

            tied_param->u.hash->tmpdata = (void *)rc_carrier;
            tied_param->gsu.h = &hash_hset_gsu;
        } else if (tpe == RD_TYPE_LIST) {
            if (!(tied_param = createparam(pmname, pmflags | PM_ARRAY | PM_SPECIAL))) {
                zwarnnam(nam, "cannot create the requested array (for list) parameter: %s", pmname);
                redisFree(rc);
                return 1;
            }
            struct gsu_array_ext *rc_carrier = NULL;
            rc_carrier = (struct gsu_array_ext *) zalloc(sizeof(struct gsu_array_ext));
            rc_carrier->std = arrlist_gsu_ext.std;
            rc_carrier->use_cache = 1;
            if (OPT_ISSET(ops,'p'))
                rc_carrier->use_cache = 0;
            rc_carrier->rc = rc;
            rc_carrier->key = ztrdup(key);
            rc_carrier->key_len = strlen(key);

            /* Fill also host:port// field */
            rc_carrier->redis_host_port = ztrdup(resource_name_in);

            tied_param->gsu.s = (GsuScalar) rc_carrier;
        } else {
            redisFree(rc);
            zwarnnam(nam, "Unknown key type: %s", type_names[tpe]);
            return 1;
        }
    }

    /* Save in tied-enumeration array */
    append_tied_name(pmname);

    return 0;
}
/* }}} */
/* FUNCTION: bin_zruntie {{{ */
/**/
static int
bin_zruntie(char *nam, char **args, Options ops, UNUSED(int func))
{
    Param pm;
    char *pmname;
    int ret = 0;

    for (pmname = *args; *args++; pmname = *args) {
        /* Get param */
        pm = (Param) paramtab->getnode(paramtab, pmname);
        if(!pm) {
            zwarnnam(nam, "cannot untie `%s', parameter not found", pmname);
            ret = 1;
            continue;
        }

        if (pm->gsu.h == &redis_hash_gsu) {
            queue_signals();
            if (OPT_ISSET(ops,'u'))
                pm->node.flags &= ~PM_READONLY;
            if (unsetparam_pm(pm, 0, 1)) {
                /* assume already reported */
                ret = 1;
            }
            unqueue_signals();
        } else if (pm->gsu.s->getfn == &redis_str_getfn) {
            if (pm->node.flags & PM_READONLY && !OPT_ISSET(ops,'u')) {
                zwarnnam(nam, "cannot untie `%s', parameter is read only, use -u option", pmname);
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
            if (pm->node.flags & PM_READONLY && !OPT_ISSET(ops,'u')) {
                zwarnnam(nam, "cannot untie `%s', parameter is read only, use -u option", pmname);
                continue;
            }
            pm->node.flags &= ~PM_READONLY;
            queue_signals();
            /* Detach from database, untie doesn't clear the database */
            redis_arrset_untie(pm);

            if (unsetparam_pm(pm, 0, 1)) {
                /* assume already reported */
                ret = 1;
            }
            unqueue_signals();
        } else if (pm->gsu.h == &hash_zset_gsu) {
            queue_signals();
            if (OPT_ISSET(ops,'u'))
                pm->node.flags &= ~PM_READONLY;
            if (unsetparam_pm(pm, 0, 1)) {
                /* assume already reported */
                ret = 1;
            }
            unqueue_signals();
        } else if (pm->gsu.h == &hash_hset_gsu) {
            queue_signals();
            if (OPT_ISSET(ops,'u'))
                pm->node.flags &= ~PM_READONLY;
            if (unsetparam_pm(pm, 0, 1)) {
                /* assume already reported */
                ret = 1;
            }
            unqueue_signals();
        } else {
            zwarnnam(nam, "not a tied redis parameter: `%s'", pmname);
            ret = 1;
            continue;
        }
    }

    return ret;
}
/* }}} */
/* FUNCTION: bin_zredishost {{{ */
/**/
static int
bin_zredishost(char *nam, char **args, Options ops, UNUSED(int func))
{
    Param pm;
    char *pmname;

    pmname = *args;

    if (!pmname) {
        zwarnnam(nam, "parameter name (whose path is to be written to $REPLY) is required");
        return 1;
    }

    pm = (Param) paramtab->getnode(paramtab, pmname);
    if(!pm) {
        zwarnnam(nam, "no such parameter: %s", pmname);
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
        zwarnnam(nam, "not a tied zredis parameter: `%s', REPLY unchanged", pmname);
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
/* FUNCTION: bin_zredisclear {{{ */
/**/
static int
bin_zredisclear(char *nam, char **args, Options ops, UNUSED(int func))
{
    Param pm;
    char *pmname, *key;

    pmname = *args++;
    key = *args;

    if (!pmname) {
        zwarnnam(nam, "parameter name (whose path is to be written to $REPLY) is required");
        return 1;
    }

    pm = (Param) paramtab->getnode(paramtab, pmname);
    if (!pm) {
        zwarnnam(nam, "no such parameter: %s", pmname);
        return 1;
    }

    if (pm->gsu.h == &redis_hash_gsu) {
        if (!key) {
            zwarnnam(nam, "Key name, which is to be cache-cleared in hash `%s', is required", pmname);
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
            zwarnnam(nam, "Ignored argument `%s'", key);
    } else if (pm->gsu.a->getfn == &redis_arrset_getfn) {
        pm->node.flags &= ~(PM_UPTODATE);
        if (key)
            zwarnnam(nam, "Ignored argument `%s'", key);
    } else if(pm->gsu.h == &hash_zset_gsu) {
        if (!key) {
            zwarnnam(nam, "Key name, which is to be cache-cleared in hash/zset `%s', is required", pmname);
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
            zwarnnam(nam, "Key name, which is to be cache-cleared in hash/hset `%s', is required", pmname);
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
            zwarnnam(nam, "Ignored argument `%s'", key);
    } else {
        zwarnnam(nam, "not a tied zredis parameter: %s", pmname);
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
 * contain such key. That might be a drawback, maybe
 * setting to empty value has sense. This would remove
 * subtle hcalloc(1) leak.
 */

/**/
static char *
redis_getfn(Param pm)
{
    struct gsu_scalar_ext *gsu_ext;
    char *key;
    size_t key_len;
    redisContext *rc;
    redisReply *reply;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : (char *) hcalloc(1);
    }

    /* Unmetafy key. Redis fits nice into this
     * process, as it can use length of data */
    int umlen = 0;
    char *umkey = unmetafy_zalloc(pm->node.nam, &umlen);

    key = umkey;
    key_len = umlen;

    rc = gsu_ext->rc;

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
            }

            /* Metafy returned data. All fits - metafy
             * can obtain data length to avoid using \0 */
            pm->u.str = metafy(reply->str, reply->len, META_DUP);
            freeReplyObject(reply);

            /* Free key, restoring its original length */
            set_length(umkey, key_len);
            zsfree(umkey);

            /* Can return pointer, correctly saved inside hash */
            return pm->u.str;
        } else if (reply) {
            freeReplyObject(reply);
        }
    } else if (reply) {
        freeReplyObject(reply);
    }

    /* Free key, restoring its original length */
    set_length(umkey, key_len);
    zsfree(umkey);

    /* Can this be "" ? */
    return (char *) hcalloc(1);
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
    redisReply *reply;

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
    rc = ((struct gsu_scalar_ext *)pm->gsu.s)->rc;
    if (rc) {
        int umlen = 0;
        char *umkey = unmetafy_zalloc(pm->node.nam, &umlen);

        key = umkey;
        key_len = umlen;

        if (val) {
            /* Unmetafy with exact zalloc size */
            char *umval = unmetafy_zalloc(val, &umlen);

            /* Store */
            content = umval;
            content_len = umlen;
            reply = redisCommand(rc, "SET %b %b", key, (size_t) key_len, content, (size_t) content_len);
            if (reply)
                freeReplyObject(reply);

            /* Free */
            set_length(umval, content_len);
            zsfree(umval);
        } else {
            reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
            if (reply)
                freeReplyObject(reply);
        }

        /* Free key */
        set_length(umkey, key_len);
        zsfree(umkey);
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
    size_t key_len;
    redisContext *rc;
    redisReply *reply;

    rc = ((struct gsu_scalar_ext *)ht->tmpdata)->rc;

    /* Iterate keys adding them to hash, so
     * we have Param to use in `func` */
    reply = redisCommand(rc, "KEYS *");
    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY) {
        if (reply)
            freeReplyObject(reply);
        return;
    }

    for (size_t j = 0; j < reply->elements; j++) {
        redisReply *entry = reply->element[j];
        if (entry == NULL || entry->type != REDIS_REPLY_STRING) {
            continue;
        }

        key = entry->str;
        key_len = entry->len;

        /* Only scan string keys, ignore the rest (hashes, sets, etc.) */
        if (RD_TYPE_STRING != type(rc, key, (size_t) key_len)) {
            continue;
        }

        /* This returns database-interfacing Param,
         * it will return u.str or first fetch data
         * if not PM_UPTODATE (newly created) */
        char *zkey = metafy(key, key_len, META_DUP);
        HashNode hn = redis_get_node(ht, zkey);
        zsfree(zkey);

        func(hn, flags);
    }

    freeReplyObject(reply);
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
    redisReply *reply, *entry, *reply2;

    if (!pm->u.hash || pm->u.hash == ht)
        return;

    if (!(rc = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->rc))
        return;

    /* KEYS */
    reply = redisCommand(rc, "KEYS *");
    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY) {
        if (reply)
            freeReplyObject(reply);
        return;
    }

    for (j = 0; j < reply->elements; j++) {
        entry = reply->element[j];
        if (entry == NULL || entry->type != REDIS_REPLY_STRING) {
            continue;
        }
        key = entry->str;
        key_len = entry->len;

        /* Only scan string keys, ignore the rest (hashes, sets, etc.) */
        if (RD_TYPE_STRING != type(rc, key, (size_t) key_len)) {
            continue;
        }

        queue_signals();

        /* DEL */
        reply2 = redisCommand(rc, "DEL %b", key, (size_t) key_len);
        if (reply2)
            freeReplyObject(reply2);

        unqueue_signals();
    }
    freeReplyObject(reply);

    emptyhashtable(pm->u.hash);

    if (!ht)
        return;

     /* Put new strings into database, having
      * their interfacing-Params created */

    for (i = 0; i < ht->hsize; i++)
        for (hn = ht->nodes[i]; hn; hn = hn->next) {
            struct value v;

            v.isarr = v.flags = v.start = 0;
            v.end = -1;
            v.arr = NULL;
            v.pm = (Param) hn;

            /* Unmetafy key */
            int umlen = 0;
            char *umkey = unmetafy_zalloc(v.pm->node.nam, &umlen);

            key = umkey;
            key_len = umlen;

            queue_signals();

            /* Unmetafy data */
            char *umval = unmetafy_zalloc(getstrvalue(&v), &umlen);

            content = umval;
            content_len = umlen;

            /* SET */
            reply = redisCommand(rc, "SET %b %b", key, (size_t) key_len, content, (size_t) content_len);
            if (reply)
                freeReplyObject(reply);

            /* Free, restoring original length */
            set_length(umval, content_len);
            zsfree(umval);
            set_length(umkey, key_len);
            zsfree(umkey);

            unqueue_signals();
        }
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

    /* Uses normal unsetter. Will delete all owned
     * parameters and also hashtable. */
    pm->gsu.h->setfn(pm, NULL);

    /* Don't need custom GSU structure with its
     * redisContext pointer anymore */
    zsfree(gsu_ext->redis_host_port);
    zfree(gsu_ext, sizeof(struct gsu_scalar_ext));

    pm->node.flags |= PM_UNSET;
}
/* }}} */
/* FUNCTION: redis_hash_untie {{{ */
/**/
static void
redis_hash_untie(Param pm)
{
    redisContext *rc = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->rc;
    HashTable ht = pm->u.hash;

    if (rc) { /* paranoia */
        redisFree(rc);

        /* Let hash fields know there's no backend */
        ((struct gsu_scalar_ext *)ht->tmpdata)->rc = NULL;

        /* Remove from list of tied parameters */
        remove_tied_name(pm->node.nam);
    }

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
    redisReply *reply;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;
    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : (char *) hcalloc(1);
    }

    rc = gsu_ext->rc;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

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

    /* Can this be "" ? */
    return (char *) hcalloc(1);
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
    redisReply *reply;

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
    struct gsu_scalar_ext *gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;
    rc = gsu_ext->rc;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    if (rc) {
        if (val) {
            /* Unmetafy with exact zalloc size */
            int umlen = 0;
            char *umval = unmetafy_zalloc(val, &umlen);

            /* Store */
            content = umval;
            content_len = umlen;
            reply = redisCommand(rc, "SET %b %b", key, (size_t) key_len, content, (size_t) content_len);
            if (reply)
                freeReplyObject(reply);

            /* Free */
            set_length(umval, content_len);
            zsfree(umval);
        } else {
            reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
            if (reply)
                freeReplyObject(reply);
        }
    }
}
/* }}} */
/* FUNCTION: redis_str_unsetfn {{{ */
/**/
static void
redis_str_unsetfn(Param pm, UNUSED(int um))
{
    /* Will clear the database */
    redis_str_setfn(pm, NULL);

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

    if (gsu_ext->rc) /* paranoia */
        redisFree(gsu_ext->rc);

    /* Remove from list of tied parameters */
    remove_tied_name(pm->node.nam);

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.s = &stdscalar_gsu;

    /* Free gsu_ext */
    zsfree(gsu_ext->redis_host_port);
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
    redisReply *reply;
    int j;

    gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.arr ? pm->u.arr : &my_nullarray;
    }

    rc = gsu_ext->rc;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    reply = redisCommand(rc, "EXISTS %b", key, (size_t) key_len);
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
        freeReplyObject(reply);

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

            /* Can return pointer, correctly saved inside Param */
            return pm->u.arr;
        } else if (reply) {
            freeReplyObject(reply);
        }
    } else if (reply) {
        freeReplyObject(reply);
    }

    /* Array with 0 elements */
    return &my_nullarray;
}
/* }}} */
/* FUNCTION: redis_arrset_setfn {{{ */
/**/
mod_export void
redis_arrset_setfn(Param pm, char **val)
{
    char *key, *content;
    size_t key_len, content_len;
    int alen = 0, j;
    redisContext *rc;
    redisReply *reply;

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
    struct gsu_array_ext *gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    rc = gsu_ext->rc;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    if (rc) {
        reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
        if (reply)
            freeReplyObject(reply);

        if (val) {
            for (j=0; j<alen; j ++) {
                /* Unmetafy with exact zalloc size */
                int umlen = 0;
                char *umval = unmetafy_zalloc(val[j], &umlen);

                /* Store */
                content = umval;
                content_len = umlen;
                reply = redisCommand(rc, "SADD %b %b", key, (size_t) key_len, content, (size_t) content_len);
                if (reply)
                    freeReplyObject(reply);

                /* Free */
                set_length(umval, umlen);
                zsfree(umval);
            }
        }
    }

    if (pm->ename && val)
        arrfixenv(pm->ename, val);
}
/* }}} */
/* FUNCTION: redis_arrset_unsetfn {{{ */
/**/
mod_export void
redis_arrset_unsetfn(Param pm, UNUSED(int exp))
{
    /* Will clear the database */
    redis_arrset_setfn(pm, NULL);

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

    if (gsu_ext->rc) /* paranoia */
        redisFree(gsu_ext->rc);

    /* Remove from list of tied parameters */
    remove_tied_name(pm->node.nam);

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.s = &stdscalar_gsu;

    /* Free gsu_ext */
    zsfree(gsu_ext->redis_host_port);
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
    char *main_key, *key;
    size_t main_key_len, key_len;
    redisContext *rc;
    redisReply *reply;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : (char *) hcalloc(1);
    }

    /* Unmetafy key. Redis fits nice into this
     * process, as it can use length of data */
    int umlen = 0;
    char *umkey = unmetafy_zalloc(pm->node.nam, &umlen);

    key = umkey;
    key_len = umlen;

    rc = gsu_ext->rc;
    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    reply = redisCommand(rc, "ZSCORE %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
    if (reply && reply->type == REDIS_REPLY_STRING) {
        /* We have data – store it and return it */
        pm->node.flags |= PM_UPTODATE;

        /* Ensure there's no leak */
        if (pm->u.str) {
            zsfree(pm->u.str);
        }

        /* Metafy returned data. All fits - metafy
          * can obtain data length to avoid using \0 */
        pm->u.str = metafy(reply->str, reply->len, META_DUP);
        freeReplyObject(reply);

        /* Free key, restoring its original length */
        set_length(umkey, key_len);
        zsfree(umkey);

        /* Can return pointer, correctly saved inside hash */
        return pm->u.str;
    } else if (reply) {
        freeReplyObject(reply);
    }

    /* Free key, restoring its original length */
    set_length(umkey, key_len);
    zsfree(umkey);

    /* Can this be "" ? */
    return (char *) hcalloc(1);
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
    redisReply *reply;

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
    rc = gsu_ext->rc;

    if (rc) {
        int umlen = 0;
        char *umkey = unmetafy_zalloc(pm->node.nam, &umlen);

        key = umkey;
        key_len = umlen;

        main_key = gsu_ext->key;
        main_key_len = gsu_ext->key_len;

        if (val) {
            /* Unmetafy with exact zalloc size */
            char *umval = unmetafy_zalloc(val, &umlen);

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
            set_length(umval, content_len);
            zsfree(umval);
        } else {
            reply = redisCommand(rc, "ZREM %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
            if (reply)
                freeReplyObject(reply);
        }

        /* Free key */
        set_length(umkey, key_len);
        zsfree(umkey);
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
    size_t main_key_len, key_len;
    unsigned long long cursor = 0;
    redisContext *rc;
    redisReply *reply, *reply2;
    struct gsu_scalar_ext *gsu_ext;

    gsu_ext = (struct gsu_scalar_ext *) ht->tmpdata;
    rc = gsu_ext->rc;
    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* Iterate keys adding them to hash, so we have Param to use in `func` */
    do {
      reply = redisCommand(rc, "ZSCAN %b %llu", main_key, (size_t) main_key_len, cursor);
      if (reply == NULL || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
          if (reply && reply->type == REDIS_REPLY_ERROR) {
              zwarn("Problem occured during ZSCAN: %s", reply->str);
          } else {
              zwarn("Problem occured during ZSCAN, no error message available");
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

      for (size_t j = 0; j < reply2->elements; j+= 2) {
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
    redisReply *reply;
    struct gsu_scalar_ext *gsu_ext;

    if (!pm->u.hash || pm->u.hash == ht)
        return;

    gsu_ext = (struct gsu_scalar_ext *) pm->u.hash->tmpdata;
    rc = gsu_ext->rc;
    if (!rc) {
        /* TODO: RECONNECT */
        return;
    }

    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* PRUNE */
    reply = redisCommand(rc, "ZREMRANGEBYSCORE %b -inf +inf", main_key, (size_t) main_key_len);
    if (reply == NULL || reply->type != REDIS_REPLY_INTEGER) {
        zwarn("Error 4 occured, database not updated");
        if (reply)
            freeReplyObject(reply);
        return;
    }
    freeReplyObject(reply);

    emptyhashtable(pm->u.hash);

    if (!ht)
        return;

     /* Put new strings into database, having
      * their interfacing-Params created */

    for (i = 0; i < ht->hsize; i++)
        for (hn = ht->nodes[i]; hn; hn = hn->next) {
            struct value v;

            v.isarr = v.flags = v.start = 0;
            v.end = -1;
            v.arr = NULL;
            v.pm = (Param) hn;

            /* Unmetafy key */
            int umlen = 0;
            char *umkey = unmetafy_zalloc(v.pm->node.nam, &umlen);

            key = umkey;
            key_len = umlen;

            queue_signals();

            /* Unmetafy data */
            char *umval = unmetafy_zalloc(getstrvalue(&v), &umlen);

            content = umval;
            content_len = umlen;

            /* ZADD myzset 1.0 element1 */
            reply = redisCommand(rc, "ZADD %b %b %b", main_key, (size_t) main_key_len,
                                 content, (size_t) content_len,
                                 key, (size_t) key_len);
            if (reply)
                freeReplyObject(reply);

            /* Free, restoring original length */
            set_length(umval, content_len);
            zsfree(umval);
            set_length(umkey, key_len);
            zsfree(umkey);

            unqueue_signals();
        }
}
/* }}} */
/* FUNCTION: redis_hash_zset_unsetfn {{{ */
/**/
static void
redis_hash_zset_unsetfn(Param pm, UNUSED(int exp))
{
    /* This will make database contents survive the
     * unset, as standard GSU will be put in place */
    redis_hash_zset_untie(pm);

    /* Remember custom GSU structure assigned to
     * u.hash->tmpdata before hash gets deleted */
    struct gsu_scalar_ext * gsu_ext = pm->u.hash->tmpdata;

    /* Uses normal unsetter. Will delete all owned
     * parameters and also hashtable. */
    pm->gsu.h->setfn(pm, NULL);

    /* Don't need custom GSU structure with its
     * redisContext pointer anymore */
    zsfree(gsu_ext->redis_host_port);
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
    redisContext *rc = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->rc;
    HashTable ht = pm->u.hash;

    if (rc) { /* paranoia */
        redisFree(rc);

        /* Let hash fields know there's no backend */
        ((struct gsu_scalar_ext *)ht->tmpdata)->rc = NULL;

        /* Remove from list of tied parameters */
        remove_tied_name(pm->node.nam);
    }

    /* for completeness ... createspecialhash() should have an inverse */
    ht->getnode = ht->getnode2 = gethashnode2;
    ht->scantab = NULL;

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.h = &stdhash_gsu;
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
    char *main_key, *key;
    size_t main_key_len, key_len;
    redisContext *rc;
    redisReply *reply;

    gsu_ext = (struct gsu_scalar_ext *) pm->gsu.s;

    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.str ? pm->u.str : (char *) hcalloc(1);
    }

    /* Unmetafy key. Redis fits nice into this
     * process, as it can use length of data */
    int umlen = 0;
    char *umkey = unmetafy_zalloc(pm->node.nam, &umlen);

    key = umkey;
    key_len = umlen;

    rc = gsu_ext->rc;
    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    reply = redisCommand(rc, "HGET %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
    if (reply && reply->type == REDIS_REPLY_STRING) {
        /* We have data – store it and return it */
        pm->node.flags |= PM_UPTODATE;

        /* Ensure there's no leak */
        if (pm->u.str) {
            zsfree(pm->u.str);
        }

        /* Metafy returned data. All fits - metafy
          * can obtain data length to avoid using \0 */
        pm->u.str = metafy(reply->str, reply->len, META_DUP);
        freeReplyObject(reply);

        /* Free key, restoring its original length */
        set_length(umkey, key_len);
        zsfree(umkey);

        /* Can return pointer, correctly saved inside hash */
        return pm->u.str;
    } else if (reply) {
        freeReplyObject(reply);
    }

    /* Free key, restoring its original length */
    set_length(umkey, key_len);
    zsfree(umkey);

    /* Can this be "" ? */
    return (char *) hcalloc(1);
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
    redisReply *reply;

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
    rc = gsu_ext->rc;

    if (rc) {
        int umlen = 0;
        char *umkey = unmetafy_zalloc(pm->node.nam, &umlen);

        key = umkey;
        key_len = umlen;

        main_key = gsu_ext->key;
        main_key_len = gsu_ext->key_len;

        if (val) {
            /* Unmetafy with exact zalloc size */
            char *umval = unmetafy_zalloc(val, &umlen);

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
            set_length(umval, content_len);
            zsfree(umval);
        } else {
            reply = redisCommand(rc, "HDEL %b %b", main_key, (size_t) main_key_len, key, (size_t) key_len);
            if (reply)
                freeReplyObject(reply);
        }

        /* Free key */
        set_length(umkey, key_len);
        zsfree(umkey);
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
    size_t main_key_len, key_len;
    unsigned long long cursor = 0;
    redisContext *rc;
    redisReply *reply, *reply2;
    struct gsu_scalar_ext *gsu_ext;

    gsu_ext = (struct gsu_scalar_ext *) ht->tmpdata;
    rc = gsu_ext->rc;
    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* Iterate keys adding them to hash, so we have Param to use in `func` */
    do {
      reply = redisCommand(rc, "HSCAN %b %llu", main_key, (size_t) main_key_len, cursor);
      if (reply == NULL || reply->type != REDIS_REPLY_ARRAY || reply->elements != 2) {
          if (reply && reply->type == REDIS_REPLY_ERROR) {
              zwarn("Problem occured during HSCAN: %s", reply->str);
          } else {
              zwarn("Problem occured during HSCAN, no error message available");
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

      for (size_t j = 0; j < reply2->elements; j+= 2) {
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
    redisReply *reply, *reply2, *entry;
    struct gsu_scalar_ext *gsu_ext;

    if (!pm->u.hash || pm->u.hash == ht)
        return;

    gsu_ext = (struct gsu_scalar_ext *) pm->u.hash->tmpdata;
    rc = gsu_ext->rc;
    if (!rc) {
        /* TODO: RECONNECT */
        return;
    }

    main_key = gsu_ext->key;
    main_key_len = gsu_ext->key_len;

    /* PRUNE */
    reply = redisCommand(rc, "HKEYS %b", main_key, (size_t) main_key_len);
    if (reply == NULL || reply->type != REDIS_REPLY_ARRAY) {
        zwarn("Error 5 occured (redis communication), database and tied hash not updated");
        if (reply)
            freeReplyObject(reply);
        return;
    }

    for (i = 0; i < reply->elements; i++) {
        entry = reply->element[i];
        if (NULL == entry || entry->type != REDIS_REPLY_STRING) {
            if (entry && entry->str && entry->len > 0) {
                zwarn("Error 6 when replacing database (element %d, message: %s)", i+1, reply2->str);
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
    }

    freeReplyObject(reply);

    emptyhashtable(pm->u.hash);

    if (!ht)
        return;

     /* Put new strings into database, having
      * their interfacing-Params created */

    for (i = 0; i < ht->hsize; i++)
        for (hn = ht->nodes[i]; hn; hn = hn->next) {
            struct value v;

            v.isarr = v.flags = v.start = 0;
            v.end = -1;
            v.arr = NULL;
            v.pm = (Param) hn;

            /* Unmetafy key */
            int umlen = 0;
            char *umkey = unmetafy_zalloc(v.pm->node.nam, &umlen);

            key = umkey;
            key_len = umlen;

            queue_signals();

            /* Unmetafy data */
            char *umval = unmetafy_zalloc(getstrvalue(&v), &umlen);

            content = umval;
            content_len = umlen;

            /* HSET myhset element1 value */
            reply = redisCommand(rc, "HSET %b %b %b", main_key, (size_t) main_key_len,
                                 key, (size_t) key_len,
                                 content, (size_t) content_len);
            if (reply)
                freeReplyObject(reply);

            /* Free, restoring original length */
            set_length(umval, content_len);
            zsfree(umval);
            set_length(umkey, key_len);
            zsfree(umkey);

            unqueue_signals();
        }
}
/* }}} */
/* FUNCTION: redis_hash_hset_unsetfn {{{ */
/**/
static void
redis_hash_hset_unsetfn(Param pm, UNUSED(int exp))
{
    /* This will make database contents survive the
     * unset, as standard GSU will be put in place */
    redis_hash_hset_untie(pm);

    /* Remember custom GSU structure assigned to
     * u.hash->tmpdata before hash gets deleted */
    struct gsu_scalar_ext * gsu_ext = pm->u.hash->tmpdata;

    /* Uses normal unsetter. Will delete all owned
     * parameters and also hashtable. */
    pm->gsu.h->setfn(pm, NULL);

    /* Don't need custom GSU structure with its
     * redisContext pointer anymore */
    zsfree(gsu_ext->redis_host_port);
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
    redisContext *rc = ((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->rc;
    HashTable ht = pm->u.hash;

    if (rc) { /* paranoia */
        redisFree(rc);

        /* Let hash fields know there's no backend */
        ((struct gsu_scalar_ext *)ht->tmpdata)->rc = NULL;

        /* Remove from list of tied parameters */
        remove_tied_name(pm->node.nam);
    }

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
    redisReply *reply;
    int j;

    gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    /* Key already retrieved? */
    if ((pm->node.flags & PM_UPTODATE) && gsu_ext->use_cache) {
        return pm->u.arr ? pm->u.arr : &my_nullarray;
    }

    rc = gsu_ext->rc;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    reply = redisCommand(rc, "EXISTS %b", key, (size_t) key_len);
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
        freeReplyObject(reply);

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

            /* Can return pointer, correctly saved inside Param */
            return pm->u.arr;
        } else if (reply) {
            freeReplyObject(reply);
        }
    } else if (reply) {
        freeReplyObject(reply);
    }

    /* Array with 0 elements */
    return &my_nullarray;
}
/* }}} */
/* FUNCTION: redis_arrlist_setfn {{{ */
/**/
mod_export void
redis_arrlist_setfn(Param pm, char **val)
{
    char *key, *content;
    size_t key_len, content_len;
    int alen = 0, j;
    redisContext *rc;
    redisReply *reply;

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
    struct gsu_array_ext *gsu_ext = (struct gsu_array_ext *) pm->gsu.a;
    rc = gsu_ext->rc;
    key = gsu_ext->key;
    key_len = gsu_ext->key_len;

    if (rc) {
        reply = redisCommand(rc, "DEL %b", key, (size_t) key_len);
        if (reply)
            freeReplyObject(reply);

        if (val) {
            for (j=0; j<alen; j ++) {
                /* Unmetafy with exact zalloc size */
                int umlen = 0;
                char *umval = unmetafy_zalloc(val[j], &umlen);

                /* Store */
                content = umval;
                content_len = umlen;
                reply = redisCommand(rc, "RPUSH %b %b", key, (size_t) key_len, content, (size_t) content_len);
                if (reply)
                    freeReplyObject(reply);

                /* Free */
                set_length(umval, umlen);
                zsfree(umval);
            }
        }
    }

    if (pm->ename && val)
        arrfixenv(pm->ename, val);
}
/* }}} */
/* FUNCTION: redis_arrlist_unsetfn {{{ */
/**/
mod_export void
redis_arrlist_unsetfn(Param pm, UNUSED(int exp))
{
    /* Will clear the database */
    redis_arrlist_setfn(pm, NULL);

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

    if (gsu_ext->rc) /* paranoia */
        redisFree(gsu_ext->rc);

    /* Remove from list of tied parameters */
    remove_tied_name(pm->node.nam);

    pm->node.flags &= ~(PM_SPECIAL|PM_READONLY);
    pm->gsu.s = &stdscalar_gsu;

    /* Free gsu_ext */
    zsfree(gsu_ext->redis_host_port);
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
static Param createhash(char *name, int flags, int which)
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
    ht = pm->u.hash = newparamtable(32, name);
    if (!pm->u.hash) {
        paramtab->removenode(paramtab, name);
        paramtab->freenode(&pm->node);
        zwarnnam(name, "Out of memory when allocating hash");
        return NULL;
    }

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
/* FUNCTION: append_tied_name {{{ */

/*
 * Adds parameter name to `zredis_tied`
 */

static int append_tied_name(const char *name)
{
    int old_len = arrlen(zredis_tied);
    char **new_zredis_tied = zshcalloc((old_len+2) * sizeof(char *));

    /* Copy */
    char **p = zredis_tied;
    char **dst = new_zredis_tied;
    while (*p) {
        *dst++ = *p++;
    }

    /* Append new one */
    *dst = ztrdup(name);

    /* Substitute, free old one */
    zfree(zredis_tied, sizeof(char *) * (old_len + 1));
    zredis_tied = new_zredis_tied;

    return 0;
}
/* }}} */
/* FUNCTION: remove_tied_name {{{ */

/*
 * Removes parameter name from `zredis_tied`
 */

static int remove_tied_name(const char *name)
{
    int old_len = arrlen(zredis_tied);

    /* Two stage, to always have arrlen() == zfree-size - 1.
     * Could do allocation and revert when `not found`, but
     * what would be better about that. */

    /* Find one to remove */
    char **p = zredis_tied;
    while (*p) {
        if (0==strcmp(name,*p)) {
            break;
        }
        p++;
    }

    /* Copy x+1 to x */
    while (*p) {
        *p=*(p+1);
        p++;
    }

    /* Second stage. Size changed? Only old_size-1
     * change is possible, but.. paranoia way */
    int new_len = arrlen(zredis_tied);
    if (new_len != old_len) {
        char **new_zredis_tied = zshcalloc((new_len+1) * sizeof(char *));

        /* Copy */
        p = zredis_tied;
        char **dst = new_zredis_tied;
        while (*p) {
            *dst++ = *p++;
        }
        *dst = NULL;

        /* Substitute, free old one */
        zfree(zredis_tied, sizeof(char *) * (old_len + 1));
        zredis_tied = new_zredis_tied;
    }

    return 0;
}
/* }}} */
/* FUNCTION: unmetafy_zalloc {{{ */

/*
 * Unmetafy that:
 * - duplicates bufer to work on it,
 * - does zalloc of exact size for the new string,
 * - restores work buffer to original content, to restore strlen
 *
 * No zsfree()-confusing string will be produced.
 */
static char *unmetafy_zalloc(const char *to_copy, int *new_len)
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
/*
 * For zsh-allocator, rest of Zsh seems to use
 * free() instead of zsfree(), and such length
 * restoration causes slowdown, but all is this
 * way strict - correct */
static void set_length(char *buf, int size) {
    buf[size]='\0';
    while (-- size >= 0) {
        buf[size]=' ';
    }
}
/* }}} */
/* FUNCTION: parse_host_string {{{ */
static void parse_host_string(const char *input, char *resource_name, int size,
                                char **host, int *port, int *db_index, char **key)
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
            if (processed[1] != '\0')
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
static int connect(char *nam, redisContext **rc, const char *host, int port,
                   int db_index, const char *resource_name_in)
{
    redisReply *reply;

    /* Connect */
    struct timeval timeout = { 1, 500000 }; // 1.5 seconds
    *rc = redisConnectWithTimeout(host, port, timeout);

    if(*rc == NULL || (*rc)->err != 0) {
        if(rc) {
            zwarnnam(nam, "error opening database %s:%d/%d (%s)", host, port, db_index, (*rc)->errstr);
            redisFree(*rc);
            *rc = NULL;
        } else {
            zwarnnam(nam, "error opening database %s (insufficient memory)", resource_name_in);
        }
        return 0;
    }

    /* Select database */
    if (db_index) {
        reply = redisCommand(*rc, "SELECT %d", db_index);
        if (reply == NULL || reply->type == REDIS_REPLY_ERROR) {
            if (reply) {
                zwarnnam(nam, "error selecting database #%d (host: %s:%d, message: %s)", db_index, host, port, reply->str);
                freeReplyObject(reply);
            } else {
                zwarnnam(nam, "IO error selecting database #%d (host: %s:%d)", db_index, host, port);
            }
            redisFree(*rc);
            *rc = NULL;
            return 0;
        }
        freeReplyObject(reply);
    }

    return 1;
}
/* }}} */
/* FUNCTION: type {{{ */
static int type(redisContext *rc, char *key, size_t key_len) {
    redisReply *reply = NULL;
    reply = redisCommand(rc, "TYPE %b", key, (size_t) key_len);
    if (reply == NULL || reply->type != REDIS_REPLY_STATUS) {
        if (reply)
            freeReplyObject(reply);
        return RD_TYPE_UNKNOWN;
    }
    if (0 == strncmp("string", reply->str, reply->len)) {
        freeReplyObject(reply);
        return RD_TYPE_STRING;
    }
    if (0 == strncmp("list", reply->str, reply->len)) {
        freeReplyObject(reply);
        return RD_TYPE_LIST;
    }
    if (0 == strncmp("set", reply->str, reply->len)) {
        freeReplyObject(reply);
        return RD_TYPE_SET;
    }
    if (0 == strncmp("zset", reply->str, reply->len)) {
        freeReplyObject(reply);
        return RD_TYPE_ZSET;
    }
    if (0 == strncmp("hash", reply->str, reply->len)) {
        freeReplyObject(reply);
        return RD_TYPE_HASH;
    }
    freeReplyObject(reply);
    return RD_TYPE_UNKNOWN;
}
/* }}} */
/* FUNCTION: is_tied {{{ */
static int is_tied(Param pm) {
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
/* FUNCTION: zrtie_usage {{{ */
static void zrtie_usage() {
    fprintf(stdout, YELLOW "Usage:" RESET " zrtie -d db/redis [-p] [-r] " MAGENTA "-f {host-spec}"
            RESET " " RED "{parameter_name}" RESET "\n");
    fprintf(stdout, YELLOW "Options:" RESET "\n");
    fprintf(stdout, GREEN " -d" RESET ": select database type, can change in future, currently only \"db/redis\"\n");
    fprintf(stdout, GREEN " -p" RESET ": passthrough - always do a fresh query to database, don't use cache\n");
    fprintf(stdout, GREEN " -r" RESET ": create read-only parameter\n" );
    fprintf(stdout, GREEN " -f" RESET ": database address in format {host}[:port][/[db_idx][/key]]\n");
}
/* }}} */

#else
# error no hiredis
#endif /* have hiredis */
