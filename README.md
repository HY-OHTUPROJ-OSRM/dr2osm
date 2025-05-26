# dr2osm
Command line utility to convert Digiroad K-material geopackage to an OSM format compatible with OSRM. Get the Digiroad material at https://ava.vaylapilvi.fi/ava/Tie/Digiroad/Aineistojulkaisut. You can get the latest one at https://ava.vaylapilvi.fi/ava/Tie/Digiroad/Aineistojulkaisut/latest/KokoSuomi_DIGIROAD_R.zip

The program has a wiki page with some other information here:
https://github.com/HY-OHTUPROJ-OSRM/osrm-project/wiki/Digiroad-to-OSM-transformation

### Dependencies
- libsqlite3
- libproj

#### Installing dependencies on MacOS
Install the PROJ library

	brew install proj

### Building

#### Building on Linux/MacOS
Compile the program

	./build.sh release

This outputs a single executable file called `dr2osm`.

#### Building on Windows
These instructions assume that you are using Microsoft's C compiler and build
tools and use the provided script for installing dependencies via OSGeo4W.
Otherwise make the necessary adjustments.

The script `install_dependencies.bat` automatically installs the required
libraries. By default it creates an install directory called `OSGeo4W` in your
home directory, but this can be changed by editing the variable named `root` in
the install script. After running the script, the install directory should
have a subdirectory named `bin`. Add this directory to your path, so that
Windows can find the installed libraries.

The script `build.bat` compiles the program. If you changed the install
directory in the previous step, change the `osgeopath` variable in the build
script to reflect this. Then, in a developer command prompt, run

	build.bat release

to compile the program. This outputs a single executable file named
`dr2osm.exe`.

### Usage
The program takes two arguments: first the name of the geopackage file used as
input and second the name of the OSM file to be written. For example:

	dr2osm KokoSuomi_Digiroad_K_GeoPackage.gpkg route-data.osm

The following commandline can be used to modify the behavior of the program.
If present, they must precede the input argument.

	--mml-iceroads <ice-road-path>	Includes ice roads from MML ice road
					data in the output.
	--default-speed-limits		For ways without a specified speed
					limit in the source data, enters a
					default speed limit based on the type
					of way in question.

#### Fix big size on MacOS

	brew install osmium-tool
	osmium cat route-data.osm -o route-data.pbf
