[![Built with Spacemacs](https://cdn.rawgit.com/syl20bnr/spacemacs/442d025779da2f62fc86c2082703697714db6514/assets/spacemacs-badge.svg)](http://spacemacs.org)
![ZSH 5.0.0](https://img.shields.io/badge/zsh-v5.0.0-orange.svg?style=flat-square)
[![Zredis](https://img.shields.io/badge/zredis-0.93-green.svg)](https://github.com/zdharma/zredis/releases)

<!-- START doctoc generated TOC please keep comment here to allow auto update -->
<!-- DON'T EDIT THIS SECTION, INSTEAD RE-RUN doctoc TO UPDATE -->
**Table of Contents**  *generated with [DocToc](https://github.com/thlorenz/doctoc)*

- [Zredis](#zredis)
  - [Rationale](#rationale)
  - [Deleting From Database](#deleting-from-database)
  - [Compiling modules](#compiling-modules)
  - [Cache](#cache)
  - [News](#news)
  - [Mapping Of Redis Types To Zsh Data Structures](#mapping-of-redis-types-to-zsh-data-structures)
    - [Database string keys -> Zsh hash](#database-string-keys---zsh-hash)
    - [Redis hash -> Zsh hash](#redis-hash---zsh-hash)
    - [Redis set -> Zsh array](#redis-set---zsh-array)
    - [Redis sorted set -> Zsh hash](#redis-sorted-set---zsh-hash)
    - [Redis list -> Zsh array](#redis-list---zsh-array)
    - [Redis string key -> Zsh string](#redis-string-key---zsh-string)
- [Installation](#installation)
    - [Zplugin](#zplugin)
    - [Antigen](#antigen)
    - [Oh-My-Zsh](#oh-my-zsh)
    - [Zgen](#zgen)
- [Zredis Zstyles](#zredis-zstyles)

<!-- END doctoc generated TOC please keep comment here to allow auto update -->

# Zredis

Zsh binary module written in C interfacing with `redis` database via `Zshell`
`variables` mapped to `keys` or the whole `database`.

```SystemVerilog
% redis-cli -n 3 hmset HASHSET field1 value1 field2 value2
% ztie -d db/redis -a "127.0.0.1/3/HASHSET" hset
% echo ${(kv)hset}  # (kv) – keys and values of Zsh hash
field1 value1 field2 value2
% echo ${(k)hset}
field1 field2
% echo $hset  # values are output by default
value1 value2

% ztie -d db/redis -a "127.0.0.1/3/LIST" -L list lst # Lazy binding, will create list-key on write
                                                     # -L {type}, obtains Redis type name like zset, hash, string
% echo ${(t)lst}  # (t) – display type of Zsh variable
array-special
% lst=( ${(k)hset} )  # Copying hash keys into list
% echo $lst
field1 field2
% redis-cli -n 3 lrange LIST 0 -1
1) "field1"
2) "field2"
```
## Rationale

Building commands for `redis-cli` quickly becomes inadequate. For example, if copying
of one hash to another one is needed, what `redis-cli` invocations are needed? With
`zredis`, this task is simple:

```zsh
% ztie -r -d db/redis -a "127.0.0.1/3/HASHSET1" hset1 # -r - read-only
% ztie -d db/redis -a "127.0.0.1/3/HASHSET2" hset2
% echo ${(kv)hset2}
other data
% echo ${(kv)hset1}
field1 value1 field2 value2
% hset2=( "${(kv)hset1[@]}" )
% echo ${(kv)hset2}
field1 value1 field2 value2
```

The `"${(kv)hset1[@]}"` construct guarantees that empty elements (keys or values) will
be preserved, thanks to quoting and `@` operator. `(kv)` means keys and values, alternating.
 
Or, for example, if one needs a large sorted set (`zset`), how to accomplish this with
`redis-cli`? With `zredis`, one can do:

```zsh
% redis-cli -n 3 zadd NEWZSET 1.0 a
% ztie -d db/redis -a "127.0.0.1/3/NEWZSET" zset
% echo ${(kv)zset}
a 1
% count=0
% for i in {a..z} {A..Z}; do (( count ++ )); zset[$i]=$count; done
% echo ${(kv)zset}
a 1 b 2 c 3 d 4 e 5 f 6 g 7 h 8 i 9 j 10 k 11 l 12 m 13 n 14 o 15 p 16 q 17 r 18 s 19 t 20 u 21 v 22 w 23 x 24 y 25 z 26 A 27 B 28 C 29 D 30 E 31 F 32 G 33 H 34 I 35 J 36 K 37 L 38 M 39 N 40 O 41 P 42 Q 43 R 44 S 45 T 46 U 47 V 48 W 49 X 50 Y 51 Z 52
% zrzset -h
Usage: zrzset {tied-param-name}
Output: $reply array, to hold elements of the sorted set
% zrzset zset
% echo $reply
a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W X Y Z
```

## Deleting From Database

Unsetting the first type of mapped variable (Zsh hash -> whole database) doesn't cause a deletion from
database. If option `-D` is given to `ztie` when binding to concrete key in database, then unsets, also
caused by automatic Zsh scoping actions, cause the corresponding key to be deleted. `zuntie` never deletes
from database.

More: in Redis, removing all elements from a set, list, etc. means the same as deletion. So you can delete
all datatypes except string, by doing `variable=()`. For string you can unset key in whole-database mapped
hash: `unset 'wholedb[key]'`.

## Compiling modules

The Zsh modules provided by the plugin will build automatically (`hiredis` library is needed). You can
start more than 1 shell, only the first one will be compiling. If a developer commits a new timestamp to
`module/RECOMPILE_REQUEST`, the module will recompile (don't worry, at startup, `mtime` is checked
first, so check for recompilation is fast). I do this when I add tested features or fixes. You can
recompile the modules yourself by invoking Zsh function `zredis_compile`.

## Cache

By default, reads are cached. If a tied variable is read for the first time,
then database is accessed. For the second read there's no database access.
Writes aren't cached in any way.

To clear the cache, invoke:

```zsh
ztclear my_string_var       # Also for types: list, set
ztclear my_hashset_var key  # Also for types: whole-db mapping, zset
```

To disable the cache, pass `-z` ("zero-cache") option to ztie.

## News
* 2018-12-19
  - The builtin `zrpush` can have the param-name argument skipped – if it's called for the second
    time, meaning that a new special (but writeable) parameter has been set – `$zredis_last`. It
    holds the param-name used in the 1st call and will be used in place of the `{pm-name}` argument.
    The short call is then to look like the following: `zrpush {l|r} [ {val1} {val2} ... ]`.

* 2018-12-18
  - New builtin `zrpush {l|r} {pm-name} [ {val1} {val2} ... ]` that in an optimized manner pushes
    the given elements `{val1} {val2}`, etc. onto the front or back (i.e. `l|r`, left or right,
    head or tail) of the list tied to param `{pm-name}`.
  - Hash-**set** operation (i.e. `hsh=( a b ...)`) has been greately optimized for over-internet tied hash parameters.

* 2018-01-09
  - New option to `ztie`: `-S`, which used in conjunction with `-L` (lazy binding) causes database connection
    to be defered until first use of variable. Standard lazy binding means: key isn't required to exist.

* 2018-01-08
  - New option to `ztie`: `-D`, which causes mapped database key to be deleted on `unset` of the tied
    variable. Up to this moment this behavior was the default.

## Mapping Of Redis Types To Zsh Data Structures
### Database string keys -> Zsh hash

Redis can store strings at given keys, using `SET` command. `Zredis` maps those to hash array
(like Zsh `gdbm` module):

```
% redis-cli -n 4 SET key1 value1
% redis-cli -n 4 SET key2 value2
% ztie -d db/redis -a "127.0.0.1/4" redis
% echo $zredis_tied
redis
% echo ${(kv)redis}
key1 value1 key2 value2
```

### Redis hash -> Zsh hash

By appending `/NAME` to the `host-spec` (`-f` option), one can select single
key of type `HASH` and map it to `Zsh` hash:

```
% redis-cli -n 4 hmset HASH key1 value1 key2 value2
% ztie -d db/redis -a "127.0.0.1/4/HASH" hset
% echo $zredis_tied
hset
% echo ${(kv)hset}
key1 value1 key2 value2
% echo $hset[key2]
value2
% unset 'hset[key2]'
% echo ${(kv)hset}
key1 value1
```

### Redis set -> Zsh array

Can clear single elements by assigning `()` to array element. Can overwrite
whole set by assigning via `=( ... )` to set, and delete set from database
by use of `unset`. Use `zuntie` to only detach variable from database without
deleting any data.

```
% redis-cli -n 4 sadd SET value1 value2 value3 ''
% ztie -d db/redis -a "127.0.0.1/4/SET" myset
% echo ${myset[@]}
value2 value3 value1
% echo -E ${(qq)myset[@]}  # (qq) – quote with '', use to see empty elements
'value2' 'value3' '' 'value1'
% myset=( 1 2 3 )
% myset[2]=()
% redis-cli -n 4 smembers SET
1) "1"
3) "3"
% unset myset
% redis-cli -n 4 smembers SET
(empty list or set)
```

### Redis sorted set -> Zsh hash

This variant maps `zset` as hash - keys are set elements, values are ranks.
`zrzset` call outputs elements sorted according to the rank:

```
% redis-cli -n 4 zadd NEWZSET 1.0 a
% ztie -d db/redis -a "127.0.0.1/4/NEWZSET" zset
% echo ${(kv)zset}
a 1
% zset[a]=2.5
% zset[b]=1.5
% zrzset zset
% echo $reply
b a
```

### Redis list -> Zsh array

There is no analogue of `zrzset` call because `Zsh` array already has correct order:

```zsh
% redis-cli -n 4 rpush LIST value1 value2 value3
% ztie -d db/redis -a "127.0.0.1/4/LIST" mylist
% echo $mylist
value1 value2 value3
% mylist=( 1 2 3 )
% mylist[2]=()
% redis-cli -n 4 lrange LIST 0 -1
1) "1"
3) "3"
% zuntie mylist
% redis-cli -n 4 lrange LIST 0 -1
1) "1"
3) "3"
```

### Redis string key -> Zsh string

Single keys in main Redis storage are bound to `Zsh` string variables:

```zsh
% redis-cli -n 4 KEYS "*"
1) "key1"
2) "SET"
3) "LIST"
4) "HASH"
5) "NEWZSET"
6) "key2"
% ztie -d db/redis -a "127.0.0.1/4/key1" key1
% echo $key1
value1
% key1=value2
% echo $key1
value2
% redis-cli -n 4 get key1
"value2"
```

# Installation

**The plugin is "standalone"**, which means that only sourcing it is needed. So to
install, unpack `zredis` somewhere and add

```SystemVerilog
source {where-zredis-is}/zredis.plugin.zsh
```

to `zshrc`.

If using a plugin manager, then `Zplugin` is recommended, but you can use any
other too, and also install with `Oh My Zsh` (by copying directory to
`~/.oh-my-zsh/custom/plugins`).

### [Zplugin](https://github.com/zdharma/zplugin)

Add `zplugin light zdharma/zredis` to your `.zshrc` file. Zplugin will handle
cloning the plugin for you automatically the next time you start zsh. To update
issue `zplugin update zdharma/zredis`.

### Antigen

Add `antigen bundle zdharma/zredis` to your `.zshrc` file. Antigen will handle
cloning the plugin for you automatically the next time you start zsh.

### Oh-My-Zsh

1. `cd ~/.oh-my-zsh/custom/plugins`
2. `git clone git@github.com:zdharma/zredis.git`
3. Add `zredis` to your plugin list

### Zgen

Add `zgen load zdharma/zredis` to your .zshrc file in the same place you're doing
your other `zgen load` calls in.

# Zredis Zstyles

The values being set are the defaults. Change the values before loading `zredis` plugin.

```zsh
zstyle ":plugin:zredis" cppflags "-I/usr/local/include"  # Additional include directory
zstyle ":plugin:zredis" cflags "-Wall -O2 -g"            # Additional CFLAGS
zstyle ":plugin:zredis" ldflags "-L/usr/local/lib"       # Additional library directory
zstyle ":plugin:zredis" configure_opts ""                # Additional ./configure options
```
