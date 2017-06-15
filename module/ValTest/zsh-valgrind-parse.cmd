#!/bin/sh
# -*- Mode: sh; sh-indentation: 4; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# vim:ft=zsh:sw=4:sts=4:et

#
# /bin/sh stage, load configuration to obtain $zsh_control_bin
#

# This barely works
SH_ZERO_DIR=${0%/zsh-valgrind-parse.cmd}

[[ -z "$ZSHV_TCONF_FILE" ]] && ZSHV_TCONF_FILE="vtest.conf"
[[ "$1" = conf:* ]] && { ZSHV_TCONF_FILE="${1#conf:}"; shift; }

if [[ -n "$ZSHV_TCONF_DIR" ]]; then
    source "${ZSHV_TCONF_DIR}/${ZSHV_TCONF_FILE}"
elif [[ -f "${SH_ZERO_DIR}/${ZSHV_TCONF_FILE}" ]]; then
    source "${SH_ZERO_DIR}/${ZSHV_TCONF_FILE}"
elif [[ -f "${PWD}/${ZSHV_TCONF_FILE}" ]]; then
    source "${PWD}/${ZSHV_TCONF_FILE}"
elif [[ -f "ValTest/${ZSHV_TCONF_FILE}" ]]; then
    source "ValTest/${ZSHV_TCONF_FILE}"
else
    echo "Couldn't find ${ZSHV_TCONF_FILE} (searched paths: \$ZSHV_TCONF_DIR=\`$ZSHV_TCONF_DIR', ${SH_ZERO_DIR}/, \$PWD,  ValTest/)"
    exit 1
fi

# Some fallbacks (currently unused)
[[ "$test_bin" = "local-zsh" ]] && test_bin="${ZTST_exe}"
[[ -z "$test_bin" ]] && test_bin="../Src/zsh"

#
# Restart with proper binary (configuration is loaded)
#

[[ -z "$ZSH_VERSION" ]] && exec /usr/bin/env "$zsh_control_bin" -f -c "source \"$0\" \"$1\" \"$2\" \"$3\" \"$4\" \"$5\" \"$6\" \"$7\" \"$8\" \"$9\" \"$10\""

#
# Init
#

typeset -g ZERO="${(%):-%N}" # this gives immunity to functionargzero being unset
typeset -g ZERO_DIR="${ZERO:h}"

emulate zsh -o warncreateglobal -o extendedglob -o typesetsilent

autoload colors; colors

typeset -g REPLY
typeset -ga reply

trap "coproc exit; return" TERM INT QUIT

source "${ZERO_DIR}/"__error*.def

mdebug_mode()
{
    [[ "$mdebug" = 1 || "$mdebug" = "yes" || "$mdebug" = "on" ]]
}

#
# Set of filters, applied in order
#

local -A filters
filters=(
    "1-ByAt" "(#b)(#s)(==[0-9]##==[[:blank:]]##)((#B)(by|at) 0x[A-F0-9]##: )(?##)[[:blank:]]\(([^:]##:[0-9]##)\)(#e)"
    "2-ByAt" "(#b)(#s)(==[0-9]##==[[:blank:]]##)((#B)(by|at) 0x[A-F0-9]##: )(?##)[[:blank:]]\((in) ([^\)[:blank:]]##)\)(#e)"
    "3-ByAt" "(#b)(#s)(==[0-9]##==[[:blank:]]##)((#B)(by|at) 0x[A-F0-9]##: )(?##)(#e)"
    "4-Summary" "(#b)(#s)(==[0-9]##==[[:blank:]]##)([^:]##:)(?#)(#e)"
    "5-Error" "(#b)(#s)(==[0-9]##==[[:blank:]]##)([^[:blank:]]?##)(#e)"
    "6-Info" "(#b)(#s)(==[0-9]##==[[:blank:]]##)([^[:blank:]]?##)(#e)"
    "7-Blank" "(#b)(#s)(==[0-9]##==[[:blank:]]#)(#e)"
)

# Helper regexes
local reachable_pat="(#b)(#s)(==[0-9]##==[[:blank:]]##)Reachable blocks?##(#e)"

#
# Theme
#
local -A theme
theme=(
    pid            $fg_bold[black]
    byat           $fg_no_bold[yellow]
    func           $fg_bold[blue]
    where_path     $fg_no_bold[red]
    where_file     $fg_bold[white] 
    where_linenr   $fg_no_bold[magenta]
    summary_header $fg_bold[green]
    summary_body   $fg_no_bold[green]
    summary_linenr $fg_no_bold[magenta]
    error          $fg_bold[red]
    info           $fg_no_bold[yellow]
    number         $fg_no_bold[magenta]
    symbol         $fg_bold[black]
    skip_msg       $fg_bold[black]
    rst            $reset_color
)

#
# Functions
#

# Block of matched lines
process_block() {
    local -a bl first_subblock_funs second_subblock_funs
    local blank="" first second

    bl=( "$@" )
    if is_blank "${bl[1]}"; then
        blank="${bl[1]}"
        shift 1 bl
    fi

    if is_error "${bl[1]}"; then
        which_byat "${bl[2]}"
        if [[ "$REPLY" != "0" ]]; then
            first="${bl[1]}"
            shift 1 bl
            integer found_idx
            found_idx=${bl[(I)*=[[:blank:]]##Address*0x*]}

            if [[ "$found_idx" -eq "0" ]]; then
                reply=()
                to_clean_stacktrace "${bl[@]}"
                if test_stack_trace "${reply[@]}"; then
                    show_block "$blank" "$first" "${bl[@]}"
                else
                    print -r -- "${theme[skip_msg]}Skipped single-block error: $REPLY${theme[rst]}"
                fi
            else
                second="${bl[found_idx]}"
                first_subblock_funs=( ${(@)bl[1,found_idx-1]} )
                second_subblock_funs=( ${(@)bl[found_idx+1,-1]} )

                reply=()
                to_clean_stacktrace "${first_subblock_funs[@]}"
                to_clean_stacktrace "${second_subblock_funs[@]}"

                if test_stack_trace "${reply[@]}"; then
                    show_block "$blank" "$first" "${first_subblock_funs[@]}" "$second" ${second_subblock_funs[@]}
                else
                    print -r -- "${theme[skip_msg]}Skipped double-block error: $REPLY${theme[rst]}"
                fi
            fi
            return
        fi
    fi

    show_block "$@"
}

# Block of unmatched lines (e.g. program output)
process_umblock() {
    print -rl -- "$@"
}

is_error() {
    [[ "$1" = "5-Error/"* ]]
}

is_blank() {
    [[ "$1" = "7-Blank/"* ]]
}

which_byat() {
    [[ "$1" = "1-ByAt/"* ]] && { REPLY="1"; return; }
    [[ "$1" = "2-ByAt/"* ]] && { REPLY="2"; return; }
    [[ "$1" = "3-ByAt/"* ]] && { REPLY="3"; return; }
    REPLY="0"
}

to_clean_stacktrace() {
    local -a lines out match mbegin mend
    local l
    lines=( "$@" )

    for l in "${lines[@]}"; do
        which_byat "$l"
        if [[ "$REPLY" = 1 ]]; then
            if [[ "${l#*/}" = ${~filters[1-ByAt]} ]]; then
                out+=( $match[3] );
            fi
        elif [[ "$REPLY" = 2 ]]; then
            if [[ "${l#*/}" = ${~filters[2-ByAt]} ]]; then
                out+=( $match[3] );
            fi
        elif [[ "$REPLY" = 3 ]]; then
            if [[ "${l#*/}" = ${~filters[3-ByAt]} ]]; then
                out+=( $match[3] );
            fi
        fi
    done

    reply+=( "${out[@]}" )
}

test_stack_trace() {
    local -a stacktrace cur_errors
    integer idx ssize
    local error var_name

    stacktrace=( "${(Oa)@}" )

    for (( idx = 0; idx <= 10; idx ++ )); do
        var_name="errors$idx"
        cur_errors=( "${(PA@)var_name}" )
        [[ -z "$cur_errors[1]" ]] && continue
        if [[ "${#cur_errors}" -gt 0 ]]; then
            for error in "${cur_errors[@]}"; do
                mdebug_mode && print "Processing error: $error"
                if compare_error "$error" "${stacktrace[@]}"; then
                    # Error matched stack trace, result is false
                    # i.e. skip displaying the block
                    REPLY="$error"
                    return 1;
                fi
            done
        fi
    done

    # No error matched, return true
    return 0;
}

compare_error() {
    local error="$1"
    shift

    local -a stacktrace
    stacktrace=( "$@" )
    ssize=${#stacktrace}

    local -a parts
    parts=( "${(@s:/:)error}" )
    parts=( "${parts[@]//[[:blank:]]##/}" )

    integer stack_idx=1
    local part mode="exact"
    for part in "${parts[@]}"; do
        (( stack_idx > ssize )) && { stack_idx=-1; break; }
        if [[ "$part" = "*" ]]; then
            mode="skip"
            continue
        fi
        if [[ "$mode" = "skip" ]]; then
            while [[ "${stacktrace[stack_idx]}" != "$part" ]]; do
                mdebug_mode && print "Looking for \`$part', skipping (in valgrind stack trace): \`${stacktrace[stack_idx]}'"
                stack_idx+=1
                (( stack_idx > ssize )) && break
            done

            if (( stack_idx > ssize )); then
                mdebug_mode && print "Failed to match element \`$part' (after \`*' in error definition)"
                # Failed to match error-element after "*"
                return 1;
            fi

            # Found, move to next stack element, continue to next error-part
            mdebug_mode && print "Found \`$part', moving to next (in valgrind stack trace): \`${stacktrace[stack_idx+1]}'"
            stack_idx+=1
            mode="exact"
            continue
        elif [[ "$mode" = "exact" ]]; then
            if [[ "${stacktrace[stack_idx]}" != "$part" ]]; then
                mdebug_mode && print "Failed to match \`$part' (vs. valgrind stack trace element: \`${stacktrace[stack_idx]})'"
                # Failed to match error-element
                return 1;
            fi

            mdebug_mode && print "Matched $part (vs. valgrind stack trace: ${stacktrace[stack_idx]})"
            # Matched current error-part, move to next stack
            # trace element, continue to next part
            stack_idx+=1
            mode="exact"
            continue
        fi
    done

    if (( stack_idx == -1 )); then
        mdebug_mode && print "Valgrind stack trace finished first (too many error elements, no match)"
        # Had error-part to test but stack trace ended -> no match
        return 1;
    fi

    if (( stack_idx > ssize )); then
        mdebug_mode && print "Processed whole valgrind stack, used all error-parts - a match"
        # Processed whole stack trace and error-parts
        # have been fully used - match, return true
        return 0
    fi

    if (( stack_idx <= ssize )); then
        # Error-parts ended before reaching end of
        # stack trace -> in order to match, last part
        # must be "*", i.e. skip mode
        if [[ "$mode" = "skip" ]]; then
            mdebug_mode && print "Final \`*' in error definition - a match"
            # Match, return true
            return 0;
        else
            mdebug_mode && print "Stack trace was longer than error-parts, no match"
            return 1;
        fi
    fi

    # Unreachable
    return 0;
}

show_block()
{
    local line MATCH
    integer MBEGIN MEND

    for line; do
        if [[ "$line" = "1-ByAt/"* ]]; then
            if [[ "${line#*/}" = ${~filters[1-ByAt]} ]]; then
                print "${theme[pid]}${match[1]}${theme[byat]}${match[2]}${theme[func]}${match[3]}${theme[symbol]} (${theme[where_file]}${match[4]/:/${theme[symbol]}:${theme[number]}}${theme[symbol]})${theme[rst]}"
            fi
        elif [[ "$line" = "2-ByAt/"* ]]; then
            if [[ "${line#*/}" = ${~filters[2-ByAt]} ]]; then
                print "${theme[pid]}${match[1]}${theme[byat]}${match[2]}${theme[func]}${match[3]}${theme[symbol]} (${theme[rst]}${match[4]} ${theme[where_path]}${match[5]}${theme[symbol]})${theme[rst]}"
            fi
        elif [[ "$line" = "3-ByAt/"* ]]; then
            if [[ "${line#*/}" = ${~filters[3-ByAt]} ]]; then
                print "${theme[pid]}${match[1]}${theme[byat]}${match[2]}${theme[func]}${match[3]}${theme[rst]}"
            fi
        elif [[ "$line" = "4-Summary/"* ]]; then
            if [[ "${line#*/}" = ${~filters[4-Summary]} ]]; then
                if [[ "${match[2]}" = [A-Z[:blank:]]##[:] ]]; then
                    match[3]="${match[3]//(#m) [0-9,]## /${theme[number]}$MATCH${theme[rst]}}"
                    print "${theme[pid]}${match[1]}${theme[summary_header]}${match[2]}${theme[rst]}${match[3]}"
                else
                    match[3]="${match[3]//(#m) [0-9,]## /${theme[number]}$MATCH${theme[rst]}}"
                    print "${theme[pid]}${match[1]}${theme[summary_body]}${match[2]}${theme[rst]}${match[3]}"
                fi
            fi
        elif [[ "$line" = "5-Error/"* ]]; then
            if [[ "${line#*/}" = ${~filters[5-Error]} ]]; then
                print "${theme[pid]}${match[1]}${theme[error]}${match[2]}${theme[rst]}"
            fi
        elif [[ "$line" = "6-Info/"* ]]; then
            if [[ "${line#*/}" = ${~filters[6-Info]} ]]; then
                print "${theme[pid]}${match[1]}${theme[info]}${match[2]}${theme[rst]}"
            fi
        elif [[ "$line" = "7-Blank/"* ]]; then
            print "${theme[pid]}${line#*/}${theme[rst]}"
        fi
    done
}

#
# Main code
#

coproc 2>&1 valgrind "$@"

integer count=0
local matched prev_matched blank_seen=0
local -a block umblock

while read -p line; do
    matched=""
    for key in ${(onk)filters[@]}; do
        pat=${filters[$key]}
        if [[ "$line" = $~pat ]]; then
            if [[ $key = *Error && ( $line = $~reachable_pat || "$blank_seen" -eq "0" ) ]]; then
                key="6-Info"
            fi

            if [[ "$key" = *Blank ]]; then
                blank_seen=1
                process_block $block
                block=( "${key}/$line" )
            elif [[ "$prev_matched" = *Summary && "$key" = *Error ]]; then
                process_block $block
                block=( "${key}/$line" )
            else
                block+=( "${key}/$line" )
            fi

            if [[ "$prev_matched" = *Unmatched ]]; then
                process_umblock $umblock
                umblock=()
            fi

            matched="$key"
            break
        fi
    done
    if [[ -z "$matched" ]]; then
        matched="Unmatched"
        umblock+=( "$line" )
    fi

    prev_matched="$matched"
done
