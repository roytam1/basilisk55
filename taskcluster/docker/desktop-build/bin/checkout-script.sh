#! /bin/bash -vex

set -x -e

# Inputs, with defaults

: GOANNA_HEAD_REPOSITORY              ${GOANNA_HEAD_REPOSITORY:=https://hg.mozilla.org/mozilla-central}
: GOANNA_HEAD_REV                ${GOANNA_HEAD_REV:=default}

: SCRIPT_DOWNLOAD_PATH          ${SCRIPT_DOWNLOAD_PATH:=$PWD}
: SCRIPT_PATH                   ${SCRIPT_PATH:?"script path must be set"}
set -v

# download script from the goanna repository
url=${GOANNA_HEAD_REPOSITORY}/raw-file/${GOANNA_HEAD_REV}/${SCRIPT_PATH}
wget --directory-prefix=${SCRIPT_DOWNLOAD_PATH} $url
chmod +x `basename ${SCRIPT_PATH}`
