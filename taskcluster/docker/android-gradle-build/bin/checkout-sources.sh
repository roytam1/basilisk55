#! /bin/bash -vex

set -x -e

# Inputs, with defaults

# mozharness builds use three repositories: goanna (source), mozharness (build
# scripts) and tools (miscellaneous) for each, specify *_REPOSITORY.  If the
# revision is not in the standard repo for the codebase, specify *_BASE_REPO as
# the canonical repo to clone and *_HEAD_REPO as the repo containing the
# desired revision.  For Mercurial clones, only *_HEAD_REV is required; for Git
# clones, specify the branch name to fetch as *_HEAD_REF and the desired sha1
# as *_HEAD_REV.

: GOANNA_REPOSITORY              ${GOANNA_REPOSITORY:=https://hg.mozilla.org/mozilla-central}
: GOANNA_BASE_REPOSITORY         ${GOANNA_BASE_REPOSITORY:=${GOANNA_REPOSITORY}}
: GOANNA_HEAD_REPOSITORY         ${GOANNA_HEAD_REPOSITORY:=${GOANNA_REPOSITORY}}
: GOANNA_HEAD_REV                ${GOANNA_HEAD_REV:=default}
: GOANNA_HEAD_REF                ${GOANNA_HEAD_REF:=${GOANNA_HEAD_REV}}

: TOOLS_REPOSITORY              ${TOOLS_REPOSITORY:=https://hg.mozilla.org/build/tools}
: TOOLS_BASE_REPOSITORY         ${TOOLS_BASE_REPOSITORY:=${TOOLS_REPOSITORY}}
: TOOLS_HEAD_REPOSITORY         ${TOOLS_HEAD_REPOSITORY:=${TOOLS_REPOSITORY}}
: TOOLS_HEAD_REV                ${TOOLS_HEAD_REV:=default}
: TOOLS_HEAD_REF                ${TOOLS_HEAD_REF:=${TOOLS_HEAD_REV}}
: TOOLS_DISABLE                 ${TOOLS_DISABLE:=false}

: WORKSPACE                     ${WORKSPACE:=/home/worker/workspace}

set -v

# check out tools where mozharness expects it to be ($PWD/build/tools and $WORKSPACE/build/tools)
if [ ! "$TOOLS_DISABLE" = true ]
then
    tc-vcs checkout $WORKSPACE/build/tools $TOOLS_BASE_REPOSITORY $TOOLS_HEAD_REPOSITORY $TOOLS_HEAD_REV $TOOLS_HEAD_REF

    if [ ! -d build ]; then
        mkdir -p build
        ln -s $WORKSPACE/build/tools build/tools
    fi
fi

# TODO - include tools repository in EXTRA_CHECKOUT_REPOSITORIES list
for extra_repo in $EXTRA_CHECKOUT_REPOSITORIES; do
    BASE_REPO="${extra_repo}_BASE_REPOSITORY"
    HEAD_REPO="${extra_repo}_HEAD_REPOSITORY"
    HEAD_REV="${extra_repo}_HEAD_REV"
    HEAD_REF="${extra_repo}_HEAD_REF"
    DEST_DIR="${extra_repo}_DEST_DIR"

    tc-vcs checkout ${!DEST_DIR} ${!BASE_REPO} ${!HEAD_REPO} ${!HEAD_REV} ${!HEAD_REF}
done

export GOANNA_DIR=$WORKSPACE/build/src
tc-vcs checkout $GOANNA_DIR $GOANNA_BASE_REPOSITORY $GOANNA_HEAD_REPOSITORY $GOANNA_HEAD_REV $GOANNA_HEAD_REF
