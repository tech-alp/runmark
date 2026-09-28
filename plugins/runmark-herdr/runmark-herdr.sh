#!/bin/sh
# herdr can start plugins with a narrow PATH; find node and rmk where they are
# usually installed (same list as the runmark-agent hook wrapper).
PATH="$PATH:/usr/bin:/bin:/opt/homebrew/bin:/usr/local/bin:$HOME/.local/bin"
export PATH
case "$0" in */*) dir=${0%/*} ;; *) dir=. ;; esac
node=$(command -v node) || exit 0
exec "$node" "$dir/runmark-herdr.js" "$@"
