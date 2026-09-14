#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run tests with an ephemeral keyring. Never touch the user's real credentials.
set -euo pipefail
if [[ ${SUPERSMART_KEYRING_CHILD:-} != 1 ]]; then
  temporary=$(mktemp -d /tmp/supersmart-keyring.XXXXXXXX)
  trap 'rm -rf -- "$temporary"' EXIT
  export XDG_DATA_HOME="$temporary/data" XDG_CONFIG_HOME="$temporary/config"
  export XDG_RUNTIME_DIR="$temporary/runtime"
  mkdir -p "$XDG_DATA_HOME" "$XDG_CONFIG_HOME" "$XDG_RUNTIME_DIR"
  chmod 700 "$XDG_RUNTIME_DIR"
  export SUPERSMART_KEYRING_CHILD=1
  exec_status=0
  dbus-run-session -- bash "$0" "$@" || exec_status=$?
  exit "$exec_status"
fi
printf '%s' 'temporary-test-keyring' | gnome-keyring-daemon --unlock --components=secrets >/dev/null
export SUPERSMART_TEST_KEYRING=1
"$@"
