#!/usr/bin/env bash
# Rejects code that reports raw machine values instead of what they mean.
#
# This project is a reverse-engineered game mod: no documentation, and every
# value is an opaque id until someone names it. A log that prints 0x00C4 or
# 65535 or mode=1 has to be decoded by hand on every single read, and a decode
# that gets skipped once produces a wrong conclusion. That has cost real hours
# here, repeatedly.
#
# The rule: if a number has a meaning, the meaning is what appears.

set -u

payload=$(cat)
file=$(printf '%s' "$payload" | sed -n 's/.*"file_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
file=${file//\\\\//}

case "$file" in
  *src/*.cpp|*src/*.h|*test/*.cpp|*test/*.h) ;;
  *) exit 0 ;;
esac

[ -f "$file" ] || exit 0

violations=""

add() {
  violations="${violations}  $1
"
}

# Strip comment-only lines and block-comment bodies before checking, so
# documentation that cites a raw game field stays legal -- that is exactly where
# the raw name belongs.
code_only=$(sed -e 's://.*::' -e '/^[[:space:]]*\*/d' -e '/^[[:space:]]*\/\*/d' "$file")

# 1. Hex printed into a log. There is never a good reason: name it.
while IFS= read -r line; do
  [ -n "$line" ] && add "prints hex to a log -- print the name instead: $line"
done < <(printf '%s\n' "$code_only" | grep -nE '"[^"]*0x%' | head -5)

# 2. Raw offset-named game fields used in code. A named accessor citing the raw
#    name in its comment keeps it greppable against the decomp.
while IFS= read -r line; do
  [ -n "$line" ] && add "raw field name in code -- wrap it in a named accessor: $line"
done < <(printf '%s
' "$code_only" | grep -nE 'field_0x[0-9a-f]+' | grep -vE '^[0-9]+:[[:space:]]*return ' | head -5)

# 3. Bare hex literals outside a named constant definition.
while IFS= read -r line; do
  [ -n "$line" ] && add "unnamed hex literal -- give it a named constant: $line"
done < <(printf '%s
' "$code_only" | grep -nE '0x[0-9A-Fa-f]{2,}' | grep -vE 'static const|0x%|^[0-9]+:[[:space:]]*return ' | head -5)

if [ -n "$violations" ]; then
  printf 'Legibility check on %s\n\n%s\nEvery value a human reads must say what it means, not what it is.\n' \
    "$(basename "$file")" "$violations" >&2
  exit 2
fi

exit 0
