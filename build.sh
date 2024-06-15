#!/bin/sh

CFLAGS="-pg -g"
INFILES="dr2osm.c"
LIBS="-lsqlite3 -lproj"

if [ "$1" = "release" ]
then CFLAGS="-O3 -DRELEASE_BUILD"
fi

cc $CFLAGS -o dr2osm $INFILES $LIBS
