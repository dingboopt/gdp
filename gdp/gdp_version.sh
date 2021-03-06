#!/bin/sh

# ----- BEGIN LICENSE BLOCK -----
#	GDP: Global Data Plane
#	From the Ubiquitous Swarm Lab, 490 Cory Hall, U.C. Berkeley.
#
#	Copyright (c) 2015-2016, Regents of the University of California.
#	All rights reserved.
#
#	Permission is hereby granted, without written agreement and without
#	license or royalty fees, to use, copy, modify, and distribute this
#	software and its documentation for any purpose, provided that the above
#	copyright notice and the following two paragraphs appear in all copies
#	of this software.
#
#	IN NO EVENT SHALL REGENTS BE LIABLE TO ANY PARTY FOR DIRECT, INDIRECT,
#	SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING LOST
#	PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS DOCUMENTATION,
#	EVEN IF REGENTS HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
#	REGENTS SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
#	LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
#	FOR A PARTICULAR PURPOSE. THE SOFTWARE AND ACCOMPANYING DOCUMENTATION,
#	IF ANY, PROVIDED HEREUNDER IS PROVIDED "AS IS". REGENTS HAS NO
#	OBLIGATION TO PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS,
#	OR MODIFICATIONS.
# ----- END LICENSE BLOCK -----

major=$1
minor=$2
patch=$3

#
#  Create a new gdp_version.h file from template
#

{
	echo "// DO NOT EDIT THIS FILE.  IT IS AUTOGENERATED."
	sed	-e "s/@GDP_VERSION_MAJOR@/$major/" \
		-e "s/@GDP_VERSION_MINOR@/$minor/" \
		-e "s/@GDP_VERSION_PATCH@/$patch/" \
		gdp_version.template
	echo "// DO NOT EDIT THIS FILE.  IT IS AUTOGENERATED."
} > _gdp_version_$$

if [ ! -e gdp_version.h ] || ! cmp -s gdp_version.h _gdp_version_$$
then
	test -e gdp_version.h && diff gdp_version.h _gdp_version_$$
	cp _gdp_version_$$ gdp_version.h
fi

rm _gdp_version_$$

#
#  While we are at it, create an administrative script that can
#  be used by other modules.
#

{
	echo "# DO NOT EDIT THIS FILE.  IT IS AUTOGENERATED."
	echo sed -e "'s/@WARNING@/DO NOT EDIT THIS FILE.  IT IS AUTOGENERATED./' \\"
	echo "	-e 's/@GDP_VERSION_MAJOR@/$major/' \\"
	echo "	-e 's/@GDP_VERSION_MINOR@/$minor/' \\"
	echo "	-e 's/@GDP_VERSION_PATCH@/$patch/'"

	echo "# DO NOT EDIT THIS FILE.  IT IS AUTOGENERATED."
} > ../adm/gdp-version-edit.sh

{
	echo "# DO NOT EDIT THIS FILE.  IT IS AUTOGENERATED."
	echo GDP_VERSION_MAJOR=$major
	echo GDP_VERSION_MINOR=$minor
	echo GDP_VERSION_PATCH=$patch
	echo "# DO NOT EDIT THIS FILE.  IT IS AUTOGENERATED."
} > ../adm/gdp-version.sh
