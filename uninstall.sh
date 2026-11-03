#!/usr/bin/env bash
# Run with sudo.  Restores Waydroid's software rendering and video decoding
# and removes the render server and video decoder services.  Restarts a
# running Waydroid container.  Safe to run again after a failure: the
# property backup is removed last.
set -euo pipefail

user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
prefix=/usr/local/lib/waydroid-venus
overlay=/var/lib/waydroid/overlay/vendor
cfg=/var/lib/waydroid/waydroid.cfg
backup=/var/lib/waydroid/waydroid-venus-properties.json
unit=/etc/systemd/system/waydroid-venus.service
sysusers=/etc/sysusers.d/waydroid-venus.conf
hwdec_unit=/etc/systemd/system/waydroid-hwdec.service
hwdec_sysusers=/etc/sysusers.d/waydroid-hwdec.conf
socket_dir=$home/.local/share/waydroid/data/venus
legacy_unit=$home/.config/systemd/user/waydroid-venus.service

python3 - "$cfg" "$backup" <<'EOF'
import configparser, json, os, sys
cfg_path, backup_path = sys.argv[1:]
props = {
    "ro.hardware.egl": "angle",
    "ro.hardware.vulkan": "virtio",
    "mesa.vn.debug": "vtest",
    "mesa.vtest.socket.name": "/data/venus/vtest.sock",
    "ro.media.xml_variant.codecs": "_waydroid",
}
if os.path.exists(backup_path):
    with open(backup_path) as f:
        previous = json.load(f)
else:
    # without a backup, only remove values that are still ours
    previous = None
cfg = configparser.ConfigParser()
cfg.read(cfg_path)
if cfg.has_section("properties"):
    for key, value in props.items():
        if previous is None:
            if cfg["properties"].get(key) == value:
                cfg.remove_option("properties", key)
        elif key not in previous:
            # added after the backup was made
            if cfg["properties"].get(key) == value:
                cfg.remove_option("properties", key)
        elif previous[key] is None:
            cfg.remove_option("properties", key)
        else:
            cfg["properties"][key] = previous[key]
with open(cfg_path, "w") as f:
    cfg.write(f)
EOF

for lib in lib64 lib; do
   rm -f "$overlay/$lib/hw/vulkan.virtio.so" "$overlay/$lib/hw/gralloc.default.so"
done
rm -f "$overlay/bin/waydroid-venus-recover.sh" "$overlay/etc/init/waydroid-venus.rc"
rm -f "$overlay/lib/libstagefrighthw.so" "$overlay/etc/media_codecs_waydroid.xml" \
   "$overlay/etc/media_codecs_waydroid_hwdec.xml" "$overlay/etc/seccomp_policy/mediacodec.policy"
waydroid upgrade --offline

systemctl disable --now waydroid-venus.service waydroid-hwdec.service 2> /dev/null || true
rm -f "$unit" "$hwdec_unit"
systemctl daemon-reload
# the directory is shared with other services' sockets
rm -f "$socket_dir/vtest.sock" "$socket_dir/hwdec.sock"
for account in waydroid-venus waydroid-hwdec; do
   setfacl -x "u:$account" "$socket_dir" 2> /dev/null || true
done
rm -rf "$prefix" /var/cache/waydroid-venus /var/cache/waydroid-hwdec
rm -f "$sysusers" "$hwdec_sysusers"
for account in waydroid-venus waydroid-hwdec; do
   if getent passwd "$account" > /dev/null; then
      userdel "$account"
   fi
done

if [ -f "$legacy_unit" ]; then
   systemctl --user -M "$user@" disable --now waydroid-venus.service || true
   rm -f "$legacy_unit"
   systemctl --user -M "$user@" daemon-reload || true
   rm -rf "$home/.local/lib/waydroid-venus"
fi

rm -f "$backup"
echo "uninstalled"
