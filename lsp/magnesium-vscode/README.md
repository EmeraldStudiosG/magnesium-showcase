# Magnesium Language Server

Language server extension for [Visual Studio Code](https://code.visualstudio.com/) providing rich editing support for the [Magnesium](https://github.com/EmeraldStudiosG/magnesium-showcase) programming language.

## Features

- **Diagnostics** - compile errors shown inline as you type
- **Autocomplete** - keywords, builtins, stdlib modules, and document symbols
- **Hover** - descriptive hints for keywords, functions, and stdlib members
- **Go to Definition** - jump to local symbol declarations
- **Signature Help** - parameter info when writing function calls
- **Rename** - rename symbols across the document
- **Formatting** - auto-indent with consistent 4-space style
- **Syntax Highlighting** - TextMate grammar with semantic token support
- **Magnesium Dark Theme** - included dark color theme

## Installation

### Via `mt` (recommended)

```bash
mt install extension
```

### Manual

1. Download the latest `.vsix` from [releases](https://github.com/EmeraldStudiosG/magnesium-showcase/releases)
2. Run `code --install-extension magnesium-vscode-3.6.0.vsix`

## Configuration

| Setting | Default | Description |
| --- | --- | --- |
| `magnesium.executablePath` | `"magnesium"` | Path to the `magnesium` binary used by the language server |

## Commands

- **Magnesium: Restart Language Server** - restarts the LSP connection

## Requirements

The `magnesium` interpreter must be installed and on your PATH (or configured via `magnesium.executablePath`). Install it with:

```bash
mt install
```
