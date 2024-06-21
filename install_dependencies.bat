@echo off

set root=%USERPROFILE%\OSGeo4W
set site=https://download.osgeo.org/osgeo4w/v2

curl --silent "%site%/osgeo4w-setup.exe" --output osgeo4w-setup.exe
osgeo4w-setup.exe ^
	--local-package-dir "%TEMP%" ^
	--no-desktop ^
	--no-shortcuts ^
	--packages sqlite3-devel,proj-devel ^
	--quiet-mode ^
	--root "%root%" ^
	--site "%site%"
del osgeo4w-setup.exe
