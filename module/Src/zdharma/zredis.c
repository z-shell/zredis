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

#ifndef PM_UPTODATE
#define PM_UPTODATE     (1<<19) /* Parameter has up-to-date data (e.g. loaded from DB) */
#endif

/*
 * Make sure we have all the bits I'm using for memory mapping, otherwise
 * I don't know what I'm doing.
 */
#if defined(HAVE_HIREDIS_HIREDIS_H) && defined(HAVE_REDISCONNECT)

#define RD_TYPE_UNKNOWN 0
#define RD_TYPE_STRING 1
#define RD_TYPE_LIST 2
#define RD_TYPE_SET 3
#define RD_TYPE_ZSET 4
#define RD_TYPE_HASH 5

#include <hiredis/hiredis.h>

static Param createhash(char *name, int flags);
static int append_tied_name(const char *name);
static int remove_tied_name(const char *name);
static char *unmetafy_zalloc(const char *to_copy, int *new_len);
static void set_length(char *buf, int size);
static void parse_host_string(const char *input, char *buffer, int size,
                                char **host, int *port, int *db_index, char **key);
static int connect(char *nam, redisContext **rc, const char *host, int port, int db_index,
                    const char *resource_name_in);

static char *backtype = "db/redis";

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
 */

struct gsu_scalar_ext {
    struct gsu_scalar std;
    redisContext *rc;
    char *redis_host_port;
};

/* Source structure - will be copied to allocated one,
 * with `rc` filled. `rc` allocation <-> gsu allocation. */
static const struct gsu_scalar_ext gdbm_gsu_ext =
{ { redis_getfn, redis_setfn, redis_unsetfn }, 0, 0 };

/**/
static const struct gsu_hash redis_hash_gsu =
{ hashgetfn, redis_hash_setfn, redis_hash_unsetfn };

static struct builtin bintab[] = {
    BUILTIN("zrtie", 0, bin_zrtie, 1, -1, 0, "d:f:r", NULL),
    BUILTIN("zruntie", 0, bin_zruntie, 1, -1, 0, "u", NULL),
    BUILTIN("zredishost", 0, bin_zredishost, 1, -1, 0, "", NULL),
    BUILTIN("zredisclear", 0, bin_zredisclear, 2, -1, 0, "", NULL),
};

#define ROARRPARAMDEF(name, var) \
    { name, PM_ARRAY | PM_READONLY, (void *) var, NULL,  NULL, NULL, NULL }

/* Holds names of all tied parameters */
char **zredis_tied;

static struct paramdef patab[] = {
    ROARRPARAMDEF("zredis_tied", &zredis_tied),
};

/**/
static int
bin_zrtie(char *nam, char **args, Options ops, UNUSED(int func))
{
    redisContext *rc = NULL;
    int read_write = 1, pmflags = PM_REMOVABLE;
    Param tied_param;

    /* Check options */

    if(!OPT_ISSET(ops,'d')) {
        zwarnnam(nam, "you must pass `-d %s'", backtype);
	return 1;
    }
    if(!OPT_ISSET(ops,'f')) {
        zwarnnam(nam, "you must pass `-f' with host[:port][/[db_idx][/key]]", NULL);
	return 1;
    }
    if (OPT_ISSET(ops,'r')) {
	read_write = 0;
	pmflags |= PM_READONLY;
    }

    if (strcmp(OPT_ARG(ops, 'd'), backtype) != 0) {
        zwarnnam(nam, "unsupported backend type `%s'", OPT_ARG(ops, 'd'));
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

    if ((tied_param = (Param)paramtab->getnode(paramtab, pmname)) &&
	!(tied_param->node.flags & PM_UNSET)) {
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

    /* Create hash */

    if (!(tied_param = createhash(pmname, pmflags))) {
        zwarnnam(nam, "cannot create the requested parameter %s", pmname);
        redisFree(rc);
	return 1;
    }

    append_tied_name(pmname);

    tied_param->gsu.h = &redis_hash_gsu;

    /* Allocate parameter sub-gsu, fill rc field.
     * rc allocation is 1 to 1 accompanied by
     * gsu_scalar_ext allocation. */

    struct gsu_scalar_ext *rc_carrier = (struct gsu_scalar_ext *) zalloc(sizeof(struct gsu_scalar_ext));
    rc_carrier->std = gdbm_gsu_ext.std;
    rc_carrier->rc = rc;
    tied_param->u.hash->tmpdata = (void *)rc_carrier;

    /* Fill also host:port// field */
    rc_carrier->redis_host_port = ztrdup(resource_name_in);
    return 0;
}

/**/
static int
bin_zruntie(char *nam, char **args, Options ops, UNUSED(int func))
{
    Param pm;
    char *pmname;
    int ret = 0;

    for (pmname = *args; *args++; pmname = *args) {
	pm = (Param) paramtab->getnode(paramtab, pmname);
	if(!pm) {
	    zwarnnam(nam, "cannot untie %s", pmname);
	    ret = 1;
	    continue;
	}
	if (pm->gsu.h != &redis_hash_gsu) {
	    zwarnnam(nam, "not a tied redis hash: %s", pmname);
	    ret = 1;
	    continue;
	}

	queue_signals();
	if (OPT_ISSET(ops,'u'))
	    redisuntie(pm);	/* clear read-only-ness */
	if (unsetparam_pm(pm, 0, 1)) {
	    /* assume already reported */
	    ret = 1;
	}
	unqueue_signals();
    }

    return ret;
}

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

    if (pm->gsu.h != &redis_hash_gsu) {
        zwarnnam(nam, "not a tied gdbm parameter: %s", pmname);
        return 1;
    }

    /* Paranoia, it *will* be always set */
    if (((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->redis_host_port) {
        setsparam("REPLY", ztrdup(((struct gsu_scalar_ext *)pm->u.hash->tmpdata)->redis_host_port));
    } else {
        setsparam("REPLY", ztrdup(""));
    }

    return 0;
}

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
    if(!pm) {
        zwarnnam(nam, "no such parameter: %s", pmname);
        return 1;
    }

    if (pm->gsu.h != &redis_hash_gsu) {
        zwarnnam(nam, "not a tied gdbm parameter: %s", pmname);
        return 1;
    }

    HashTable ht = pm->u.hash;
    HashNode hn = gethashnode2(ht, key);
    Param val_pm = (Param) hn;
    if (val_pm) {
        val_pm->node.flags &= ~(PM_UPTODATE);
    }

    return 0;
}

/*
 * The param is actual param in hash – always, because
 * getgdbmnode creates every new key seen. However, it
 * might be not PM_UPTODATE - which means that database
 * wasn't yet queried.
 *
 * It will be left in this state if database doesn't
 * contain such key. That might be a drawback, maybe
 * setting to empty value has sense, as no other writer
 * can exist. This would remove subtle hcalloc(1) leak.
 */

/**/
static char *
redis_getfn(Param pm)
{
    char *key;
    size_t key_len;
    redisContext *rc;
    redisReply *reply;

    /* Key already retrieved? There is no sense of asking the
     * database again, because:
     * - there can be only multiple readers
     * - so, no writer + reader use is allowed
     *
     * Thus:
     * - if we are writers, we for sure have newest copy of data
     * - if we are readers, we for sure have newest copy of data
     */
    if (pm->node.flags & PM_UPTODATE) {
        return pm->u.str ? pm->u.str : (char *) hcalloc(1);
    }

    /* Unmetafy key. Redis fits nice into this
     * process, as it can use length of data */
    int umlen = 0;
    char *umkey = unmetafy_zalloc(pm->node.nam, &umlen);

    key = umkey;
    key_len = umlen;

    rc = ((struct gsu_scalar_ext *)pm->gsu.s)->rc;

    reply = redisCommand(rc, "EXISTS %b", key, (size_t) key_len);
    if (reply && reply->type == REDIS_REPLY_INTEGER && reply->integer == 1) {
        freeReplyObject(reply);

        /* We have data – store it, return it */
        pm->node.flags |= PM_UPTODATE;

        reply = redisCommand(rc, "GET %b", key, (size_t) key_len);
        if (reply && reply->type == REDIS_REPLY_STRING) {
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
            freeReplyObject(reply);
        }

        /* Free key */
        set_length(umkey, key_len);
        zsfree(umkey);
    }
}

/**/
static void
redis_unsetfn(Param pm, UNUSED(int um))
{
    /* Set with NULL */
    redis_setfn(pm, NULL);
}

/**/
static HashNode
getgdbmnode(HashTable ht, const char *name)
{
    HashNode hn = gethashnode2(ht, name);
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

    if (!val_pm) {
        val_pm = (Param) zshcalloc(sizeof (*val_pm));
        val_pm->node.flags = PM_SCALAR | PM_HASHELEM; /* no PM_UPTODATE */
        val_pm->gsu.s = (GsuScalar) ht->tmpdata;
        ht->addnode(ht, ztrdup(name), val_pm); // sets pm->node.nam
    }

    return (HashNode) val_pm;
}

/**/
static void
scangdbmkeys(HashTable ht, ScanFunc func, int flags)
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

        /* This returns database-interfacing Param,
         * it will return u.str or first fetch data
         * if not PM_UPTODATE (newly created) */
        char *zkey = metafy(key, key_len, META_DUP);
        HashNode hn = getgdbmnode(ht, zkey);
        zsfree(zkey);

	func(hn, flags);
    }

    freeReplyObject(reply);
}

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

	queue_signals();

        /* DEL */
        reply2 = redisCommand(rc, "DEL %b", key, (size_t) key_len);
        freeReplyObject(reply2);

	unqueue_signals();
    }
    freeReplyObject(reply);

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

/**/
static void
redisuntie(Param pm)
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

/**/
static void
redis_hash_unsetfn(Param pm, UNUSED(int exp))
{
    redisuntie(pm);

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

static struct features module_features = {
    bintab, sizeof(bintab)/sizeof(*bintab),
    NULL, 0,
    NULL, 0,
    patab, sizeof(patab)/sizeof(*patab),
    0
};

/**/
int
setup_(UNUSED(Module m))
{
    return 0;
}

/**/
int
features_(Module m, char ***features)
{
    *features = featuresarray(m, &module_features);
    return 0;
}

/**/
int
enables_(Module m, int **enables)
{
    return handlefeatures(m, &module_features, enables);
}

/**/
int
boot_(UNUSED(Module m))
{
    zredis_tied = zshcalloc((1) * sizeof(char *));
    return 0;
}

/**/
int
cleanup_(Module m)
{
    /* This frees `zredis_tied` */
    return setfeatureenables(m, &module_features, NULL);
}

/**/
int
finish_(UNUSED(Module m))
{
    return 0;
}

/*********************
 * Utility functions *
 *********************/

static Param createhash(char *name, int flags) {
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
    }

    /* These provide special features */
    ht->getnode = ht->getnode2 = getgdbmnode;
    ht->scantab = scangdbmkeys;

    return pm;
}

/*
 * Adds parameter name to `zredis_tied`
 */

static int append_tied_name(const char *name) {
    int old_len = arrlen(zredis_tied);
    char **new_zgdbm_tied = zshcalloc((old_len+2) * sizeof(char *));

    /* Copy */
    char **p = zredis_tied;
    char **dst = new_zgdbm_tied;
    while (*p) {
        *dst++ = *p++;
    }

    /* Append new one */
    *dst = ztrdup(name);

    /* Substitute, free old one */
    zfree(zredis_tied, sizeof(char *) * (old_len + 1));
    zredis_tied = new_zgdbm_tied;

    return 0;
}

/*
 * Removes parameter name from `zredis_tied`
 */

static int remove_tied_name(const char *name) {
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
        char **new_zgdbm_tied = zshcalloc((new_len+1) * sizeof(char *));

        /* Copy */
        p = zredis_tied;
        char **dst = new_zgdbm_tied;
        while (*p) {
            *dst++ = *p++;
        }
        *dst = NULL;

        /* Substitute, free old one */
        zfree(zredis_tied, sizeof(char *) * (old_len + 1));
        zredis_tied = new_zgdbm_tied;
    }

    return 0;
}

/*
 * Unmetafy that:
 * - duplicates bufer to work on it,
 * - does zalloc of exact size for the new string,
 * - restores work buffer to original content, to restore strlen
 *
 * No zsfree()-confusing string will be produced.
 */
static char *unmetafy_zalloc(const char *to_copy, int *new_len) {
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

static int type(redisContext *rc, char *key, int key_len) {
    redisReply *reply = NULL;
    reply = redisCommand(rc, "TYPE %b", key, (size_t) key_len);
    if (reply == NULL || reply->type != REDIS_REPLY_STRING) {
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
    return RD_TYPE_UNKNOWN;
}

#else
# error no gdbm
#endif /* have gdbm */
