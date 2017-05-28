#!/bin/zsh -f

emulate zsh

# Run all specified tests, keeping count of which succeeded.
# The reason for this extra layer above the test script is to
# protect from catastrophic failure of an individual test.
# We could probably do that with subshells instead.

integer success failure skipped retval
for file in "${(f)ZTST_testlist}"; do
  if (( ${+commands[colour-valgrind]} )); then
      if [[ 3 -nt 2 && 3 -nt 1 ]]; then
          colour-valgrind --leak-check=full --show-possibly-lost=no $ZTST_exe +Z -f $ZTST_srcdir/ztst.zsh $file
      elif [[ 2 -nt 1 ]]; then
          colour-valgrind $ZTST_exe +Z -f $ZTST_srcdir/ztst.zsh $file
      else
          colour-valgrind --leak-check=full $ZTST_exe +Z -f $ZTST_srcdir/ztst.zsh $file
      fi
  else
      valgrind --leak-check=full $ZTST_exe +Z -f $ZTST_srcdir/ztst.zsh $file
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
