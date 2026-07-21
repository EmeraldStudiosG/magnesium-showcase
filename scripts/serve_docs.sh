#!/bin/bash

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$REPO_ROOT/docs"

if ! command -v mdbook &>/dev/null; then
    echo "Error: mdbook not found. Install with: cargo install mdbook"
    exit 1
fi

echo "Starting Magnesium docs at http://0.0.0.0:3000"
MG_BINARY="$REPO_ROOT/magnesium" mdbook serve -n 0.0.0.0 -p 3000
