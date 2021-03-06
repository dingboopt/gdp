<!-- Use
	pandoc -sS -o README-compiling.html README-compiling.md
to process this to HTML -->

COMPILING THE GLOBAL DATAPLANE SOFTWARE
=======================================

This directory contains the source code for the Global Data Plane
(GDP) with several language bindings.  The primary binding is for
C (and will work with C++).  The others include Python, Java, and
Javascript.

NOTE: these instructions assume you are starting with the source
distribution.  This is not appropriate if you are installing the
Debian package.  Since you are reading this, you presumably already
have the source code.  If not, you can get the GDP source distribution
using one of the following commands:

	git clone git://repo.eecs.berkeley.edu/projects/swarmlab/gdp.git
	git clone https://repo.eecs.berkeley.edu/git/projects/swarmlab/gdp.git
	git clone repoman@repo.eecs.berkeley.edu:projects/swarmlab/gdp.git

The first form gives you public, read-only access, while the other
two require that you have an account on the EECS repository.
The third is only available if you have registered your public
ssh key with `repo.eecs.berkeley.edu`.

At the moment, compiles work on many platforms, including Debian,
RedHat, MacOS, and FreeBSD.  However, some other GDP-related packages
work on Debian only (which includes Ubuntu), so you may have
difficulties outside the main source tree.  See the section on
Operating System Quirks below for some hints.

Installing Requisite Packages
-----------------------------

When compiling from source code, there is no distinction between
client and server packages; both are compiled every time.  For this
reason, you must install all requisite packages before compiling.
The easiest way to do this is to run the `adm/gdp-setup.sh`
script.

Note that on some systems you may need to install the compile suite
as well.

Compilation
-----------

Compiling the primary code tree should just be a matter of typing
`make` in the root of the `gdp` tree.  If you want to clear out
old cruft, use `make clean`.  You can install the packages in the
system tree using `make install`.  If you prefer installing into
something other than the main tree, set the `LOCALROOT` variable
on the `make` command line.  For example:

	make clean install LOCALROOT=/usr/local

It is not necessary to install the code for testing and debugging.

If you are going to be debugging it can be convenient to use
`O=` on the `make` command line.  This will turn off optimization,
which makes debuggers more understandable.

Note: gcc on linux has a bug that causes it to complain about
non-constant expressions in an initializer when the `-std=c99`
flag is given.  Those same expressions are constant in Clang
and even in gcc without the `-std=c99` flag.  As a result of
this problem, we do not use the `-std=c99` flag by default, but
this means that not all features of C99 are available.
If you want full C99, use `STD=-std=c99` on the make command
line.

Further note: At least some versions of gcc give warnings
about ignored return values even when the function call has
been explicitly voided.  We know about this and do not
consider it to be a bug in the GDP code.  If these warnings
bother you we recommend installing clang and using that
compiler.  (Hint: it gives much better error messages and
catches things that gcc does not.)

### Compilation Flags

There are a few compilation flags you can use to turn on additional
debugging.  You can generally do this using `O=-D`flag on the
`make` command line; this also turns off optimization so that
debuggers can give better information.

* *EP_OPT_EXTENDED_MUTEX_CHECK* &mdash; Check for cases such as locking
  mutexes that are already locked.  Also enables some assertions
  that check lock status.  Only works on systems using the NTPL
  implementation (which means most versions of Linux).  This breaks
  information hiding and hence may fail if the NPTL implementation
  changes.
* *GDP_OPT_EXTENDED_CACHE_CHECK* &mdash; This does some additional
  checking on the GCL cache.  Notably, it fails if a loop in one of
  the linked lists is detected.  This can be expensive, so use it
  sparingly.

### Other language bindings

In addition to C, there is support for Python, Java, and
Javascript.  These bindings are all in the `lang` subtree.
See the instructions in those directories for compiling.

Operating System Quirks
-----------------------

### MacOS

If you are trying to compile on MacOS you will need to install
Xcode from the App Store to get the compilers, libraries, and
build tools you will need.  Instructions for doing this are
available at https://www.macports.org/install.php.

Other packages are installed by `adm/gdp-setup.sh`.  Note that
this script will try to determine if you are using `brew` or
`macports`.  Of the two, `macports` is better understood and
supports more required packages.  If you use `homebrew` you will
have to install some packages by hand.

To install `macports`, see https://www.macports.org/install.php.
It comes as standard binary packages for most versions: download
the `.dmg` file, double click on the package, and proceed.

To install brew, see http://brew.sh/ and run
   /usr/bin/ruby -e \"$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/master/install)\"

Unfortunately, neither of them has all the modules you may need
if you are compiling everything, so you may have to download other
packages from source code.  For example, `macports` does not
support `mosquitto`, which is necessary if you are compiling the
MQTT-GDP gateway code (which is not part of the default build).
Conversely, `brew` does not support Avahi, which _is_ part of
the default build.

If you cannot use `macports` but you want Zeroconf, you can install
Avahi by hand using:

    wget https://github.com/lathiat/avahi/releases/download/v0.6.32/avahi-0.6.32.tar.gz
    tar -zxf avahi-0.6.32.tar.gz
    cd avahi-0.6.32
   ./configure --disable-qt4 --disable-qt3 --disable-gtk \
        --disable-gtk3 --disable-gdbm --disable-pygtk \
		--disable-python-dbus --disable-mono 
    make -k

Then `cd` to the `avahi-0.6.32` directory and run:

    sudo make -k install

Note also that `0.6.32` is the current version as of this writing;
you may need to find the latest version if that one becomes obsolete.

As an alternative, you can remove Zeroconf from the compilation
entirely using:

    make all_noavahi

Be aware that this will increase the need for manual configuration
at all user sites.

If make fails because openssl/evp.h is not found, then find evp.h
with:

   find /usr/local/Cellar -name "evp.h"

For example:

   bash-3.2$ find /usr/local/Cellar -name "evp.h"
   /usr/local/Cellar/openssl/1.0.2k/include/openssl/evp.h

Use the directory above the include directory for the value
of LOCAL2:

   make LOCAL2=/usr/local/Cellar/openssl/1.0.2k all_noavahi

### Red Hat

Debian is the preferred Linux distribution for the GDP, though
a nightly build uses Red Hat.

### FreeBSD

We do attempt to compile on FreeBSD occasionally in an attempt
to promote portability, but some of the other optional packages
do not compile or run on FreeBSD.  However, the base code
should compile.

Next Steps
----------

If you just want to use the GDP client programs, continue
reading README.md

If you intend to install and maintain your own GDP routers
and/or log servers, please continue with README-admin.md.

If you plan on debugging the GDP code itself, continue with
README-developers.md.

Directory Structure
-------------------

The following is a brief explanation of the subdirectories
contained in this source tree.

* ep &mdash; A library of C utility functions.  This is a stripped
	down version of a library I wrote several years ago.
	If you look at the code you will see vestiges of some
	of the stripped out functions.  I plan on cleaning
	this version up and releasing it again.

* gdp &mdash; A library for GDP manipulation.  This is the library
	that applications must link to access the GDP.

* gdplogd &mdash; The GDP log daemon.  This implements physical
	(on disk) logs for the GDP.  The implementation is
	still fairly simplistic.  It depends on a routing
	layer (currently gdp_router, in a separate repository).

* scgilib &mdash; An updated version of the SCGI code from
	[`http://www.xamuel.com/scgilib/`](http://www.xameul.com/scgilib/).
	SCGI permits a web server to access outside programs by opening
	a socket in a manner much more efficient than basic
	CGI fork/exec.  This is only used for the REST interface.

* apps &mdash; Application programs, including tests.

* doc &mdash; Some documentation, woefully incomplete.

* examples &mdash; Some example programs, intended be usable as
	tutorials.

* lang &mdash; sub-directories with language-specific application
	programs and supporting code.

* lang/java &mdash; Java-specific apps and libraries.

* lang/js &mdash; JavaScript-specific apps and libraries.  Also contains
	the Node.js/JS GDP RESTful interface code.  See associated
	README files for details.

* lang/python &mdash; Python-specific apps and libraries.  See the
	associated README file for details.

<!-- vim: set ai sw=4 sts=4 ts=4 : -->
