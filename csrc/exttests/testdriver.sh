# exttests/testdriver.sh
# 
# Copyright (C) 2009, 2012 Palo Alto Research Center, Inc.
#
# This work is free software; you can redistribute it and/or modify it under
# the terms of the GNU General Public License version 2 as published by the
# Free Software Foundation.
# This work is distributed in the hope that it will be useful, but WITHOUT ANY
# WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
# FOR A PARTICULAR PURPOSE.
#
#
# This orchestrates the execution of various test scripts
#
# Test scripts should live in the same directory as this script
# and should have whitespace-free names matching test_*
# These are coded using Bourne shell syntax.
#
# Stylized commands of the form
# AFTER : test_foo test_bar
# BEFORE : test_baz
# are used to order the tests, where there are dependencies.
# Naturally, independent tests are to be preferred where possible.

# Run out of our own subdirectory
cd $(dirname "$0")

# Set up PATH so the tested programs are used, rather than any that
# might be installed.
X=..
export PATH=.:../ccnr:../sync:$X/ccnd:$X/libexec:$X/cmd:$X/lib:$X/util:../../bin:$PATH:./stubs
type ccnd
type ccnr
type ccncat
type jot
type SyncTest

# If there are any ccnds running on test ports, wait a minute and retry.
TestBusy () {
  (. settings; ls ${CCN_LOCAL_SOCKNAME:-/tmp/.ccnd.sock}.$((CCN_LOCAL_PORT_BASE/10))[01234] 2>/dev/null)
}
TestBusy && { echo There is something else happening, waiting one minute ... '' >&2; sleep 60; }
TestBusy && exit 1

# If we are running in a git repo, print out the git hash
test -d ../../.git && {
  git rev-parse HEAD | xargs echo ccnx at
}

# If we need to generate key pairs for the tests, make them smallish
export RSA_KEYSIZE=512

# Keep track of failed tests and skipped tests
: > FAILING
: > SKIPPED
# Also keep track of status codes
rm -rf STATUS
mkdir STATUS

GetTestNames () {
  case ${1:-x} in
     test_*)	echo "$@" test_final_teardown;;
     x)		echo test_*;;
  esac
}

ExtractDeps () {
  for i in "$@"; do
    sed -n -e 's/^AFTER : //p' $i | xargs -n 1 echo  | while read j; do test -f "$j" && echo $j $i; done
    sed -n -e 's/^BEFORE : //p' $i | xargs -n 1 echo | while read j; do test -f "$j" && echo $i $j; done
    echo $i test_final_teardown
  done
}

RunATest () {
  export TESTNAME=$1
  ( . settings; . functions; . preamble; echo === $1 >&2; . $1 )
  STATUS=$?
  echo $STATUS >&2
  case $STATUS in
    0) ;;
    9) echo $1 >> SKIPPED;;
    *) echo '***' $1 Failed >&2; echo $1 >> FAILING;;
  esac
  echo $STATUS > STATUS/$1
  return $STATUS
}

SetExitCode () {
  : | cmp -s - FAILING
}

# Construct a test arder if necessary
test -f order.txt || {
  ExtractDeps $(GetTestNames) > deps.out
  tsort deps.out > order.txt
}

ExtractDeps $(GetTestNames "$@") | xargs -n 1 | sort | uniq > unordered.out
grep -w -F -f unordered.out order.txt > planned-tests.out
xargs <planned-tests.out echo Planned: >&2
cat planned-tests.out | while read TEST; do RunATest $TEST; done

grep test_ SKIPPED FAILING
SetExitCode
