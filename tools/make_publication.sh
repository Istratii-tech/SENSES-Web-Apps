#!/bin/sh
# The public repository of SENSES Web Apps (topic local-release): what is
# under git in this checkout, without the internal notes and tools, plus the
# publication's README and NOTICE, as a commit in OUT -- a repository of its
# own with a fresh history signed istratiitech. The personal-data gate
# (tools/checknames.sh) checks OUT before the commit; anything found, no
# commit. git push is for a person to do.
#
#   tools/make_publication.sh OUT [VERSION]      (make publication PUBLICATION=OUT)
#
# OUT must be outside this checkout. Only committed files are taken (a clean
# working tree is required): what is published is what history here has.
set -eu
OUT=${1:?usage: tools/make_publication.sh OUT [VERSION]}
VERSION=${2:-$(date +%Y.%m.%d)}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
case "$(cd "$(dirname "$OUT")" 2>/dev/null && pwd)/" in
  "$ROOT"/*) echo "⛔ OUT must be outside this checkout"; exit 1 ;;
esac
[ -z "$(git status --porcelain --untracked-files=no)" ] || { echo "⛔ commit first: only committed files are published"; exit 1; }

# Not published: the internal notes (journals of the work, with machines,
# paths and devices of the author) and the tools of that work.
EXCLUDE='^docs/|^tools/move_worktree_to_dt2\.sh$|^tools/release/publication/'

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
find "$OUT" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
git -c core.quotePath=false ls-files -z | tr '\0' '\n' | grep -vE "$EXCLUDE" | tr '\n' '\0' |
  tar --null -T - -cf - | tar -xf - -C "$OUT"
mv "$OUT/README.md" "$OUT/TECHNICAL.md"
cp tools/release/publication/README.md tools/release/publication/NOTICE "$OUT/"
[ -f tools/release/publication/LICENSE ] && cp tools/release/publication/LICENSE "$OUT/"

cd "$OUT"
[ -d .git ] || git init -q -b main
git config user.name istratiitech
git config user.email istratiitech@users.noreply.github.com
git add -A
echo "== the personal-data gate on $OUT"
if ! tools/checknames.sh; then
  echo "⛔ not committed: clean what the gate found (in the source checkout) and run again"
  exit 1
fi
if git diff --cached --quiet; then
  echo "nothing changed since the last publication"
else
  git commit -q -m "SENSES Web Apps $VERSION"
  echo "committed: $(git log -1 --format='%h %an <%ae> %s')"
fi
echo "files: $(git ls-files | wc -l | tr -d ' '); to publish: cd \"$OUT\" && git push"
