#!/bin/bash

set -e

export LC_ALL=C
export LANG=C

SAVEIFS=$IFS
IFS=$(echo -en "\n\b")
: ${1?"Usage: $0 ARGUMENT"}
if ! [ -d $1 ] ; then
  echo "You must give a directory as an argument!"
  exit
fi

for f in `find $1 -type f |grep \.discoverer-expected`
do
  echo "Removing $f ..."
  rm $f
done
IFS=$SAVEIFS
