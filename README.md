[![Built with Spacemacs](https://cdn.rawgit.com/syl20bnr/spacemacs/442d025779da2f62fc86c2082703697714db6514/assets/spacemacs-badge.svg)](http://spacemacs.org)
![ZSH 5.0.0](https://img.shields.io/badge/zsh-v5.0.0-orange.svg?style=flat-square)
[![Zredis](http://img.shields.io/zredis/0.91.png)](https://github.com/zdharma/zredis/releases)

[![paypal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=D6XDCHDSBDSDG)

# Zredis

Module interfacing with `redis` database via `Zshell` `variables` mapped to `keys` or whole `database`.

```SystemVerilog
% redis-cli -n 3 hmset HASHSET field1 value1 fld2 val2
% ztie -d db/redis -f "127.0.0.1/3/HASHSET" hset
% echo ${(kv)hset}
field1 value1 fld2 val2
% echo ${(k)hset}
field1 fld2
% echo ${(v)hset}
value1 val2
% ztie -d db/redis -f "127.0.0.1/3/LIST" -L list list  % lazy binding, will create string-key on write
% echo ${(t)list}
array-special
% list=( ${(k)hset} )
% echo $list
field1 fld2
% redis-cli -n 3 lrange LIST 0 -1
1) "field1"
2) "fld2"
% for (( i=1; i <= 2000; i ++ )); do; hset[$i]=$i; done
% echo ${#hset}
2002
```
## Rationale

Building commands for `redis-cli` quickly becomes inadequate. For example, if copying
of one hash to another one is needed, what `redis-cli` invocations are needed? With
`zredis`, this task is simple:

```SystemVerilog
% ztie -r -d db/redis -f "127.0.0.1/3/HASHSET1" hset1 # -r - read-only
% ztie -d db/redis -f "127.0.0.1/3/HASHSET2" hset2
% echo ${(kv)hset2}
other data
% echo ${(kv)hset1}
fld2 val2 field1 value1
% hset2=( "${(kv)hset1[@]}" )
% echo ${(kv)hset2}
fld2 val2 field1 value1
```

The `"${(kv)hset1[@]}"` construct guarantees that empty elements (keys or values) will
be preserved, thanks to quoting and `@` operator. `(kv)` means keys and values, alternating.
 
Or, for example, if one needs a large sorted set (`zset`), how to accomplish this with
`redis-cli`? With `zredis`, this is as always simple:

```SystemVerilog
% redis-cli -n 3 zadd NEWZSET 1.0 a
% ztie -d db/redis -f "127.0.0.1/3/NEWZSET" zset
% echo ${(kv)zset}
a 1
% count=0
% for i in {a..z} {A..Z}; do
> (( count ++ ))
> zset[$i]=$count;
> done
% echo ${(kv)zset}
a 1 b 2 c 3 d 4 e 5 f 6 g 7 h 8 i 9 j 10 k 11 l 12 m 13 n 14 o 15 p 16 q 17 r 18 s 19 t 20 u 21 v 22 w 23 x 24 y 25 z 26 A 27 B 28 C 29 D 30 E 31 F 32 G 33 H 34 I 35 J 36 K 37 L 38 M 39 N 40 O 41 P 42 Q 43 R 44 S 45 T 46 U 47 V 48 W 49 X 50 Y 51 Z 52
% zrzset -h
Usage: zrzset {tied-param-name}
Output: $reply array, to hold elements of the sorted set
% zrzset zset
% echo $reply
a b c d e f g h i j k l m n o p q r s t u v w x y z A B C D E F G H I J K L M N O P Q R S T U V W X Y Z
```

## Mapping of redis types to Zsh data structures
### Database string keys -> Zsh hash

Redis can store strings at given keys, using `SET` command. `Zredis` maps those to hash array:

```SystemVerilog
% redis-cli -n 4 SET key1 value1
% redis-cli -n 4 SET key2 value2
% ztie -d db/redis -f "127.0.0.1/4" redis
% echo $zredis_tied
redis
% echo ${(kv)redis}
key1 value1 key2 value2
```

### Redis hash -> Zsh hash

By appending `/NAME` to the `host-spec` (`-f` option), one can select single
key of type `HASH` and map it to `Zsh` hash:

```SystemVerilog
% redis-cli -n 4 hmset HASH key1 value1 key2 value2
% ztie -d db/redis -f "127.0.0.1/4/HASH" hset
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

```SystemVerilog
% redis-cli -n 4 sadd SET value1 value2 value3 ''
% ztie -d db/redis -f "127.0.0.1/4/SET" myset
% echo ${myset[@]}
value2 value3 value1
% echo -E ${(qq)myset[@]}  # Quote with '', use to see empty elements
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

```SystemVerilog
% redis-cli -n 4 zadd NEWZSET 1.0 a
% ztie -d db/redis -f "127.0.0.1/4/NEWZSET" zset
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

```SystemVerilog
% redis-cli -n 4 rpush LIST value1 value2 value3
% ztie -d db/redis -f "127.0.0.1/4/LIST" mylist
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

```SystemVerilog
% redis-cli -n 4 KEYS "*"
1) "key1"
2) "SET"
3) "LIST"
4) "HASH"
5) "NEWZSET"
6) "key2"
% ztie -d db/redis -f "127.0.0.1/4/key1" key1
% echo $key1
value1
% key1=valueB
% echo $key1
valueB
% redis-cli -n 4 get key1
"valueB"
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
```
