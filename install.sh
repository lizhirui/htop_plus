#!/bin/sh
# Install this fork under its own name, independently of the system's htop.
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
prefix=${PREFIX:-/usr/local}
stage=${DESTDIR:-}
build=yes

usage() {
   echo 'Usage: ./install.sh [--prefix PATH] [--no-build]'
   echo 'Defaults: PREFIX=/usr/local; optional DESTDIR for staged installations.'
   echo 'Builds when needed, then installs htop_plus, its private binary, defaults, docs and man page.'
}

while [ "$#" -gt 0 ]; do
   case "$1" in
      --prefix) [ "$#" -ge 2 ] || { usage >&2; exit 2; }; prefix=$2; shift 2 ;;
      --prefix=*) prefix=${1#*=}; shift ;;
      --no-build) build=no; shift ;;
      -h|--help) usage; exit 0 ;;
      *) usage >&2; exit 2 ;;
   esac
done
case "$prefix" in /*) ;; *) echo 'PREFIX must be an absolute path.' >&2; exit 2 ;; esac
case "$stage" in ''|/*) ;; *) echo 'DESTDIR must be empty or an absolute path.' >&2; exit 2 ;; esac
prefix=${prefix%/}
stage=${stage%/}
target="$stage$prefix"

if [ "$build" = yes ]; then
   if [ ! -f "$project_dir/Makefile" ]; then
      (
         cd "$project_dir"
         ./autogen.sh
         CC="${CC:-/usr/bin/gcc}" ./configure --enable-unicode
      )
   fi
   make -s -C "$project_dir" -j "${JOBS:-8}"
fi
if [ ! -x "$project_dir/htop" ]; then
   echo 'No htop binary found. Run ./install.sh without --no-build first.' >&2
   exit 1
fi

# Preflight all inputs before creating or replacing installed files.
for file in htop_plus htoprc.example COPYING docs/htop_plus.md docs/htop_plus.1 uninstall.sh; do
   if [ ! -f "$project_dir/$file" ]; then
      echo "Missing installation input: $file" >&2
      exit 1
   fi
done

install -d -m 0755 "$target/bin" "$target/libexec/htop_plus" \
   "$target/share/htop_plus" "$target/share/doc/htop_plus" "$target/share/man/man1"
temporary=
trap '[ -z "$temporary" ] || rm -f -- "$temporary"' EXIT
trap 'exit 1' HUP INT TERM

install_file() {
   # Rename into place so upgrades also work while the previous ELF is running.
   temporary=$(mktemp "$(dirname -- "$2")/.htop_plus.XXXXXX")
   install -m "$3" -- "$project_dir/$1" "$temporary"
   mv -f -- "$temporary" "$2"
   temporary=
}

install_file htop "$target/libexec/htop_plus/htop" 0755
install_file htoprc.example "$target/share/htop_plus/htoprc.example" 0644
install_file COPYING "$target/share/doc/htop_plus/COPYING" 0644
install_file docs/htop_plus.md "$target/share/doc/htop_plus/README.md" 0644
install_file docs/htop_plus.1 "$target/share/man/man1/htop_plus.1" 0644
install_file uninstall.sh "$target/share/htop_plus/uninstall.sh" 0755
install_file htop_plus "$target/bin/htop_plus" 0755

echo "Installed: $target/bin/htop_plus"
echo 'Run: htop_plus'
echo 'Layout: ${XDG_CONFIG_HOME:-$HOME/.config}/htop_plus/htoprc (created on first run)'
echo 'Cable configuration: /etc/nvpowermon/config.json'
