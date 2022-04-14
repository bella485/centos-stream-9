#!/usr/bin/bash

[ -z "${RHDISTDATADIR}" ] && echo "ERROR: RHDISTDATADIR undefined." && exit 1

# This script generates 'dist-dump-variables' output for various configurations
# using known ark commit IDs.  It uses this information as well as setting
# different values for DISTRO and DIST.
#
# The centos-stream commit IDs are
#
#    13d668e5a3f9 := kernel-5.14.0-76.el9
#    cdb70f21aa06 := kernel-5.14.0-76.el9 + 2 commits
#

for DISTRO in fedora rhel centos
do
	for commit in 13d668e5a3f9 cdb70f21aa06
	do
		for DIST in .fc32 .el9
		do
			varfilename="${RHDISTDATADIR}/${DISTRO}-${commit}${DIST}"

			echo "building $varfilename"

			# CURDIR is a make special target and cannot be easily changed.  Omit
			# CURDIR from the output.
			make RHSELFTESTDATA=1 DIST="${DIST}" DISTRO="${DISTRO}" HEAD=${commit} dist-dump-variables | grep "=" | grep -v CURDIR >& "${varfilename}"

			# When executed from a script, the variables in Makefile.variables are
			# listed as having origin 'environment'.  This is because the script
			# inherits the variables from the 'export' command in the redhat/Makefile.
			# The 'dist-dump-variables' target explicitly omits these variables from
			# its output.  As a workaround, read in the variables and output them to
			# the data file.
			# shellcheck disable=SC2002
			cat Makefile.variables | grep -v "^#" | sed '/^$/d' | tr -d " " | awk -F "?=|:=" '{print $1}' | while read -r VAR
			do
				[ "$VAR" == "RHDISTDATADIR" ] && continue
				echo "$VAR=${!VAR}"
			done >> "${varfilename}"

			echo "building ${varfilename}.spec"
			make RHSELFTESTDATA=1 DIST="${DIST}" DISTRO="${DISTRO}" HEAD=${commit} setup-source
			cp "${SOURCES}"/kernel.spec "${varfilename}".spec
		done
	done
done

exit 0
