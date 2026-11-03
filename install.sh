#!/usr/bin/env bash
# Run with sudo after build.sh.  Installs the Venus driver and the video
# decoders into Waydroid, runs the render server and the video decoder as
# sandboxed system services and restarts a running Waydroid container to
# apply the new properties.  On failure it restores everything it changed.
set -eEuo pipefail

here=$(cd "$(dirname "$0")" && pwd)
user=${SUDO_USER:?run with sudo}
home=$(getent passwd "$user" | cut -d: -f6)
host=$here/build/host
out=$here/build/out
prefix=/usr/local/lib/waydroid-venus
overlay=/var/lib/waydroid/overlay/vendor
cfg=/var/lib/waydroid/waydroid.cfg
# property values from before the first install, for uninstall.sh
backup=/var/lib/waydroid/waydroid-venus-properties.json
unit=/etc/systemd/system/waydroid-venus.service
sysusers=/etc/sysusers.d/waydroid-venus.conf
hwdec_unit=/etc/systemd/system/waydroid-hwdec.service
hwdec_sysusers=/etc/sysusers.d/waydroid-hwdec.conf
socket_dir=$home/.local/share/waydroid/data/venus
# earlier versions ran the server as a user service
legacy_unit=$home/.config/systemd/user/waydroid-venus.service
legacy_prefix=$home/.local/lib/waydroid-venus

# build/out/LIB/MODULE goes to the overlay as vendor/LIB/hw/MODULE
modules=(lib64/vulkan.virtio.so lib64/gralloc.default.so lib/vulkan.virtio.so lib/gralloc.default.so)
overlay_files=()
for module in "${modules[@]}"; do
   overlay_files+=("$overlay/$(dirname "$module")/hw/$(basename "$module")")
done
# build/out/PATH goes to the overlay as vendor/PATH.  The seccomp policy and
# codec list are the image's with additions, which build.sh made.
hwdec_files=(lib/libstagefrighthw.so etc/media_codecs_waydroid.xml
   etc/media_codecs_waydroid_hwdec.xml etc/seccomp_policy/mediacodec.policy)

missing=0
for file in "${modules[@]/#/$out/}" "${hwdec_files[@]/#/$out/}" \
   "$host/bin/virgl_test_server" "$host/libexec/virgl_render_server" "$host/bin/waydroid-hwdec" \
   "$here/android/waydroid-venus-recover.sh" "$here/android/waydroid-venus.rc" \
   "$here/waydroid-venus.service" "$here/waydroid-venus.sysusers" \
   "$here/waydroid-hwdec.service" "$here/waydroid-hwdec.sysusers"; do
   if [ ! -f "$file" ]; then
      echo "missing $file" >&2
      missing=1
   fi
done
[ "$missing" = 0 ] || { echo "run build.sh first" >&2; exit 1; }
[ -f "$cfg" ] || { echo "$cfg not found; initialize Waydroid first" >&2; exit 1; }

# Everything this script replaces is saved here and restored on failure.
tx=$(mktemp -d)
was_active=$(systemctl is-active waydroid-venus.service 2> /dev/null || true)
was_enabled=$(systemctl is-enabled waydroid-venus.service 2> /dev/null || true)
hwdec_was_active=$(systemctl is-active waydroid-hwdec.service 2> /dev/null || true)
hwdec_was_enabled=$(systemctl is-enabled waydroid-hwdec.service 2> /dev/null || true)
legacy_stopped=0
save() {
   local path
   for path; do
      if [ -e "$path" ]; then
         mkdir -p "$tx/root$(dirname "$path")"
         cp -a "$path" "$tx/root$path"
         echo "$path" >> "$tx/saved"
      else
         echo "$path" >> "$tx/absent"
      fi
   done
}
rollback() {
   trap - ERR
   echo "installation failed, restoring the previous state" >&2
   systemctl stop waydroid-venus.service waydroid-hwdec.service 2> /dev/null || true
   [ "$was_enabled" = enabled ] || systemctl disable waydroid-venus.service 2> /dev/null || true
   [ "$hwdec_was_enabled" = enabled ] || systemctl disable waydroid-hwdec.service 2> /dev/null || true
   if [ -f "$tx/absent" ]; then
      while read -r path; do rm -rf "$path"; done < "$tx/absent"
   fi
   if [ -f "$tx/saved" ]; then
      while read -r path; do
         rm -rf "$path"
         cp -a "$tx/root$path" "$path"
      done < "$tx/saved"
   fi
   systemctl daemon-reload
   [ "$was_active" = active ] && systemctl start waydroid-venus.service
   [ "$hwdec_was_active" = active ] && systemctl start waydroid-hwdec.service
   if [ "$legacy_stopped" = 1 ]; then
      systemctl --user -M "$user@" start waydroid-venus.service || true
   fi
   rm -rf "$tx"
   exit 1
}
trap rollback ERR

save "$prefix" "$cfg" "$backup" "$unit" "$sysusers" "$hwdec_unit" "$hwdec_sysusers" \
   "${overlay_files[@]}" "${hwdec_files[@]/#/$overlay/}" \
   "$overlay/bin/waydroid-venus-recover.sh" "$overlay/etc/init/waydroid-venus.rc"

install -D -m 0644 "$here/waydroid-venus.sysusers" "$sysusers"
install -D -m 0644 "$here/waydroid-hwdec.sysusers" "$hwdec_sysusers"
systemd-sysusers "$sysusers" "$hwdec_sysusers"

rm -rf "$prefix.new"
cp -a "$host" "$prefix.new"
chown -R root:root "$prefix.new"
rm -rf "$prefix"
mv "$prefix.new" "$prefix"

for i in "${!modules[@]}"; do
   install -D -m 0644 "$out/${modules[$i]}" "${overlay_files[$i]}"
done
for file in "${hwdec_files[@]}"; do
   install -D -m 0644 "$out/$file" "$overlay/$file"
done
install -D -m 0755 "$here/android/waydroid-venus-recover.sh" \
   "$overlay/bin/waydroid-venus-recover.sh"
install -D -m 0644 "$here/android/waydroid-venus.rc" \
   "$overlay/etc/init/waydroid-venus.rc"

legacy=0
[ -f "$legacy_unit" ] && legacy=1
python3 - "$cfg" "$backup" "$legacy" <<'EOF'
import configparser, json, os, sys
cfg_path, backup_path, legacy = sys.argv[1], sys.argv[2], sys.argv[3] == "1"
props = {
    "ro.hardware.egl": "angle",
    "ro.hardware.vulkan": "virtio",
    "mesa.vn.debug": "vtest",
    "mesa.vtest.socket.name": "/data/venus/vtest.sock",
    # media_codecs_waydroid.xml, which adds the OMX.waydroid decoders
    "ro.media.xml_variant.codecs": "_waydroid",
}
cfg = configparser.ConfigParser()
cfg.read(cfg_path)
if not cfg.has_section("properties"):
    cfg.add_section("properties")
previous = {}
if os.path.exists(backup_path):
    with open(backup_path) as f:
        previous = json.load(f)
# keys added since the last install are backed up now
missing = [key for key in props if key not in previous]
if missing:
    for key in missing:
        current = cfg["properties"].get(key)
        # an install from before backups existed set these values itself
        previous[key] = None if legacy and current == props[key] else current
    with open(backup_path, "w") as f:
        json.dump(previous, f)
cfg["properties"].update(props)
with open(cfg_path, "w") as f:
    cfg.write(f)
EOF

# the old user service would hold the same socket
if [ "$legacy" = 1 ]; then
   systemctl --user -M "$user@" stop waydroid-venus.service || true
   legacy_stopped=1
fi

substitute() {
   sed -e "s|@PREFIX@|$prefix|g" -e "s|@SOCKET_DIR@|$socket_dir|g" -e "s|@USER@|$user|g" "$1"
}
substitute "$here/waydroid-venus.service" > "$unit"
substitute "$here/waydroid-hwdec.service" > "$hwdec_unit"
systemctl daemon-reload
systemctl enable waydroid-venus.service waydroid-hwdec.service
systemctl restart waydroid-venus.service waydroid-hwdec.service
for socket in vtest.sock hwdec.sock; do
   for _ in $(seq 50); do
      [ -S "$socket_dir/$socket" ] && [ "$(stat -c %a "$socket_dir/$socket")" = 666 ] && break
      sleep 0.2
   done
   [ "$(stat -c %a "$socket_dir/$socket" 2> /dev/null)" = 666 ]
done

# regenerates waydroid_base.prop and restarts a running container
waydroid upgrade --offline

trap - ERR
rm -rf "$tx"

if [ "$legacy" = 1 ]; then
   systemctl --user -M "$user@" disable waydroid-venus.service || true
   rm -f "$legacy_unit"
   systemctl --user -M "$user@" daemon-reload || true
   rm -rf "$legacy_prefix"
fi
echo "installed"
