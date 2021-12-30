# -*- Mode: sh; sh-indentation: 4; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# vim:ft=zsh:sw=4:sts=4:et
#
# No plugin manager is needed to use this file. All that is needed is adding:
#   source {where-zredis-is}/zredis.plugin.zsh
#
# to ~/.zshrc.
#

0="${${ZERO:-${0:#$ZSH_ARGZERO}}:-${(%):-%N}}"
0="${${(M)0:#/*}:-$PWD/$0}"

ZREDIS_REPO_DIR="${0:h}"
ZREDIS_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/zredis"

#
# Update FPATH if:
# 1. Not loading with Zplugin
# 2. Not having fpath already updated (that would equal: using other plugin manager)
#

if [[ ( ${+zsh_loaded_plugins} = 0 || ${zsh_loaded_plugins[-1]} != */zredis ) && -z "${fpath[(r)${0:h}]}" ]]; then
    fpath+=( "$ZREDIS_REPO_DIR" )
fi

[[ -z "${fg_bold[green]}" ]] && builtin autoload -Uz colors && colors

autoload zredis_compile

#
# Compile the module
#

if [ ! -e "${ZREDIS_REPO_DIR}/module/Src/zshell/db.so" ]; then
    zredis_compile
elif [[ ! -f "${ZREDIS_REPO_DIR}/module/COMPILED_AT" || ( "${ZREDIS_REPO_DIR}/module/COMPILED_AT" -ot "${ZREDIS_REPO_DIR}/module/RECOMPILE_REQUEST" ) ]]; then
    # Don't trust access times and verify hard stored values
    [[ -e ${ZREDIS_REPO_DIR}/module/COMPILED_AT ]] && local compiled_at_ts="$(<${ZREDIS_REPO_DIR}/module/COMPILED_AT)"
    [[ -e ${ZREDIS_REPO_DIR}/module/RECOMPILE_REQUEST ]] && local recompile_request_ts="$(<${ZREDIS_REPO_DIR}/module/RECOMPILE_REQUEST)"

    if [[ "${recompile_request_ts:-1}" -gt "${compiled_at_ts:-0}" ]]; then
        builtin echo "${fg_bold[red]}zredis: single recompilation requested by plugin's update${reset_color}"
        zredis_compile
    fi
fi

# Finally load the module - if it has compiled
MODULE_PATH="${ZREDIS_REPO_DIR}/module/Src":"$MODULE_PATH"
if [[ -e "${ZREDIS_REPO_DIR}/module/Src/zshell/zredis.so" ]]; then
    #zmodload -u zshell/db 2>/dev/null
    zmodload -d zshell/zredis zshell/db
    zmodload zshell/zredis
fi
