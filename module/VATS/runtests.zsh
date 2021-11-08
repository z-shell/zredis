#!/bin/sh
# -*- Mode: sh; sh-indentation: 2; indent-tabs-mode: nil; sh-basic-offset: 2; -*-
# vim:ft=zsh:sw=2:sts=2:et

#
# /bin/sh (e.g. the limited dash shell) stage, load configuration to obtain
# $zsh_control_bin and to restart the script with the configured control binary
#

SH_ZERO_DIR=${0%/zsh-valgrind-parse.cmd}

[ -z "$ZSHV_TCONF_FILE" ] && ZSHV_TCONF_FILE="vtest.conf"
[ "$1" != "${1#conf:}" ] && { ZSHV_TCONF_FILE="${1#conf:}"; shift; }

if [ -n "$ZSHV_TCONF_DIR" ]; then
  . "${ZSHV_TCONF_DIR}/${ZSHV_TCONF_FILE}"
elif [ -f "${SH_ZERO_DIR}/${ZSHV_TCONF_FILE}" ]; then
  . "${SH_ZERO_DIR}/${ZSHV_TCONF_FILE}"
elif [ -f "${PWD}/${ZSHV_TCONF_FILE}" ]; then
  . "${PWD}/${ZSHV_TCONF_FILE}"
elif [ -f "VATS/${ZSHV_TCONF_FILE}" ]; then
  . "VATS/${ZSHV_TCONF_FILE}"
fi

[ -z "$zsh_control_bin" ] && zsh_control_bin="zsh"

#
# Restart with zsh as interpreter
#

[ -z "$ZSH_VERSION" ] && exec /usr/bin/env "$zsh_control_bin" -f -c "source \"$0\" \"$1\" \"$2\" \"$3\" \"$4\" \"$5\" \"$6\" \"$7\" \"$8\" \"$9\""

#
# Init
#

typeset -g ZERO="${(%):-%N}" # this gives immunity to functionargzero being unset
typeset -g ZERO_DIR="${ZERO:h}"

emulate zsh -o warncreateglobal -o typesetsilent

autoload colors; colors

test_type_msg()
{
  print "$fg[green]@@@$reset_color Test type: $1 $fg[green]@@@$reset_color Test binary: $test_bin $fg[green]@@@$reset_color Control binary: $zsh_control_bin $ZSH_VERSION $fg[green]@@@$reset_color"
}

# Run all specified tests, keeping count of which succeeded.
# The reason for this extra layer above the test script is to
# protect from catastrophic failure of an individual test.
# We could probably do that with subshells instead.

export VATS_exe
local cmd="valgrind"
local -a valargs
[[ -x "${ZERO_DIR}/zsh-valgrind-parse.cmd" ]] && cmd="${ZERO_DIR}/zsh-valgrind-parse.cmd"
[[ "$test_bin" = "local-zsh" ]] && test_bin="${VATS_exe}"

[[ ! -f "$test_bin" ]] && { print "VATS: Test binary ($test_bin) doesn't exist, aborting"; exit 1; }

if [[ "$tkind" = nopossiblylost* ]]; then
  valargs=( "--leak-check=full" "--show-possibly-lost=no" )
  test_type_msg "leaks, nopossiblylost"
elif [[ "$tkind" = error* ]]; then
  valargs=()
  test_type_msg "only errors (no leaks)"
elif [[ "$tkind" = leak* ]]; then
  valargs=( "--leak-check=full" )
  test_type_msg "full leak check"
else
  print "VATS: Unknown test type \`$tkind\', supported are: error, leak, nopossiblylost. Aborting."
  exit 1
fi

local ctarg    # current arg
local -a targs # evaluated test_bin args, non-evaluated Valgrind args
integer success failure skipped count=0

for file in "${(f)VATS_testlist}"; do
  # Prepare test_bin-args
  targs=()
  for ctarg in "${(z@)test_bin_args}"; do
    eval "print -rl -- $ctarg | while read line; do targs+=( \"\${(Q)line}\" ); done"
  done

  (( ++ count ))

  # Invoke Valgrind (through zsh-valgrind-parse.cmd)
  $cmd "${valargs[@]}" "$test_bin" "${targs[@]}"
done

print "**************************************"
print "$count test file(s) were ran"
print "**************************************"

return 0
