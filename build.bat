@echo off
pushd %~dp0

set osgeopath=C:\OSGeo4W
set includepath=%osgeopath%\include
set libpath=%osgeopath%\lib

set CFLAGS=/nologo /Z7 /I%includepath%
set INFILES=dr2osm.c
set LIBS=sqlite3_i.lib proj.lib
set LDFLAGS=/incremental:no /subsystem:console /libpath:%libpath%

cl %CFLAGS% /Fedr2osm.exe %INFILES% %LIBS% /link %LDFLAGS%

popd
