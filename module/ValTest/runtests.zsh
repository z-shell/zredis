#!/bin/sh
# -*- Mode: sh; sh-indentation: 4; indent-tabs-mode: nil; sh-basic-offset: 4; -*-
# vim:ft=zsh:sw=4:sts=4:et

#
# /bin/sh stage, load configuration to obtain $zsh_bin
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
fi

[[ -z "$zsh_control_bin" ]] && zsh_control_bin="zsh"

#
#
#

[[ -z "$ZSH_VERSION" ]] && exec /usr/bin/env "$zsh_control_bin" -f -c "source \"$0\" \"$1\" \"$2\" \"$3\" \"$4\" \"$5\" \"$6\" \"$7\" \"$8\" \"$9\" \"$10\""

typeset -g ZERO="${(%):-%N}" # this gives immunity to functionargzero being unset
typeset -g ZERO_DIR="${ZERO:h}"

emulate zsh

# Run all specified tests, keeping count of which succeeded.
# The reason for this extra layer above the test script is to
# protect from catastrophic failure of an individual test.
# We could probably do that with subshells instead.

[[ -f "test_type" ]] && local tpe=$(<test_type) || local tpe="1"
local cmd="valgrind"
[[ -x "${ZERO_DIR}/zsh-valgrind-parse.cmd" ]] && cmd="${ZERO_DIR}/zsh-valgrind-parse.cmd"

integer success failure skipped retval
for file in "${(f)ZTST_testlist}"; do
  if [[ "$tpe" = "3" ]]; then
    $cmd --leak-check=full --show-possibly-lost=no $ZTST_exe +Z -f $ZTST_srcdir/ztst.zsh $file
  elif [[ "$tpe" = "2" ]]; then
    $cmd $ZTST_exe +Z -f $ZTST_srcdir/ztst.zsh $file
  else
    $cmd --leak-check=full $ZTST_exe +Z -f $ZTST_srcdir/ztst.zsh $file
  fi
  retval=$?
  if (( $retval == 2 )); then
    (( skipped++ ))
  elif (( $retval )); then
    (( failure++ ))
  else
    (( success++ ))
  fi
done
print "**************************************
$success successful test script${${success:#1}:+s}, \
$failure failure${${failure:#1}:+s}, \
$skipped skipped
**************************************"
return $(( failure ? 1 : 0 ))
