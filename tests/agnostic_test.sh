#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-or-later
# The bridge is generic: no file in the tree, no file name and (with
# --history) no commit message may name a particular product built on it. The
# only exception is one line of README.md, exactly:
#   Used in production by <site>.
# Fails (exit 1) listing every match.
#
#   tests/agnostic_test.sh [ROOT] [--history]
set -eu
root=.
history=no
for a in "$@"; do
  case "$a" in
    --history) history=yes ;;
    *) root=$a ;;
  esac
done
cd "$root"

# Built from pieces so that this file does not match itself.
p=ygo
pattern="${p}101|${p}-core|${p}-player"
allowed="Used in production by ${p}101.com."

list_files() {
  if git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    git ls-files -co --exclude-standard
  else
    find . -type f ! -path './.git/*' ! -path './build*' ! -path '*/node_modules/*' ! -path './tests/data/*' \
      ! -path './client-ts/dist/*' ! -path './client-ts/dist-test/*' | sed 's|^\./||'
  fi
}

bad=$(mktemp)
trap 'rm -f "$bad"' EXIT
list_files | while IFS= read -r f; do
  [ -f "$f" ] || continue
  if printf '%s\n' "$f" | grep -qiE "$pattern"; then echo "file name: $f" >>"$bad"; fi
  grep -inaE "$pattern" "$f" 2>/dev/null | while IFS= read -r hit; do
    line=${hit#*:}
    if [ "$f" = README.md ] && [ "$line" = "$allowed" ]; then
      echo "allowed: $f:$hit" >>"$bad.allowed"
      continue
    fi
    echo "$f:$hit" >>"$bad"
  done
done
if [ -f "$bad.allowed" ]; then
  n=$(wc -l <"$bad.allowed" | tr -d ' ')
  [ "$n" -le 1 ] || echo "README.md: the allowed line appears $n times (at most once)" >>"$bad"
  rm -f "$bad.allowed"
fi

if [ "$history" = yes ] && git rev-parse --verify -q HEAD >/dev/null 2>&1; then
  git log --format='%H %B' HEAD | grep -iE "$pattern" | sed 's/^/commit message: /' >>"$bad" || true
fi

if [ -s "$bad" ]; then
  echo "agnostic: FAILED, the tree names a product built on the bridge:"
  cat "$bad"
  exit 1
fi
echo "agnostic: ok"
