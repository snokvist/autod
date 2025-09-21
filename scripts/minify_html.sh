#!/usr/bin/env bash
set -euo pipefail
UI_IN="${1:-ui.html}"
UI_MIN="ui.min.html"
UI_GZ="ui.min.html.gz"
UI_HDR="ui_embed.h"

if command -v npx >/dev/null 2>&1; then
  npx html-minifier-terser@7 \
    --collapse-whitespace \
    --remove-comments \
    --remove-optional-tags \
    --remove-redundant-attributes \
    --use-short-doctype \
    --minify-css true \
    --minify-js true \
    -o "$UI_MIN" "$UI_IN"
else
  python3 - "$UI_IN" "$UI_MIN" <<'PY'
import re,sys,io
src=sys.argv[1]; dst=sys.argv[2]
s=io.open(src,'r',encoding='utf-8').read()
s=re.sub(r'<!--.*?-->', '', s, flags=re.S)
s=re.sub(r'>\s+<', '><', s)
s=re.sub(r'\s{2,}', ' ', s)
io.open(dst,'w',encoding='utf-8').write(s.strip())
PY
fi

gzip -9 -c "$UI_MIN" > "$UI_GZ"
xxd -i "$UI_GZ" > "$UI_HDR"
echo "Wrote $UI_HDR (embed this header and serve it with Content-Encoding: gzip)"
