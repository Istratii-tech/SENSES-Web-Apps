#!/bin/sh
# SENSES Web Apps (macOS): double-click to start the server for your phone.
# Keep the window open while you use the apps; Ctrl+C or closing it stops it.
cd "$(dirname "$0")" || exit 1

ok() { "$1" -c 'import sys; sys.exit(sys.version_info < (3, 8))' >/dev/null 2>&1; }
PY=
for p in /Library/Frameworks/Python.framework/Versions/Current/bin/python3 \
         /opt/homebrew/bin/python3 /usr/local/bin/python3; do
  if [ -x "$p" ] && ok "$p"; then PY=$p; break; fi
done
# /usr/bin/python3 is Apple's: without the Command Line Tools it only offers
# to install them.
if [ -z "$PY" ] && xcode-select -p >/dev/null 2>&1 && ok /usr/bin/python3; then
  PY=/usr/bin/python3
fi
if [ -z "$PY" ]; then
  echo "SENSES needs Python 3 (free). Install it from https://www.python.org/downloads/macos/"
  echo "and double-click \"Start SENSES\" again."
  echo
  echo "Для SENSES нужен Python 3 (бесплатно). Установите его с https://www.python.org/downloads/macos/"
  echo "и снова дважды нажмите «Start SENSES»."
  open "https://www.python.org/downloads/macos/"
  exit 1
fi
exec "$PY" server/senses.py "$@"
