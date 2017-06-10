#!/bin/zsh -f

emulate zsh

# Run all specified tests, keeping count of which succeeded.
# The reason for this extra layer above the test script is to
# protect from catastrophic failure of an individual test.
# We could probably do that with subshells instead.

[[ -f "test_type" ]] && local tpe=$(<test_type) || local tpe="2"
local cmd="valgrind"
(( ${+commands[colour-valgrind]} )) && cmd="colour-valgrind"

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
