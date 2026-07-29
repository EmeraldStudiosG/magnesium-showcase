#!/usr/bin/env bash
set -euo pipefail

REPO_URL="https://github.com/EmeraldStudiosG/magnesium-showcase.git"
BRANCH="${MAGNESIUM_BRANCH:-main}"
PREFIX="${MAGNESIUM_PREFIX:-$HOME/.magnesium}"

echo "=== Magnesium Installer ==="

if ! command -v git &>/dev/null; then
    echo "Error: git is required. Install it first."
    exit 1
fi

if ! command -v gcc &>/dev/null && ! command -v cc &>/dev/null; then
    echo "Error: a C compiler (gcc/cc) is required. Install it first."
    exit 1
fi

if ! command -v make &>/dev/null; then
    echo "Error: make is required. Install it first."
    exit 1
fi

C_COMPILER="$(command -v gcc || command -v cc)"

echo "Cloning repository..."
TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT
git clone --depth 1 --branch "$BRANCH" "$REPO_URL" "$TMPDIR/magnesium"

echo "Building magnesium..."
make -C "$TMPDIR/magnesium" CC="$C_COMPILER" 2>&1

echo "Installing to $PREFIX..."
make -C "$TMPDIR/magnesium" install CC="$C_COMPILER" PREFIX="$PREFIX" 2>&1

# VS Code extension
if command -v code &>/dev/null && command -v npm &>/dev/null; then
    echo ""
    echo "Installing VS Code extension..."
    LSP_DIR="$TMPDIR/magnesium/lsp/magnesium-vscode"
    if [ -d "$LSP_DIR" ]; then
        (cd "$LSP_DIR" && npm install 2>&1 && npx tsc 2>&1)
        if command -v vsce &>/dev/null; then
            (cd "$LSP_DIR" && vsce package --allow-missing-repository 2>&1)
        else
            (cd "$LSP_DIR" && npx @vscode/vsce package --allow-missing-repository 2>&1)
        fi
        VSIX=$(ls -t "$LSP_DIR"/*.vsix 2>/dev/null | head -1)
        if [ -n "$VSIX" ]; then
            code --install-extension "$VSIX" 2>&1
            mkdir -p "$PREFIX/share/magnesium"
            echo "1" > "$PREFIX/share/magnesium/extension-installed"
            echo "VS Code extension installed!"
        fi
    fi
else
    echo ""
    echo "VS Code or npm not found. Skipping extension."
    echo "Run 'mt install extension' later."
fi

echo ""
echo "Installed:"
echo "  $PREFIX/bin/magnesium  - interpreter"
echo "  $PREFIX/bin/mt         - toolchain manager"
echo "  $PREFIX/lib/libmagnesium.so - shared library"
echo "  $PREFIX/include/magnesium.h - C header"
echo ""
echo "To install bindings, run:"
echo "  mt install rust"
echo "  mt install cpp"
echo "  mt install csharp"
echo ""

BIN_DIR="$PREFIX/bin"
if ! echo "$PATH" | grep -q "$BIN_DIR"; then
    echo "NOTE: $BIN_DIR is not in your PATH. Add it with:"
    echo "  echo 'export PATH=\"$BIN_DIR:\$PATH\"' >> ~/.bashrc && source ~/.bashrc"
fi

echo "Done! Run 'magnesium --version' or 'mt list' to verify."
