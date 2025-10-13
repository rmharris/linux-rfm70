#!/bin/sh

ver=$(git describe --tags --abbrev=0 2> /dev/null || echo "unknown")
cat <<EOF | sed -e "s/@VERSION@/$ver/" > dkms.conf
PACKAGE_NAME="linux-rfm70"
PACKAGE_VERSION="@VERSION@"
MAKE[0]="make -C src"
CLEAN="make -C src clean"
BUILT_MODULE_LOCATION[0]="src"
BUILT_MODULE_NAME[0]="rfm70"
DEST_MODULE_LOCATION[0]="/updates"
AUTOINSTALL="yes"
POST_INSTALL="dkms.post_install.sh"
POST_REMOVE="dkms.post_remove.sh"
EOF
