#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
generated=$(mktemp)
trap 'rm -f "$generated"' EXIT HUP INT TERM
gzip -n -9 -c "$root/web/index.html" >"$generated"
cmp "$generated" "$root/web/index.html.gz"
gzip -t "$root/web/index.html.gz"
node -e 'const fs=require("fs"),z=require("zlib");const s=z.gunzipSync(fs.readFileSync(process.argv[1])).toString();const m=s.match(/<script>([\s\S]*?)<\/script>/);if(!m)throw Error("missing inline script");new Function(m[1]);' "$root/web/index.html.gz"
echo "web asset checks passed"
