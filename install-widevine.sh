#!/usr/bin/env bash
# Run with sudo.  Installs Google's Widevine DRM (security level L3) into
# Waydroid, so apps can play protected video; the decrypted frames go to the
# same decoders as other video.  Waydroid's image only has ClearKey.  The
# Widevine service is a proprietary Google binary from ChromeOS's Android 13
# image, downloaded from the archive waydroid_script uses.  Restarts a
# running Waydroid container.
set -euo pipefail

archive=https://github.com/WayDroid-ATV/vendor_google_proprietary_widevine-prebuilt/archive/679552343d8b2e8d7a19b6df61c7a03963d0c75b.zip
archive_sha256=ba7b0ceb9152df3b00f0a5893a52fe954edcbd8b95693c0e65105a34f4c519c2
overlay=/var/lib/waydroid/overlay/vendor
# vendor/PATH in the archive's prebuilts directory goes to the overlay as
# vendor/PATH; the archive's ClearKey plugin duplicates the image's own
files=(bin/hw/android.hardware.drm-service-lazy.widevine
   etc/init/android.hardware.drm-service-lazy.widevine.rc
   etc/vintf/manifest/manifest_android.hardware.drm-service.widevine.xml
   lib64/libwvaidl.so)

[ "$(id -u)" = 0 ] || { echo "run with sudo" >&2; exit 1; }
[ -d "$overlay" ] || { echo "$overlay not found; initialize Waydroid first" >&2; exit 1; }

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
curl -fsSL --retry 5 -o "$tmp/widevine.zip" "$archive"
echo "$archive_sha256  $tmp/widevine.zip" | sha256sum -c --quiet
unzip -q -d "$tmp" "$tmp/widevine.zip"
prebuilts=$(echo "$tmp"/vendor_google_proprietary_widevine-prebuilt-*/prebuilts)

for file in "${files[@]}"; do
   mode=0644
   [[ $file == bin/* ]] && mode=0755
   install -D -m "$mode" "$prebuilts/$file" "$overlay/$file"
done
# libwvaidl.so needs libprotobuf-cpp-lite.so, which the vendor image only
# has under its versioned name
ln -sfn libprotobuf-cpp-lite-3.9.1.so "$overlay/lib64/libprotobuf-cpp-lite.so"

# restarts a running container, which mounts the overlay
waydroid upgrade --offline
echo "installed Widevine"
