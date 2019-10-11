#!/bin/sh
#
# This scripts lists all the .deb files in the "build/" directory and
# publishes them one by one to Cloudsmith.io
#

for f in build/*.deb
do
    echo "Uploading '${f}' to Cloudsmith..."
    cloudsmith --credentials-file "${CLOUDSMITH_CREDENTIALS_INI}" \
        push deb --republish "${CLOUDSMITH_DEB_REPO}" "${f}"
done
