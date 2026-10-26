#!/bin/sh
# Builds the pools written in PoolEdit XML: spreader.xml to spreader.iop and spreader_ids.hpp.
# Needs PoolEditParser (GPL, github.com/moehman/PoolEditParser), built with
#   g++ pooleditparser.cxx xml.cxx parser.cxx -o pooleditparser -lexpat
# and named by POOLEDITPARSER or found on PATH. It is a build tool only: nothing of it is in
# this repository or in the release tarballs, and the .iop files it writes are committed.
set -e
here=$(cd "$(dirname "$0")" && pwd)
parser=${POOLEDITPARSER:-pooleditparser}
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
for pool in spreader; do
	python3 "$here/../tools/pool_export.py" "$here/$pool.xml" "$work/$pool.xml" "$here/${pool}_ids.hpp"
	# Drawn for 480 by 480 data masks and 60 by 60 soft keys: no scaling
	"$parser" "$work/$pool.xml" "$here/$pool.iop" -d=480 -sw=60 -sh=60 > "$work/log"
	grep -E 'ERROR|generated pool size|number of objects' "$work/log" || true
	if grep -q ERROR "$work/log"; then
		echo "$pool: the parser reported errors" >&2
		exit 1
	fi
done
