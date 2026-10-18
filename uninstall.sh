#!/bin/sh
# Remove only the files owned by this installer. Keep all user/device config.
set -eu
prefix=${PREFIX:-/usr/local}
stage=${DESTDIR:-}
case "$#" in
   0) ;;
   2) [ "$1" = --prefix ] || { echo 'Usage: ./uninstall.sh [--prefix PATH]' >&2; exit 2; }; prefix=$2 ;;
   *) echo 'Usage: ./uninstall.sh [--prefix PATH]' >&2; exit 2 ;;
esac
case "$prefix" in /*) ;; *) echo 'PREFIX must be an absolute path.' >&2; exit 2 ;; esac
case "$stage" in ''|/*) ;; *) echo 'DESTDIR must be empty or an absolute path.' >&2; exit 2 ;; esac
prefix=${prefix%/}
stage=${stage%/}
target="$stage$prefix"

rm -f -- "$target/bin/htop_plus" "$target/libexec/htop_plus/htop" \
   "$target/share/htop_plus/htoprc.example" "$target/share/htop_plus/uninstall.sh" \
   "$target/share/doc/htop_plus/COPYING" "$target/share/doc/htop_plus/README.md" \
   "$target/share/man/man1/htop_plus.1"
for directory in libexec/htop_plus share/htop_plus share/doc/htop_plus; do
   rmdir -- "$target/$directory" 2>/dev/null || :
done
echo "Removed htop_plus from ${prefix:-/}. User layout and /etc/nvpowermon were preserved."
