[![paypal](https://www.paypalobjects.com/en_US/i/btn/btn_donateCC_LG.gif)](https://www.paypal.com/cgi-bin/webscr?cmd=_s-xclick&hosted_button_id=D6XDCHDSBDSDG)

[![Built with Spacemacs](https://cdn.rawgit.com/syl20bnr/spacemacs/442d025779da2f62fc86c2082703697714db6514/assets/spacemacs-badge.svg)](http://spacemacs.org)
![ZSH 5.0.0](https://img.shields.io/badge/zsh-v5.0.0-orange.svg?style=flat-square)

# Zredis

Module interfacing with `redis` database via `variables` mapped to `keys` or whole `database`.

```zsh
% redis-cli -n 3 hmset HASHSET field1 value1 fld2 val2
% zrtie -d db/redis -f "127.0.0.1/3/HASHSET" hset
% echo ${(kv)hset}
field1 value1 fld2 val2
% echo ${(k)hset}
field1 fld2
% echo ${(v)hset}
value1 val2
% redis-cli -n 3 rpush LIST empty
% zrtie -d db/redis -f "127.0.0.1/3/LIST" list
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

## Zredis Zstyles

The values being set are the defaults. Change the values before loading `zredis` plugin.

```zsh
zstyle ":plugin:zredis" cppflags "-I/usr/local/include"  # Additional include directory
zstyle ":plugin:zredis" cflags "-Wall -O2 -g"            # Additional CFLAGS
zstyle ":plugin:zredis" ldflags "-L/usr/local/lib"       # Additional library directory
```
