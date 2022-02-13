#!/bin/zsh -f
# The line above is just for convenience.  Normally tests will be run using
# a specified version of zsh.  With dynamic loading, any required libraries
# must already have been installed in that case.
#
# Takes one argument: the name of the test file.  Currently only one such
# file will be processed each time ztst.zsh is run.  This is slower, but
# much safer in terms of preserving the correct status.
# To avoid namespace pollution, all functions and parameters used
# only by the script begin with VATS_.
#
# Options (without arguments) may precede the test file argument; these
# are interpreted as shell options to set.  -x is probably the most useful.

# Produce verbose messages if non-zero.
# If 1, produce reports of tests executed; if 2, also report on progress.
# Defined in such a way that any value from the environment is used.
: ${VATS_verbose:=0}

# We require all options to be reset, not just emulation options.
# Unfortunately, due to the crud which may be in /etc/zshenv this might
# still not be good enough.  Maybe we should trick it somehow.
emulate -R zsh

# Ensure the locale does not screw up sorting.  Don't supply a locale
# unless there's one set, to minimise problems.
[[ -n $LC_ALL ]] && LC_ALL=C
[[ -n $LC_COLLATE ]] && LC_COLLATE=C
[[ -n $LC_NUMERIC ]] && LC_NUMERIC=C
[[ -n $LC_MESSAGES ]] && LC_MESSAGES=C
[[ -n $LANG ]] && LANG=C

# Don't propagate variables that are set by default in the shell.
typeset +x WORDCHARS

# Set the module load path to correspond to this build of zsh.
# This Modules directory should have been created by "make check".
[[ -d Modules/zsh ]] && module_path=( $PWD/Modules )
# Allow this to be passed down.
export MODULE_PATH

# We need to be able to save and restore the options used in the test.
# We use the $options variable of the parameter module for this.
zmodload zsh/parameter

# Note that both the following are regular arrays, since we only use them
# in whole array assignments to/from $options.
# Options set in test code (i.e. by default all standard options)
VATS_testopts=(${(kv)options})

setopt extendedglob nonomatch
while [[ $1 = [-+]* ]]; do
  set $1
  shift
done
# Options set in main script
VATS_mainopts=(${(kv)options})

# We run in the current directory, so remember it.
VATS_testdir=$PWD
VATS_testname=$1

integer VATS_testfailed

# This is POSIX nonsense.  Because of the vague feeling someone, somewhere
# may one day need to examine the arguments of "tail" using a standard
# option parser, every Unix user in the world is expected to switch
# to using "tail -n NUM" instead of "tail -NUM".  Older versions of
# tail don't support this.
tail() {
  emulate -L zsh

  if [[ -z $TAIL_SUPPORTS_MINUS_N ]]; then
    local test
    test=$(echo "foo\nbar" | command tail -n 1 2>/dev/null)
    if [[ $test = bar ]]; then
      TAIL_SUPPORTS_MINUS_N=1
    else
      TAIL_SUPPORTS_MINUS_N=0
    fi
  fi

  integer argi=${argv[(i)-<->]}

  if [[ $argi -le $# && $TAIL_SUPPORTS_MINUS_N = 1 ]]; then
    argv[$argi]=(-n ${argv[$argi][2,-1]})
  fi

  command tail "$argv[@]"
}

# The source directory is not necessarily the current directory,
# but if $0 doesn't contain a `/' assume it is.
if [[ $0 = */* ]]; then
  VATS_srcdir=${0%/*}
else
  VATS_srcdir=$PWD
fi
[[ $VATS_srcdir = /* ]] || VATS_srcdir="$VATS_testdir/$VATS_srcdir"

# Set the function autoload paths to correspond to this build of zsh.
fpath=( $VATS_srcdir/../Functions/*~*/CVS(/)
        $VATS_srcdir/../Completion
        $VATS_srcdir/../Completion/*/*~*/CVS(/) )

: ${TMPPREFIX:=/tmp/zsh}
VATS_tmp=${TMPPREFIX}.ztst.$$
if ! rm -f $VATS_tmp || ! mkdir -p $VATS_tmp || ! chmod go-w $VATS_tmp; then
  print "Can't create $VATS_tmp for exclusive use." >&2
  exit 1
fi
# Temporary files for redirection inside tests.
VATS_in=${VATS_tmp}/ztst.in
# hold the expected output
VATS_out=${VATS_tmp}/ztst.out
VATS_err=${VATS_tmp}/ztst.err
# hold the actual output from the test
VATS_tout=${VATS_tmp}/ztst.tout
VATS_terr=${VATS_tmp}/ztst.terr

VATS_cleanup() {
  cd $VATS_testdir
  rm -rf $VATS_testdir/dummy.tmp $VATS_testdir/*.tmp(N) ${VATS_tmp}
}

# This cleanup always gets performed, even if we abort.  Later,
# we should try and arrange that any test-specific cleanup
# always gets called as well.
##trap 'print cleaning up...
##VATS_cleanup' INT QUIT TERM
# Make sure it's clean now.
rm -rf dummy.tmp *.tmp

# Report failure.  Note that all output regarding the tests goes to stdout.
# That saves an unpleasant mixture of stdout and stderr to sort out.
VATS_testfailed() {
  print -r "Test $VATS_testname failed: $1"
  if [[ -n $VATS_message ]]; then
    print -r "Was testing: $VATS_message"
  fi
  print -r "$VATS_testname: test failed."
  if [[ -n $VATS_failmsg ]]; then
    print -r "The following may (or may not) help identifying the cause:
$VATS_failmsg"
  fi
  VATS_testfailed=1
  return 1
}

# Print messages if $VATS_verbose is non-empty
VATS_verbose() {
  local lev=$1
  shift
  if [[ -n $VATS_verbose && $VATS_verbose -ge $lev ]]; then
    print -r -u $VATS_fd -- $*
  fi
}
VATS_hashmark() {
  if [[ VATS_verbose -le 0 && -t $VATS_fd ]]; then
    print -n -u$VATS_fd -- ${(pl:SECONDS::\#::\#\r:)}
  fi
  (( SECONDS > COLUMNS+1 && (SECONDS -= COLUMNS) ))
}

if [[ ! -r $VATS_testname ]]; then
  VATS_testfailed "can't read test file."
  exit 1
fi

exec {VATS_fd}>&1
exec {VATS_input}<$VATS_testname

# The current line read from the test file.
VATS_curline=''
# The current section being run
VATS_cursect=''

# Get a new input line.  Don't mangle spaces; set IFS locally to empty.
# We shall skip comments at this level.
VATS_getline() {
  local IFS=
  while true; do
    read -u $VATS_input -r VATS_curline || return 1
    [[ $VATS_curline == \#* ]] || return 0
  done
}

# Get the name of the section.  It may already have been read into
# $curline, or we may have to skip some initial comments to find it.
# If argument present, it's OK to skip the reset of the current section,
# so no error if we find garbage.
VATS_getsect() {
  local match mbegin mend

  while [[ $VATS_curline != '%'(#b)([[:alnum:]]##)* ]]; do
    VATS_getline || return 1
    [[ $VATS_curline = [[:blank:]]# ]] && continue
    if [[ $# -eq 0 && $VATS_curline != '%'[[:alnum:]]##* ]]; then
      VATS_testfailed "bad line found before or after section:
$VATS_curline"
      exit 1
    fi
  done
  # have the next line ready waiting
  VATS_getline
  VATS_cursect=${match[1]}
  VATS_verbose 2 "VATS_getsect: read section name: $VATS_cursect"
  return 0
}

# Read in an indented code chunk for execution
VATS_getchunk() {
  # Code chunks are always separated by blank lines or the
  # end of a section, so if we already have a piece of code,
  # we keep it.  Currently that shouldn't actually happen.
  VATS_code=''
  # First find the chunk.
  while [[ $VATS_curline = [[:blank:]]# ]]; do
    VATS_getline || break
  done
  while [[ $VATS_curline = [[:blank:]]##[^[:blank:]]* ]]; do
    VATS_code="${VATS_code:+${VATS_code}
}${VATS_curline}"
    VATS_getline || break
  done
  VATS_verbose 2 "VATS_getchunk: read code chunk:
$VATS_code"
  [[ -n $VATS_code ]]
}

# Read in a piece for redirection.
VATS_getredir() {
  local char=${VATS_curline[1]} fn
  VATS_redir=${VATS_curline[2,-1]}
  while VATS_getline; do
    [[ $VATS_curline[1] = $char ]] || break
    VATS_redir="${VATS_redir}
${VATS_curline[2,-1]}"
  done
  VATS_verbose 2 "VATS_getredir: read redir for '$char':
$VATS_redir"

  case $char in
    ('<') fn=$VATS_in
    ;;
    ('>') fn=$VATS_out
    ;;
    ('?') fn=$VATS_err
    ;;
    (*)  VATS_testfailed "bad redir operator: $char"
    return 1
    ;;
  esac
  if [[ $VATS_flags = *q* && $char = '<' ]]; then
    # delay substituting output until variables are set
    print -r -- "${(e)VATS_redir}" >>$fn
  else
    print -r -- "$VATS_redir" >>$fn
  fi

  return 0
}

# Execute an indented chunk.  Redirections will already have
# been set up, but we need to handle the options.
VATS_execchunk() {
  setopt localloops # don't let continue & break propagate out
  options=($VATS_testopts)
  () {
      unsetopt localloops
      eval "$VATS_code"
  }
  VATS_status=$?
  # careful... ksh_arrays may be in effect.
  VATS_testopts=(${(kv)options[*]})
  options=(${VATS_mainopts[*]})
  VATS_verbose 2 "VATS_execchunk: status $VATS_status"
  return $VATS_status
}

# Functions for preparation and cleaning.
# When cleaning up (non-zero string argument), we ignore status.
VATS_prepclean() {
  # Execute indented code chunks.
  while VATS_getchunk; do
    VATS_execchunk >/dev/null || [[ -n $1 ]] || {
      [[ -n "$VATS_unimplemented" ]] ||
      VATS_testfailed "non-zero status from preparation code:
$VATS_code" && return 0
    }
  done
}

# diff wrapper
VATS_diff() {
  emulate -L zsh
  setopt extendedglob

  local diff_out
  integer diff_pat diff_ret

  case $1 in
    (p)
    diff_pat=1
    ;;

    (d)
    ;;

    (*)
    print "Bad VATS_diff code: d for diff, p for pattern match"
    ;;
  esac
  shift

  if (( diff_pat )); then
    local -a diff_lines1 diff_lines2
    integer failed i

    diff_lines1=("${(f)$(<$argv[-2])}")
    diff_lines2=("${(f)$(<$argv[-1])}")
    if (( ${#diff_lines1} != ${#diff_lines2} )); then
      failed=1
    else
      for (( i = 1; i <= ${#diff_lines1}; i++ )); do
	if [[ ${diff_lines2[i]} != ${~diff_lines1[i]} ]]; then
	  failed=1
	  break
	fi
      done
    fi
    if (( failed )); then
      print -rl "Pattern match failed:" \<${^diff_lines1} \>${^diff_lines2}
      diff_ret=1
    fi
  else
    diff_out=$(diff "$@")
    diff_ret="$?"
    if [[ "$diff_ret" != "0" ]]; then
      print -r -- "$diff_out"
    fi
  fi

  return "$diff_ret"
}

VATS_test() {
  local last match mbegin mend found substlines
  local diff_out diff_err
  local VATS_skip

  while true; do
    rm -f $VATS_in $VATS_out $VATS_err
    touch $VATS_in $VATS_out $VATS_err
    VATS_message=''
    VATS_failmsg=''
    found=0
    diff_out=d
    diff_err=d

    VATS_verbose 2 "VATS_test: looking for new test"

    while true; do
      VATS_verbose 2 "VATS_test: examining line:
$VATS_curline"
      case $VATS_curline in
	(%*) if [[ $found = 0 ]]; then
	      break 2
	    else
	      last=1
	      break
	    fi
	    ;;
	([[:space:]]#)
	    if [[ $found = 0 ]]; then
	      VATS_getline || break 2
	      continue
	    else
	      break
	    fi
	    ;;
	([[:space:]]##[^[:space:]]*) VATS_getchunk
	  if [[ $VATS_curline == (#b)([-0-9]##)([[:alpha:]]#)(:*)# ]]; then
	    VATS_xstatus=$match[1]
	    VATS_flags=$match[2]
	    VATS_message=${match[3]:+${match[3][2,-1]}}
	  else
	    VATS_testfailed "expecting test status at:
$VATS_curline"
	    return 1
	  fi
	  VATS_getline
	  found=1
	  ;;
	('<'*) VATS_getredir || return 1
	  found=1
	  ;;
	('*>'*)
	  VATS_curline=${VATS_curline[2,-1]}
	  diff_out=p
	  ;&
	('>'*)
	  VATS_getredir || return 1
	  found=1
	  ;;
	('*?'*)
	  VATS_curline=${VATS_curline[2,-1]}
	  diff_err=p
	  ;&
	('?'*)
	  VATS_getredir || return 1
	  found=1
	  ;;
	('F:'*) VATS_failmsg="${VATS_failmsg:+${VATS_failmsg}
}  ${VATS_curline[3,-1]}"
	  VATS_getline
	  found=1
          ;;
	(*) VATS_testfailed "bad line in test block:
$VATS_curline"
	  return 1
          ;;
      esac
    done

    # If we found some code to execute...
    if [[ -n $VATS_code ]]; then
      VATS_hashmark
      VATS_verbose 1 "Running test: $VATS_message"
      VATS_verbose 2 "VATS_test: expecting status: $VATS_xstatus"
      VATS_verbose 2 "Input: $VATS_in, output: $VATS_out, error: $VATS_terr"

      VATS_execchunk <$VATS_in >$VATS_tout 2>$VATS_terr

      if [[ -n $VATS_skip ]]; then
	VATS_verbose 0 "Test case skipped: $VATS_skip"
	VATS_skip=
	if [[ -n $last ]]; then
	  break
	else
	  continue
	fi
      fi

      # First check we got the right status, if specified.
      if [[ $VATS_xstatus != - && $VATS_xstatus != $VATS_status ]]; then
	VATS_testfailed "bad status $VATS_status, expected $VATS_xstatus from:
$VATS_code${$(<$VATS_terr):+
Error output:
$(<$VATS_terr)}"
	return 1
      fi

      VATS_verbose 2 "VATS_test: test produced standard output:
$(<$VATS_tout)
VATS_test: and standard error:
$(<$VATS_terr)"

      # Now check output and error.
      if [[ $VATS_flags = *q* && -s $VATS_out ]]; then
	substlines="$(<$VATS_out)"
	rm -rf $VATS_out
	print -r -- "${(e)substlines}" >$VATS_out
      fi
      if [[ $VATS_flags != *d* ]] && ! VATS_diff $diff_out -u $VATS_out $VATS_tout; then
	VATS_testfailed "output differs from expected as shown above for:
$VATS_code${$(<$VATS_terr):+
Error output:
$(<$VATS_terr)}"
	return 1
      fi
      if [[ $VATS_flags = *q* && -s $VATS_err ]]; then
	substlines="$(<$VATS_err)"
	rm -rf $VATS_err
	print -r -- "${(e)substlines}" >$VATS_err
      fi
      if [[ $VATS_flags != *D* ]] && ! VATS_diff $diff_err -u $VATS_err $VATS_terr; then
	VATS_testfailed "error output differs from expected as shown above for:
$VATS_code"
	return 1
      fi
    fi
    VATS_verbose 1 "Test successful."
    [[ -n $last ]] && break
  done

  VATS_verbose 2 "VATS_test: all tests successful"

  # reset message to keep VATS_testfailed output correct
  VATS_message=''
}


# Remember which sections we've done.
typeset -A VATS_sects
VATS_sects=(prep 0 test 0 clean 0)

print "$VATS_testname: starting."

# Now go through all the different sections until the end.
# prep section may set VATS_unimplemented, in this case the actual
# tests will be skipped
VATS_skipok=
VATS_unimplemented=
while [[ -z "$VATS_unimplemented" ]] && VATS_getsect $VATS_skipok; do
  case $VATS_cursect in
    (prep) if (( ${VATS_sects[prep]} + ${VATS_sects[test]} + \
	        ${VATS_sects[clean]} )); then
	    VATS_testfailed "\`prep' section must come first"
            exit 1
	  fi
	  VATS_prepclean
	  VATS_sects[prep]=1
	  ;;
    (test)
	  if (( ${VATS_sects[test]} + ${VATS_sects[clean]} )); then
	    VATS_testfailed "bad placement of \`test' section"
	    exit 1
	  fi
	  # careful here: we can't execute VATS_test before || or &&
	  # because that affects the behaviour of traps in the tests.
	  VATS_test
	  (( $? )) && VATS_skipok=1
	  VATS_sects[test]=1
	  ;;
    (clean)
	   if (( ${VATS_sects[test]} == 0 || ${VATS_sects[clean]} )); then
	     VATS_testfailed "bad use of \`clean' section"
	   else
	     VATS_prepclean 1
	     VATS_sects[clean]=1
	   fi
	   VATS_skipok=
	   ;;
    *) VATS_testfailed "bad section name: $VATS_cursect"
       ;;
  esac
done

if [[ -n "$VATS_unimplemented" ]]; then
  print "$VATS_testname: skipped ($VATS_unimplemented)"
  VATS_testfailed=2
elif (( ! $VATS_testfailed )); then
  print "$VATS_testname: all tests successful."
fi
VATS_cleanup
exit $(( VATS_testfailed ))
