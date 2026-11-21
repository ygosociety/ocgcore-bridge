#!/bin/sh
# Fetches the card data the golden tests are pinned to (tests/golden/pins.json)
# into tests/data/: CardScripts (the Lua scripts) and BabelCDB (cards.cdb).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
data="${1:-$here/../data}"
fetch() { # dir repo commit
  if [ -d "$1/.git" ] && [ "$(git -C "$1" rev-parse HEAD)" = "$3" ]; then return; fi
  rm -rf "$1"
  mkdir -p "$1"
  git -C "$1" init -q
  git -C "$1" remote add origin "$2"
  git -C "$1" fetch -q --depth 1 origin "$3"
  git -C "$1" checkout -q FETCH_HEAD
}
pin() { node -e "process.stdout.write(require('$here/pins.json')['$1']['$2'])"; }
fetch "$data/CardScripts" "$(pin cardScripts repo)" "$(pin cardScripts commit)"
fetch "$data/BabelCDB" "$(pin babelCdb repo)" "$(pin babelCdb commit)"
echo "card data in $data"
