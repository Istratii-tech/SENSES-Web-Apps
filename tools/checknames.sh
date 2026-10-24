#!/bin/bash
# The personal-data gate (ported from SENSES): no personal data, no names of
# the author, their machines, network or employer in what is published.
#
#   tools/checknames.sh              the files under git
#   tools/checknames.sh PATH...      these files and folders (a release unpacked)
#
# Exit 0 clean, 1 found. It always says how many files it checked: "clean"
# without a number cannot be told from "did not look".
#
# Two halves:
#  1. the words of tools/badwords.b64 (values known in advance). The list is
#     base64: a gate that looks for a word must contain it, and the repository
#     would fail its own check. Not a secret, only out of the text:
#       { base64 -d < tools/badwords.b64; echo 'new'; } | base64 > /tmp/w && mv /tmp/w tools/badwords.b64
#     A word with a capital letter is matched case-sensitively, others not.
#  2. tools/checkpatterns.py: data by its form (MAC, IMEI, ICCID, private
#     addresses, mail, home folders), on the same files, output masked.
#
# Binary images (a VM's .data, system.img, .wasm) are read through
# tools/unpackimage.py: their strings and every DEFLATEd zip entry inside
# them (jars and APKs in /system), so compressed bytes hide nothing.
# Other compressed containers (.zip, .jar, .apk, .gz) are skipped and
# COUNTED: check them unpacked.
set -uo pipefail
cd "$(dirname "$0")/.."
LIST=tools/badwords.b64
FOUND=0
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
[ -f "$LIST" ] || { echo "⛔ нет списка $LIST"; exit 1; }
words=$(base64 -d < "$LIST")
[ -n "$words" ] || { echo "⛔ список пуст"; exit 1; }

# Not checked under git (only then): the list itself, the gate's own test
# (it builds dirty samples), and the internal notes (never published,
# tools/make_publication.sh leaves them out).
SKIP='(^|/)(\.git|build)/|(^|/)badwords\.b64$|(^|/)tests/checknames/|^docs/|^tools/move_worktree_to_dt2\.sh$'
if [ $# -eq 0 ]; then
  git rev-parse --git-dir >/dev/null 2>&1 || { echo "⛔ не репозиторий git — укажите пути"; exit 1; }
  # core.quotePath=false: otherwise names with non-ASCII letters come quoted
  # and escaped, no such file exists, and they are silently not checked.
  files=$(git -c core.quotePath=false ls-files | grep -vE "$SKIP")
  what="под учётом git"
else
  files=$(find "$@" -type f 2>/dev/null)
  if [ $# -gt 1 ]; then what="$1 и ещё $(($# - 1))"; else what="$1"; fi
fi

TODO="$WORK/todo"
: > "$TODO"
skipped_zip=0
images=0
inflated=0
while IFS= read -r f; do
  [ -n "$f" ] || continue
  case "$f" in
    *.data|*.img|*.wasm)
      images=$((images + 1))
      out="$WORK/img$images.unpacked/$(basename "$f")"
      mkdir -p "$out"
      line=$(python3 tools/unpackimage.py "$f" "$out" 2>&1) || { echo "⛔ образ не прочитан: $f"; FOUND=1; continue; }
      inflated=$((inflated + $(printf '%s' "$line" | sed -n 's/.*: \([0-9]*\) zip entries inflated.*/\1/p')))
      find "$out" -type f >> "$TODO"
      printf '%s\t%s\n' "$out" "$f" >> "$WORK/origins"
      continue ;;
    *.jar|*.zip|*.apk|*.apks|*.aar|*.gz|*.xz|*.bz2|*.7z|*.tgz)
      skipped_zip=$((skipped_zip + 1)); continue ;;
  esac
  printf '%s\n' "$f" >> "$TODO"
done <<< "$files"

# Where a hit in an image's strings came from.
origin() {
  local p=$1
  if [ -f "$WORK/origins" ]; then
    while IFS=$'\t' read -r dir img; do
      case "$p" in "$dir"/*) printf '%s (из образа %s)' "${p#"$dir"/}" "$img"; return ;; esac
    done < "$WORK/origins"
  fi
  printf '%s' "$p"
}

# The words in two groups (case-insensitive / as written), one grep each per
# file. In binary files only a word that starts a token (a string constant
# does), and a short word must go on: three letters in compressed tables are
# noise, not a leak.
printf '%s\n' "$words" | grep -v '[A-Z]' | grep . > "$WORK/w-ci"
printf '%s\n' "$words" | grep '[A-Z]' > "$WORK/w-cs"
bin_re() {
  local w esc tail out=""
  while IFS= read -r w; do
    [ -n "$w" ] || continue
    esc=$(printf '%s' "$w" | sed 's/[][\.*^$(){}?+|/]/\\&/g')
    # (not a digit: three letters and a digit are bytes of tables in images;
    # a leak goes on as a path or a name, with "/" or "_")
    if [ ${#w} -le 3 ]; then tail='[A-Za-z_./-]'; else tail=''; fi
    out="$out${out:+|}$esc$tail"
  done < "$1"
  printf '(^|[^A-Za-z0-9])(%s)' "$out"
}
RE_CI=$(bin_re "$WORK/w-ci"); RE_CS=$(bin_re "$WORK/w-cs")
n=0
unreadable=0
while IFS= read -r f; do
  [ -n "$f" ] || continue
  if [ ! -r "$f" ]; then
    FOUND=1; unreadable=$((unreadable + 1))
    printf '⛔ не прочитан (а значит и не проверен): %s\n' "$f"; continue
  fi
  n=$((n + 1))
  # The strings of an image are read as the binary they came from.
  if case "$f" in *.unpacked/*) false ;; *) true ;; esac && { grep -qI -- '' "$f" 2>/dev/null || [ ! -s "$f" ]; }; then
    hits=$( { [ -s "$WORK/w-ci" ] && grep -aniF -f "$WORK/w-ci" -- "$f"; \
              [ -s "$WORK/w-cs" ] && grep -anF -f "$WORK/w-cs" -- "$f"; } 2>/dev/null)
  else
    hits=$( { [ -s "$WORK/w-ci" ] && grep -aoiE -- "$RE_CI" "$f"; \
              [ -s "$WORK/w-cs" ] && grep -aoE -- "$RE_CS" "$f"; } 2>/dev/null | sort -u)
  fi
  if [ -n "$hits" ]; then
    FOUND=1
    printf '⛔ %s\n' "$(origin "$f")"
    printf '%s\n' "$hits" | head -3 | cut -c1-160 | sed 's/^/     /'
  fi
done < "$TODO"

pat_n=0
if [ "$n" -gt 0 ]; then
  pat_out=$(python3 tools/checkpatterns.py --list "$TODO" 2>&1); pat_rc=$?
  pat_n=$(printf '%s\n' "$pat_out" | sed -n 's/^шаблоны: проверено файлов //p' | tail -1)
  if [ "$pat_rc" != 0 ]; then
    FOUND=1
    printf '%s\n' "$pat_out" | grep -v '^шаблоны: проверено файлов ' | while IFS= read -r l; do
      case "$l" in '⛔ '*) printf '⛔ %s\n' "$(origin "${l#⛔ }")" ;; *) printf '%s\n' "$l" ;; esac
    done
    echo "⛔ найдены личные данные по форме (MAC, IMEI, ICCID, частный адрес, почта, домашний каталог)"
  elif [ "${pat_n:-0}" != "$n" ]; then
    FOUND=1
    echo "⛔ шаблоны проверили ${pat_n:-0} файлов, а список слов — $n: половины заставы смотрят на разное"
  fi
fi

note=""
[ "$images" -gt 0 ] && note="$note; образов развёрнуто: $images (записей zip распаковано: $inflated)"
[ "$skipped_zip" -gt 0 ] && note="$note; сжатых контейнеров пропущено: $skipped_zip — их проверять распакованными"
if [ "$FOUND" = 0 ]; then
  echo "✓ чисто: запрещённых слов нет, личных данных по форме нет (проверено файлов: $n, шаблонами: ${pat_n:-0}, $what$note)"
else
  echo "⛔ найдено — не публиковать (проверено файлов: $n, $what$note)"
fi
exit "$FOUND"
