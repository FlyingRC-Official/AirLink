#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
page="$script_dir/AirLink-Configurator.html"

if [ ! -f "$page" ]; then
  printf '\nMissing AirLink-Configurator.html. Keep the HTML file beside this launcher.\n\n' >&2
  printf 'Press Return to close. '
  read -r _
  exit 1
fi

if [ "${AIRLINK_CONFIGURATOR_DRY_RUN:-0}" = "1" ]; then
  printf 'AirLink macOS configurator launcher validation passed.\n'
  exit 0
fi

for browser in "Google Chrome" "Microsoft Edge"; do
  if open -Ra "$browser" >/dev/null 2>&1; then
    open -a "$browser" "$page"
    exit 0
  fi
done

printf '\nGoogle Chrome or Microsoft Edge was not found.\n' >&2
printf 'Install one of them, or right-click AirLink-Configurator.html and choose Open With.\n' >&2
printf 'https://www.google.com/chrome/\nhttps://www.microsoft.com/edge/download\n' >&2
printf '\nPress Return to close. '
read -r _
exit 1
