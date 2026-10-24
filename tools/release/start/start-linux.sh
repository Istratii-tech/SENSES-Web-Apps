#!/bin/sh
# SENSES Web Apps (Linux): ./start-senses.sh starts the server for your phone.
# Keep it running while you use the apps; Ctrl+C stops it.
cd "$(dirname "$0")" || exit 1
if ! python3 -c 'import sys; sys.exit(sys.version_info < (3, 8))' >/dev/null 2>&1; then
  echo "SENSES needs Python 3.8 or newer: install python3 with your package manager (e.g. sudo apt install python3)."
  echo "Для SENSES нужен Python 3.8 или новее: установите python3 (например, sudo apt install python3)."
  exit 1
fi
exec python3 server/senses.py "$@"
