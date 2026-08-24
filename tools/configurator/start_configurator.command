#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cd "$script_dir"

if ! command -v node >/dev/null 2>&1; then
  printf '\nAirLink Configurator requires Node.js 18 or newer.\n'
  printf 'Install Node.js from https://nodejs.org/ and run this file again.\n\n'
  printf 'Press Return to close. '
  read -r _
  exit 1
fi

exec node server.mjs --open
