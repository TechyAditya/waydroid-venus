#!/usr/bin/env bash
# Builds the patched render server and the video decoder for the host, and
# the patched Venus Vulkan driver, gralloc and OMX video decoders for
# Waydroid's Android 13 x86_64 image.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
build=$here/build
# install.sh copies the host build to /usr/local/lib/waydroid-venus
prefix=/usr/local/lib/waydroid-venus
host_out=$build/host

# Mesa 26.0.8, the release in the Waydroid vendor image, so only the
# patches differ; virglrenderer 1.3.0; AOSP android13-release.
mesa_commit=60e95b787857afbc9a00b693b91c0d9c8923a430
virgl_commit=ca50e008863837e094747a69974dde3ae148aeaa
libhardware_commit=bbd07bd2e60752a665ebc9b0915241f3c1636516
# LineageOS 20, which Waydroid's Android 13 image is built from
lineage_av_commit=673733ea38f2fae2552b43ff0b01338c6393b5d2
lineage_native_commit=915ad642e331e0f8ea107b5f5519f2c6f1e7e01c
lineage_core_commit=c0500ffe2cc283d61f1e3db1b2c979bfbd9beb8c
lineage_logging_commit=373d54e772f303c441faf2e584a2edf3601b4181
ndk_version=r28c
android_api=33

mkdir -p "$build/out"

# Checks out COMMIT of URL into DIR with PATCH applied.  An existing tree is
# used only if it is at COMMIT with exactly PATCH applied, so an interrupted
# run or an updated patch cannot build stale sources.
fetch() {
   local url=$1 commit=$2 dir=$3 patch=$4
   if [ -d "$dir" ]; then
      if [ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ] ||
         ! git -C "$dir" diff HEAD | cmp -s - "$patch" ||
         [ -n "$(git -C "$dir" ls-files --others --exclude-standard)" ]; then
         echo "$dir does not match $(basename "$patch"); remove it to rebuild" >&2
         exit 1
      fi
      return
   fi

   local tmp=$dir.tmp
   rm -rf "$tmp"
   git init -q "$tmp"
   git -C "$tmp" fetch -q --depth 1 "$url" "$commit"
   git -C "$tmp" checkout -q FETCH_HEAD
   git -C "$tmp" apply "$patch"
   mv "$tmp" "$dir"
}

# Checks out PATHS of LineageOS repository REPO at COMMIT into
# build/lineage/REPO, for headers only.
fetch_headers() {
   local repo=$1 commit=$2 dir=$build/lineage/$1
   shift 2
   if [ -d "$dir" ]; then
      if [ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ] || ! git -C "$dir" diff --quiet HEAD; then
         echo "$dir is not a clean checkout of $commit; remove it to rebuild" >&2
         exit 1
      fi
      return
   fi

   local tmp=$dir.tmp
   rm -rf "$tmp"
   git init -q "$tmp"
   git -C "$tmp" remote add origin "https://github.com/LineageOS/android_$repo"
   git -C "$tmp" sparse-checkout set --no-cone "$@"
   git -C "$tmp" fetch -q --depth 1 --filter=blob:none origin "$commit"
   git -C "$tmp" checkout -q FETCH_HEAD
   mv "$tmp" "$dir"
}

fetch https://gitlab.freedesktop.org/virgl/virglrenderer.git "$virgl_commit" \
   "$build/virglrenderer" "$here/patches/virglrenderer-vtest-import-fd.patch"
fetch https://gitlab.freedesktop.org/mesa/mesa.git "$mesa_commit" \
   "$build/mesa" "$here/patches/mesa-venus-vtest-android.patch"
fetch https://android.googlesource.com/platform/hardware/libhardware "$libhardware_commit" \
   "$build/libhardware" "$here/patches/gralloc-waydroid-venus.patch"
fetch_headers frameworks_av "$lineage_av_commit" \
   media/libstagefright/omx/include media/libstagefright/foundation/include
fetch_headers frameworks_native "$lineage_native_commit" \
   headers/media_plugin libs/nativewindow/include libs/nativebase/include libs/arect/include
fetch_headers system_core "$lineage_core_commit" \
   libutils/include libcutils/include libsystem/include
fetch_headers system_logging "$lineage_logging_commit" liblog/include

ndk=${ANDROID_NDK:-$build/android-ndk-$ndk_version}
if [ ! -f "$ndk/source.properties" ]; then
   rm -rf "$build/ndk.tmp"
   mkdir "$build/ndk.tmp"
   curl -fL --retry 5 -o "$build/ndk.tmp/ndk.zip" \
      "https://dl.google.com/android/repository/android-ndk-$ndk_version-linux.zip"
   unzip -q -d "$build/ndk.tmp" "$build/ndk.tmp/ndk.zip"
   rm -rf "$ndk"
   mv "$build/ndk.tmp/android-ndk-$ndk_version" "$ndk"
   rm -rf "$build/ndk.tmp"
fi

if [ ! -d "$build/virglrenderer/build" ]; then
   meson setup "$build/virglrenderer/build" "$build/virglrenderer" \
      --prefix="$prefix" -Dbuildtype=release -Dvenus=true -Dvideo=false -Dtests=false
fi
meson configure "$build/virglrenderer/build" --prefix="$prefix"
rm -rf "$host_out" "$host_out.tmp"
DESTDIR="$host_out.tmp" ninja -C "$build/virglrenderer/build" install
mv "$host_out.tmp$prefix" "$host_out"
rm -rf "$host_out.tmp"

# It parses bitstreams from Android apps, so build it hardened.
cc -O2 -Wall -Wextra -D_FORTIFY_SOURCE=3 -fstack-protector-strong -fstack-clash-protection \
   -fPIE -pie -Wl,-z,relro,-z,now -pthread \
   -o "$host_out/bin/waydroid-hwdec" "$here/hwdec/host/waydroid-hwdec.c" \
   $(pkg-config --cflags --libs libavcodec libavutil libswscale)

ndk_bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
gralloc_src=$build/libhardware/modules/gralloc

# Waydroid runs 64-bit and 32-bit processes, which load /vendor/lib64 and
# /vendor/lib respectively.
for abi in x86_64 x86; do
   case $abi in
   x86_64) triple=x86_64-linux-android lib=lib64 ;;
   x86) triple=i686-linux-android lib=lib ;;
   esac

   cat > "$build/android-$abi.cross" <<EOF
[binaries]
ar = '$ndk_bin/llvm-ar'
c = ['$ndk_bin/$triple$android_api-clang']
cpp = ['$ndk_bin/$triple$android_api-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$ndk_bin/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=/nonexistent', '/usr/bin/pkg-config']

[host_machine]
system = 'android'
cpu_family = '$abi'
cpu = '$abi'
endian = 'little'
EOF

   mesa_build=$build/mesa/build-android-$abi
   if [ ! -d "$mesa_build" ]; then
      meson setup "$mesa_build" "$build/mesa" \
         --cross-file "$build/android-$abi.cross" \
         -Dbuildtype=release -Db_ndebug=true \
         -Dplatforms=android -Dplatform-sdk-version=$android_api \
         -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
         -Dvulkan-drivers=virtio -Dgallium-drivers= -Dvulkan-layers= -Dtools= \
         -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dllvm=disabled \
         -Dzstd=disabled -Dxmlconfig=disabled -Dvideo-codecs= \
         -Dallow-fallback-for=libdrm -Dlibdrm:default_library=static
   fi
   ninja -C "$mesa_build" src/virtio/vulkan/libvulkan_virtio.so
   mkdir -p "$build/out/$lib"
   "$ndk_bin/llvm-strip" -o "$build/out/$lib/vulkan.virtio.so" \
      "$mesa_build/src/virtio/vulkan/libvulkan_virtio.so"

   # AOSP's default gralloc with NVIDIA-compatible row alignment and YUV
   # support.  It links against the stub libraries from the Mesa build;
   # Waydroid uses 4 KiB pages.
   "$ndk_bin/$triple$android_api-clang++" -shared -fPIC -O2 -std=c++17 \
      -static-libstdc++ -DPAGE_SIZE=4096 -DLOG_TAG='"gralloc"' \
      -I"$here/gralloc/include" -I"$build/mesa/include/android_stub" -I"$gralloc_src" \
      -o "$build/out/$lib/gralloc.default.so" \
      "$gralloc_src/gralloc.cpp" "$gralloc_src/mapper.cpp" "$here/gralloc/framebuffer_stub.cpp" \
      -L"$mesa_build/src/android_stub" -lcutils -llog
   "$ndk_bin/llvm-strip" "$build/out/$lib/gralloc.default.so"
done

# The OMX video decoders.  The codec service that loads them is 32-bit only.
# They link against the image's own libraries, which debugfs reads from the
# images without mounting them, and the image's codec list and seccomp
# policy are extended rather than replaced.
images=$(sed -n 's/^images_path *= *//p' /var/lib/waydroid/waydroid.cfg 2> /dev/null)
images=${WAYDROID_IMAGES:-${images:-/var/lib/waydroid/images}}
image_files=$build/waydroid-image
rm -rf "$image_files"
mkdir -p "$image_files/lib"
# Copies PATH from the ext4 IMAGE to DEST.
extract() {
   local image=$1 path=$2 dest=$3
   debugfs -R "dump $path $dest" "$image" > /dev/null 2>&1 || true
   if [ ! -s "$dest" ]; then
      echo "cannot read $path from $image" >&2
      exit 1
   fi
}
for name in libstagefright_foundation libhardware libutils libcutils liblog; do
   extract "$images/system.img" "/system/lib/$name.so" "$image_files/lib/$name.so"
done
extract "$images/vendor.img" /lib/libstagefright_softomx.so "$image_files/lib/libstagefright_softomx.so"
extract "$images/vendor.img" /etc/media_codecs.xml "$image_files/media_codecs.xml"
extract "$images/vendor.img" /etc/seccomp_policy/mediacodec.policy "$image_files/mediacodec.policy"

lineage=$build/lineage
"$ndk_bin/i686-linux-android$android_api-clang++" -shared -fPIC -O2 -std=gnu++17 \
   -fno-rtti -fno-exceptions -static-libstdc++ -Wall -Wno-unused-parameter \
   -I"$lineage/frameworks_av/media/libstagefright/omx/include" \
   -I"$lineage/frameworks_av/media/libstagefright/foundation/include" \
   -I"$lineage/frameworks_native/headers/media_plugin" \
   -I"$lineage/frameworks_native/headers/media_plugin/media/openmax" \
   -I"$lineage/frameworks_native/libs/nativewindow/include" \
   -I"$lineage/frameworks_native/libs/nativebase/include" \
   -I"$lineage/frameworks_native/libs/arect/include" \
   -I"$lineage/system_core/libutils/include" -I"$lineage/system_core/libcutils/include" \
   -I"$lineage/system_core/libsystem/include" -I"$lineage/system_logging/liblog/include" \
   -I"$build/libhardware/include" \
   -o "$build/out/lib/libstagefrighthw.so" "$here/hwdec/android/WaydroidDecoder.cpp" \
   -L"$image_files/lib" -lstagefright_softomx -lstagefright_foundation -lhardware -lutils \
   -lcutils -llog -Wl,--no-undefined
"$ndk_bin/llvm-strip" "$build/out/lib/libstagefrighthw.so"

mkdir -p "$build/out/etc/seccomp_policy"
cp "$here/hwdec/android/media_codecs_waydroid_hwdec.xml" "$build/out/etc/"
sed 's|^</MediaCodecs>|    <Include href="media_codecs_waydroid_hwdec.xml" />\n&|' \
   "$image_files/media_codecs.xml" > "$build/out/etc/media_codecs_waydroid.xml"
if [ "$(grep -c media_codecs_waydroid_hwdec "$build/out/etc/media_codecs_waydroid.xml")" != 1 ]; then
   echo "unexpected $images/vendor.img:/etc/media_codecs.xml" >&2
   exit 1
fi
cat "$image_files/mediacodec.policy" "$here/hwdec/android/mediacodec.policy" \
   > "$build/out/etc/seccomp_policy/mediacodec.policy"

echo "render server: $host_out/bin/virgl_test_server"
echo "video decoder: $host_out/bin/waydroid-hwdec"
echo "android files: $build/out"
