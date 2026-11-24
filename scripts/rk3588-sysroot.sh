#!/usr/bin/env bash
# Build the sysroot for cross-compiling to the Radxa ROCK 5B+ (RK3588, Debian 12),
# taken from the board itself so headers and libraries match what it runs.
#
#   scripts/rk3588-sysroot.sh                  # -> $SCIP_SYSROOT_DIR or ~/sysroots/rock5bplus-debian12
#   scripts/rk3588-sysroot.sh /some/folder
#
# Nothing is installed on the board. Headers the board image lacks (GStreamer,
# glib) are fetched there with `apt-get download` in the versions closest to the
# installed libraries, and unpacked into the sysroot on the PC only.
# Re-run after upgrading packages on the board.
set -euo pipefail
export SCIP_TARGET=rk3588
source "$(dirname "${BASH_SOURCE[0]}")/board-env.sh"

dest="${1:-${SCIP_SYSROOT_DIR:-$HOME/sysroots/rock5bplus-debian12}}"
mkdir -p "$dest"
dest="$(cd "$dest" && pwd)"
echo "sysroot: $SCIP_BOARD -> $dest"

# 1. Headers, libraries, the GCC 12 runtime and pkg-config files from the board.
#    Plugin folders the build never links against are left out (~600 MB).
#    GStreamer's plugins stay: the tests use them when run under QEMU.
"${SCIP_SSH[@]}" "tar -C / -cf - \
    --exclude='usr/lib/aarch64-linux-gnu/dri' \
    --exclude='usr/lib/aarch64-linux-gnu/qt5' \
    --exclude='usr/lib/aarch64-linux-gnu/qt6' \
    --exclude='usr/lib/aarch64-linux-gnu/maliit' \
    --exclude='usr/lib/aarch64-linux-gnu/libpinyin' \
    --exclude='usr/lib/aarch64-linux-gnu/guile' \
    --exclude='usr/lib/aarch64-linux-gnu/perl' \
    --exclude='usr/lib/aarch64-linux-gnu/samba' \
    --exclude='usr/lib/aarch64-linux-gnu/vdpau' \
    --exclude='usr/lib/aarch64-linux-gnu/ImageMagick-*' \
    --exclude='usr/lib/aarch64-linux-gnu/vtk' \
    --exclude='usr/lib/aarch64-linux-gnu/openmpi' \
    --exclude='usr/lib/aarch64-linux-gnu/xtables' \
    --exclude='usr/lib/aarch64-linux-gnu/girepository-1.0' \
    --exclude='usr/lib/aarch64-linux-gnu/gconv' \
    usr/include usr/lib/aarch64-linux-gnu usr/lib/gcc/aarch64-linux-gnu/12 \
    usr/lib/pkgconfig usr/share/pkgconfig etc/ld.so.conf etc/ld.so.conf.d \
    \$(cd / && ls -d usr/lib/*.so*) 2>/dev/null | gzip -1" | tar -C "$dest" -xzf -
# (usr/lib/*.so*: libraries outside the multiarch folder that OpenCV's dependencies
#  need at link time, e.g. HDF4 and armadillo for libgdal, and Rockchip's RKNN/ISP.)
# Debian is usr-merged: /lib -> usr/lib.
[[ -e "$dest/lib" ]] || ln -s usr/lib "$dest/lib"

# 2. Development packages missing on the board, matched to the installed versions.
#    Each line is "DEV_PKG RUNTIME_PKG". The -dev version picked is the one equal to
#    the installed runtime's, else the Debian (not vendor) one of the same upstream
#    version, else the newest one not newer than the runtime (older headers are safe
#    against a newer library; newer ones are not).
pkgs=(
    "libdrm-dev libdrm2"
    "libunwind-dev libunwind8"
    "libdw-dev libdw1"
    "libelf-dev libelf1"
    "zlib1g-dev zlib1g"
    "libgstreamer1.0-dev libgstreamer1.0-0"
    "libgstreamer-plugins-base1.0-dev libgstreamer-plugins-base1.0-0"
    "libglib2.0-dev libglib2.0-0"
    "libffi-dev libffi8"
    "libpcre2-dev libpcre2-8-0"
    "libmount-dev libmount1"
    "libblkid-dev libblkid1"
    "libselinux1-dev libselinux1"
    "libsepol-dev libsepol2"
    "liborc-0.4-dev liborc-0.4-0"
)
debs="$(mktemp -d)"
trap 'rm -rf "$debs"' EXIT
"${SCIP_SSH[@]}" "set -e
    work=\$(mktemp -d); cd \$work
    for pair in $(printf "'%s' " "${pkgs[@]}"); do
        set -- \$pair
        dev=\$1; run=\$2
        have=\$(dpkg-query -W -f='\${Version}' \"\$run\" 2>/dev/null || true)
        [ -n \"\$have\" ] || { echo \"skip \$dev: \$run not installed\" >&2; continue; }
        upstream=\${have%%-*}
        exact=; deb=; older=
        # madison lists newest first: 'name | version | repo ...'
        while IFS='|' read -r _ v repo; do
            v=\$(echo \$v)
            [ \"\$v\" = \"\$have\" ] && exact=\$v
            case \"\$v:\$repo\" in \"\$upstream\"-*debian.org*) [ -n \"\$deb\" ] || deb=\$v ;; esac
            if [ -z \"\$older\" ] && dpkg --compare-versions \"\$v\" le \"\$have\"; then older=\$v; fi
        done <<EOF
\$(apt-cache madison \"\$dev\")
EOF
        pick=\${exact:-\${deb:-\$older}}
        [ -n \"\$pick\" ] || { echo \"no \$dev for \$run \$have\" >&2; exit 1; }
        echo \"\$dev=\$pick (board has \$run \$have)\" >&2
        apt-get download -q \"\$dev=\$pick\" >/dev/null
    done
    tar -cf - *.deb; cd /; rm -rf \$work" | tar -C "$debs" -xf -
for deb in "$debs"/*.deb; do
    dpkg-deb -x "$deb" "$dest"
done

# 3. Absolute symlinks (e.g. libm.so -> /lib/..., libblas.so.3 -> /etc/alternatives/...)
#    would point into the PC's own /usr. Re-point them inside the sysroot.
"${SCIP_SSH[@]}" "find /usr/lib/aarch64-linux-gnu /usr/lib/gcc/aarch64-linux-gnu/12 -type l -lname '/*' \
    -printf '%p\t' -exec readlink -f {} \; ; \
    find /usr/lib -maxdepth 1 -name '*.so*' -type l -lname '/*' -printf '%p\t' -exec readlink -f {} \;" |
while IFS=$'\t' read -r link target; do
    [[ -L "$dest$link" ]] || continue
    ln -sfn "$(realpath -m --relative-to="$(dirname "$dest$link")" "$dest$target")" "$dest$link"
done
# Also those from the unpacked .debs.
find "$dest/usr" -type l -lname '/*' | while read -r link; do
    target="$(readlink "$link")"
    ln -sfn "$(realpath -m --relative-to="$(dirname "$link")" "$dest$target")" "$link"
done

du -sh "$dest"
echo "done. Configure with: cmake --preset rk3588-release   (SCIP_SYSROOT_DIR=$dest)"
