#!/bin/sh

SRCDIR=$(cd $(dirname $0)/../../../..; pwd)
GOANNA_DIR=$SRCDIR $SRCDIR/taskcluster/scripts/builder/build-haz-linux.sh $(pwd) "$@"
