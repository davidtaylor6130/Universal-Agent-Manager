#!/bin/sh
cd "$(dirname "$0")" || exit 1
if ! command -v python3 >/dev/null 2>&1; then
  echo 'Install Python 3 from python.org, then open this file again.'
  read -r reply
  exit 1
fi
python3 tools/build-configurator/configurator.py
