#!/bin/sh
set -eu
# Only fixed capture/output flags may reach the native executable.
case ${1:-} in
  --clipboard) [ "$#" -eq 1 ] || exit 2 ;;
  --capture-region|--capture-window|--capture-fullscreen)
    for flag in "$@"; do
      case "$flag" in
        --capture-region|--capture-window|--capture-fullscreen|--copy|--save) ;;
        *) exit 2 ;;
      esac
    done ;;
  *) exit 2 ;;
esac
# shortcut: 200 ms unmap grace, increase if compositor captures closing panels.
sleep 0.2
exec nirisnap "$@"
