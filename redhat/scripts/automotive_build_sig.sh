#!/bin/bash

# clones and updates an automotive sig repo
# $1: branch to be used
# $2: local pristine clone of auto-sig
# $3: alternate tmp directory (if you have faster storage)
# $4: kernel source tarball
# $5: package name
# shellcheck disable=SC2164

autosig_branch=$1;
autosig_cache=$2;
autosig_tmp=$3;
autosig_tarball=$4;
package_name=$5;

redhat=$(dirname "$0")/..;
topdir="$redhat"/..;

function die
{
	echo "Error: $1" >&2;
	exit 1;
}

function upload_sources()
{
	echo "Cloning the centos common repository"
	git clone https://git.centos.org/centos-git-common.git centos-git-common >/dev/null || die "Unable to clone centos tools";
	./centos-git-common/lookaside_upload -f $autosig_tarball -n $package_name -b $autosig_branch
}

function update_patches()
{
	echo "Cloning $package_name source rpm repository"
	git clone -b $autosig_branch ssh://git@git.centos.org/rpms/$package_name $package_name >/dev/null || die "Unable to clone using local cache";
	find "$redhat"/rpm/SOURCES/ \( ! -name "*${autosig_tarball#*.}" \) -type f | xargs cp -t "${package_name}/SOURCES/"
	mv "${package_name}/SOURCES/kernel.spec" "${package_name}/SPECS/"
	tarball_sha=($(sha1sum $autosig_tarball))
	echo "$tarball_sha SOURCES/$(basename -- "$autosig_tarball")" > "${package_name}/.kernel-auto.metadata"
}

date=$(date +"%Y-%m-%d")
tmp="$(mktemp -d --tmpdir="$autosig_tmp" AUTOSIG."$date".XXXXXXXX)";

cd "$tmp" || die "Unable to create temporary directory";

# upload the sources into centos repos
upload_sources || die "Unable to upload the sources"

# update the centos sources into package repo
update_patches || die "Unable to copy the patches"

# all done
echo "Commit your changes here..."
echo "$tmp/$package_name"
