#!/bin/sh
# Finds a prebuilt set of VLC's third-party libraries on VideoLAN's CI server.
#   tools-wlc/find-prebuilt.sh macos-arm64 aarch64-apple-darwin19
# Prints the URL of the first bundle that exists, or nothing.
job=$1 triplet=$2
here=$(dirname "$0")
grep -v '^#' "$here/contrib-shas.txt" | while read -r sha; do
    [ -n "$sha" ] || continue
    url="https://artifacts.videolan.org/vlc-3.0/$job/vlc-contrib-$triplet-$sha.tar.zst"
    if curl -fsIL --retry 2 -m 20 -o /dev/null "$url"; then
        echo "$url"
        exit 0
    fi
done
