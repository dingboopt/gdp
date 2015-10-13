#!/bin/bash

gdp=../../../
PTII=$gdp/../ptII

#### GDP compilation/setup

# Build GDP
cd $gdp
make

# Build Debian packages
./deb-pkg/package-client.sh 0.3-1
./lang/python/deb-pkg/package.sh 0.3-1

# install debian packages
sudo dpkg -r python-gdp gdp-client
sudo dpkg -i $gdp/gdp-client_0.3-1_amd64.deb
sudo dpkg -i $gdp/python-gdp_0.3-1_all.deb
sudo apt-get -f install

# make java stuff
cd $gdp/lang/java
make
jar cf GDP-0.3-1.jar org/terraswarm/gdp/*.class

#### End GDP Compilation/setup



# Download ptolemy source code
svn co -r 73599 https://repo.eecs.berkeley.edu/svn-anon/projects/eal/ptII/trunk $PTII

# add .jar and .so files to $PTII/lib
cp $gdp/lang/java/*.jar $PTII/lib/
cp $gdp/libs/libgdp.0.2.so $PTII/lib/

# copy GDP.module appropriately
cp -a $gdp/demo/201510/accessor/GDP.module $PTII/ptolemy/actor/lib/jjs/modules/GDP

# Apply patch
cd $PTII
patch -p0 -i $gdp/demo/201510/accessor/configfiles.patch

# make a symlink to the accessor/demo
ln -s $gdp/demo/201510/accessor/GDP.accessor
ln -s $gdp/demo/201510/accessor/demo

# Now compile ptolemy
autoconf
./configure
ant
cd bin
make

