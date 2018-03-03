#!/bin/bash

set -e

if test "${ACTION}" != "clean"; then
    echo "Building in ${GOANNA_OBJDIR}"
    make -j8 -s -C $GOANNA_OBJDIR binaries

    echo "Copying files from ${GOANNA_OBJDIR}/dist/bin"
    rsync -pvtrlL --exclude "Test*" \
          --exclude "test_*" --exclude "*_unittest" \
          --exclude xulrunner  \
          ${GOANNA_OBJDIR}/dist/bin/ $BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/Frameworks

    if test ${ARCHS} == "armv7"; then
        for x in $BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/Frameworks/*.dylib $BUILT_PRODUCTS_DIR/$CONTENTS_FOLDER_PATH/Frameworks/XUL; do
            echo "Signing $x"
            /usr/bin/codesign --force --sign "${EXPANDED_CODE_SIGN_IDENTITY}" --preserve-metadata=identifier,entitlements,resource-rules $x
        done
    fi
fi
