#!/usr/bin/env sh
set -eu

if [ "$#" -ne 1 ]; then
    echo "usage: $0 <destdir>" >&2
    exit 2
fi

dest=$1
repo=https://github.com/gregrahn/sqllogictest.git

mkdir -p "$dest"

if [ -d "$dest/test" ]; then
    echo "$dest/test already exists"
    exit 0
fi

tmp=$(mktemp -d "${TMPDIR:-/tmp}/sqllogictest.XXXXXX")
cleanup() {
    rm -rf "$tmp"
}
trap cleanup EXIT HUP INT TERM

git clone --depth 1 "$repo" "$tmp/sqllogictest"
mv "$tmp/sqllogictest/test" "$dest/test"

echo "sqllogictest corpus written to $dest/test"
