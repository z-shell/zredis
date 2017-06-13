/*
 * db.h
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

/* Backend commands */
#define DB_TIE 1
#define DB_UNTIE 2
#define DB_IS_TIED 3
#define DB_GET_ADDRESS 4
#define DB_CLEAR_CACHE 5

/* Data types, created according to  */
#define DB_KEY_TYPE_NONE 0  /* unknown type because non-existent key */
#define DB_KEY_TYPE_UNKNOWN 1
#define DB_KEY_TYPE_NO_KEY 2
#define DB_KEY_TYPE_STRING 3
#define DB_KEY_TYPE_LIST 4
#define DB_KEY_TYPE_SET 5
#define DB_KEY_TYPE_ZSET 6
#define DB_KEY_TYPE_HASH 7
