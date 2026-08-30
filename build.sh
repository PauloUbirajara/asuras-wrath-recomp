#!/usr/bin/env bash
set -e

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$REPO_ROOT/scripts/build_linux.sh" "$@"
