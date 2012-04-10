#!/bin/bash

set -e

#We only parse tags in english...
export LC_ALL=C
export LANG=C

SAVEIFS=$IFS
IFS=$(echo -en "\n\b")
: ${1?"Usage: $0 ARGUMENT"}
if ! [ -d $1 ] ; then
  echo "You must give a directory as an argument!"
  exit
fi

for f in `find $1 -type f`
do
  echo "Tagging $f ..."
  gst-discoverer-0.10 -v $f > $f.discoverer-expected
done
IFS=$SAVEIFS
