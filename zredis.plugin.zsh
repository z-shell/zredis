#
# No plugin manager is needed to use this file. All that is needed is adding:
#   source {where-zcommodore-is}/zcommodore.plugin.zsh
#
# to ~/.zshrc.
#

0="${(%):-%N}" # this gives immunity to functionargzero being unset
ZGDBM_REPO_DIR="${0%/*}"
ZGDBM_CONFIG_DIR="$HOME/.config/zgdbm"

#
# Update FPATH if:
# 1. Not loading with Zplugin
# 2. Not having fpath already updated (that would equal: using other plugin manager)
#

if [[ -z "$ZPLG_CUR_PLUGIN" && "${fpath[(r)$ZGDBM_REPO_DIR]}" != $ZGDBM_REPO_DIR ]]; then
    fpath+=( "$ZGDBM_REPO_DIR" )
fi

[[ -z "${fg_bold[green]}" ]] && builtin autoload -Uz colors && colors

#
# Compile the module
#

zgdbm_compile() {
    # Get CPPFLAGS, CFLAGS, LDFLAGS
    local cppf cf ldf
    zstyle -s ":plugin:zgdbm" cppflags cppf || cppf="-I/usr/local/include"
    zstyle -s ":plugin:zgdbm" cflags cf || cf="-Wall -O2"
    zstyle -s ":plugin:zgdbm" ldflags ldf || ldf="-L/usr/local/lib"

    (
        local build=1
        zmodload zsh/system && { zsystem flock -t 1 "${ZGDBM_REPO_DIR}/module/configure" || build=0; }
        if (( build )); then
            builtin cd "${ZGDBM_REPO_DIR}/module"
            CPPFLAGS="$cppf" CFLAGS="$cf" LDFLAGS="$ldf" ./configure
            command make clean
            command make

            local ts="$EPOCHSECONDS"
            [[ -z "$ts" ]] && ts=$( date +%s )
            builtin echo "$ts" >! "${ZGDBM_REPO_DIR}/module/COMPILED_AT"
        fi
    )
}

if [ ! -e "${ZGDBM_REPO_DIR}/module/Src/zdharma/zgdbm.so" ]; then
    builtin print "${fg_bold[magenta]}zdharma${reset_color}/${fg_bold[yellow]}zgdbm${reset_color} is building..."
    zgdbm_compile
elif [[ ! -f "${ZGDBM_REPO_DIR}/module/COMPILED_AT" || ( "${ZGDBM_REPO_DIR}/module/COMPILED_AT" -ot "${ZGDBM_REPO_DIR}/module/RECOMPILE_REQUEST" ) ]]; then
    # Don't trust access times and verify hard stored values
    [[ -e ${ZGDBM_REPO_DIR}/module/COMPILED_AT ]] && local compiled_at_ts="$(<${ZGDBM_REPO_DIR}/module/COMPILED_AT)"
    [[ -e ${ZGDBM_REPO_DIR}/module/RECOMPILE_REQUEST ]] && local recompile_request_ts="$(<${ZGDBM_REPO_DIR}/module/RECOMPILE_REQUEST)"

    if [[ "${recompile_request_ts:-1}" -gt "${compiled_at_ts:-0}" ]]; then
        builtin echo "${fg_bold[red]}zgdbm: single recompiletion requested by plugin's update${reset_color}"
        zgdbm_compile
    fi
fi

# Finally load the module - if it has compiled
if [[ -e "${ZGDBM_REPO_DIR}/module/Src/zdharma/zgdbm.so" ]]; then
    MODULE_PATH="${ZGDBM_REPO_DIR}/module/Src":"$MODULE_PATH"
    zmodload zdharma/zgdbm
fi
