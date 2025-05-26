#!/bin/sh

SRC_DIR="src"
INFILES="$SRC_DIR/dr2osm.c"
LIBS="-lsqlite3 -lproj"

# Default flags
CFLAGS="-pg"
LDFLAGS=""

# Detect platform
UNAME=$(uname)

if [ "$UNAME" = "Darwin" ]; then
    # macOS (Homebrew)
    PROJ_PREFIX=$(brew --prefix proj)
    CFLAGS="$CFLAGS -I$PROJ_PREFIX/include"
    LDFLAGS="$LDFLAGS -L$PROJ_PREFIX/lib"
elif [ "$UNAME" = "Linux" ]; then
    # Linux: PROJ is usually in standard paths
    :
fi

# Release flags
if [ "$1" = "release" ]; then
    CFLAGS="-O3 -DRELEASE_BUILD $CFLAGS"
fi

# Compile
cc $CFLAGS $LDFLAGS -o dr2osm $INFILES $LIBS
