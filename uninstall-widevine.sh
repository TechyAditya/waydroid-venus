#!/usr/bin/env bash
# Run with sudo.  Removes what install-widevine.sh installed.  Restarts a
# running Waydroid container.
set -euo pipefail

overlay=/var/lib/waydroid/overlay/vendor

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
rm -f "$overlay/bin/hw/android.hardware.drm-service-lazy.widevine" \
   "$overlay/etc/init/android.hardware.drm-service-lazy.widevine.rc" \
   "$overlay/etc/vintf/manifest/manifest_android.hardware.drm-service.widevine.xml" \
   "$overlay/lib64/libwvaidl.so" "$overlay/lib64/libprotobuf-cpp-lite.so"
waydroid upgrade --offline
echo "uninstalled Widevine"
