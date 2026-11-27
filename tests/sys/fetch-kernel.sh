#!/bin/sh
# Fetch an Ubuntu mainline kernel build (amd64, generic) and extract its
# image and modules under DESTDIR (default /), without package scripts.
# Prints the kernel release, for vng -r.
#   usage: fetch-kernel.sh <version, e.g. v7.1.13> [cache dir]
set -eu

version="$1"
cache="${2:-kernel-cache}"
destdir="${DESTDIR:-/}"
base="https://kernel.ubuntu.com/mainline/$version/amd64"

mkdir -p "$cache"
if ! ls "$cache"/linux-image-unsigned-*.deb >/dev/null 2>&1; then
    list=$(curl -fsSL "$base/" | grep -o 'href="linux-\(image-unsigned\|modules\)-[^"]*-generic_[^"]*_amd64\.deb"' \
           | sed 's/^href="//; s/"$//' | sort -u)
    [ -n "$list" ] || { echo "no generic amd64 build for $version" >&2; exit 1; }
    for deb in $list; do
        curl -fsSL -o "$cache/$deb" "$base/$deb"
    done
fi

extract() {
    if command -v dpkg-deb >/dev/null; then
        dpkg-deb -x "$1" "$destdir"
    else
        member=$(ar t "$1" | grep '^data\.tar')
        ar p "$1" "$member" | tar -x -a -f - -C "$destdir" 2>/dev/null \
            || ar p "$1" "$member" | tar --zstd -x -f - -C "$destdir"
    fi
}

mkdir -p "$destdir"
for deb in "$cache"/linux-image-unsigned-*.deb "$cache"/linux-modules-*.deb; do
    extract "$deb"
done

release=$(ls "$cache"/linux-image-unsigned-*.deb | head -n 1 \
          | sed 's/.*linux-image-unsigned-\([^_]*\)_.*/\1/')
# merged-usr packages: modules are under usr/lib
[ -e "$destdir/lib" ] || ln -s usr/lib "$destdir/lib"
depmod -b "$destdir" "$release"
echo "$release"
