#
# No plugin manager is needed to use this file. All that is needed is adding:
#   source {where-zredis-is}/zredis.plugin.zsh
#
# to ~/.zshrc.
#

0="${(%):-%N}" # this gives immunity to functionargzero being unset
ZREDIS_REPO_DIR="${0%/*}"
ZREDIS_CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/zredis"

#
# Update FPATH if:
# 1. Not loading with Zplugin
# 2. Not having fpath already updated (that would equal: using other plugin manager)
#

if [[ -z "$ZPLG_CUR_PLUGIN" && "${fpath[(r)$ZREDIS_REPO_DIR]}" != $ZREDIS_REPO_DIR ]]; then
    fpath+=( "$ZREDIS_REPO_DIR" )
fi

[[ -z "${fg_bold[green]}" ]] && builtin autoload -Uz colors && colors

#
# Compile the module
#

zredis_compile() {
    # Get CPPFLAGS, CFLAGS, LDFLAGS
    local cppf cf ldf
    zstyle -s ":plugin:zredis" cppflags cppf || cppf="-I/usr/local/include"
    zstyle -s ":plugin:zredis" cflags cf || cf="-Wall -O2 -g"
    zstyle -s ":plugin:zredis" ldflags ldf || ldf="-L/usr/local/lib"

    (
        local build=1
        zmodload zsh/system && { zsystem flock -t 1 "${ZREDIS_REPO_DIR}/module/configure" || build=0; }
        if (( build )); then
            builtin cd "${ZREDIS_REPO_DIR}/module"
            CPPFLAGS="$cppf" CFLAGS="$cf" LDFLAGS="$ldf" ./configure
            command make clean
            command make

            local ts="$EPOCHSECONDS"
            [[ -z "$ts" ]] && ts=$( date +%s )
            builtin echo "$ts" >! "${ZREDIS_REPO_DIR}/module/COMPILED_AT"
        fi
    )
}

if [ ! -e "${ZREDIS_REPO_DIR}/module/Src/zdharma/zredis.so" ]; then
    builtin print "${fg_bold[magenta]}zdharma${reset_color}/${fg_bold[yellow]}zredis${reset_color} is building..."
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
if [[ -e "${ZREDIS_REPO_DIR}/module/Src/zdharma/zredis.so" ]]; then
    MODULE_PATH="${ZREDIS_REPO_DIR}/module/Src":"$MODULE_PATH"
    #zmodload -u zdharma/db 2>/dev/null
    zmodload -d zdharma/zredis zdharma/db
    zmodload zdharma/zredis
fi
