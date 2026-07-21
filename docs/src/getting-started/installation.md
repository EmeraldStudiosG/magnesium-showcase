# Installation

This chapter gets a local Magnesium binary running. The examples assume you are in the repository root.

## Requirements

You need:

| Tool | Why it is needed |
| --- | --- |
| `git` | To clone or update the repository. |
| `make` | To run the build and test recipes. |
| A C compiler | `gcc` or `clang` on Linux/macOS; **LLVM-MinGW (clang)** on Windows. MSVC is not supported because the VM dispatch loop uses labels-as-values (computed `goto`), a GNU C extension. Install with `winget install MartinStorsjo.LLVM-MinGW.UCRT`. |
| `Node.js` and `npm` | Optional, only for the VS Code extension. |

## One-Line Installer

The fastest way to install Magnesium and the VS Code extension:

**Linux/macOS:**

```bash
bash <(curl -sL https://raw.githubusercontent.com/EmeraldStudiosG/magnesium-dev/main/install.sh)
```

**Windows (PowerShell):**

```powershell
irm https://raw.githubusercontent.com/EmeraldStudiosG/magnesium-dev/main/install.ps1 | iex
```

This installs the interpreter, shared library, C header, the `mt` toolchain manager, and the VS Code extension (if VS Code and npm are present).

## Build From Source

Clone the repository, enter it, and build:

```bash
git clone https://github.com/EmeraldStudiosG/magnesium-dev.git
cd magnesium-dev
make
```

That produces a `magnesium` executable and an `mt` toolchain binary in the repository root.

Check the version:

```bash
magnesium --version
```

## Install to a Prefix

```bash
make install PREFIX=$HOME/.magnesium
```

This installs:

| File | Description |
| --- | --- |
| `bin/magnesium` | Interpreter |
| `bin/mt` | Toolchain manager |
| `lib/libmagnesium.so` | Shared library |
| `include/magnesium.h` | C header |

Add the bin directory to your PATH:

```bash
export PATH="$HOME/.magnesium/bin:$PATH"
```

## The `mt` Toolchain Manager

`mt` installs Magnesium core and bindings from the GitHub repository.

### Install Core

```bash
mt install
```

This clones, builds, and installs the interpreter, shared library, headers, `mt` itself, and the VS Code extension (if VS Code and npm are present).

### Install Bindings

```bash
mt install rust
mt install cpp
mt install csharp
```

Each command downloads the binding source from the repository and installs it to `share/magnesium/bindings/<lang>/` under the install prefix.

After installing a binding, `mt` prints usage instructions for that language. See [Embedding and FFI](../reference/embedding.md) for details.

### Install VS Code Extension

```bash
mt install extension
```

Builds and installs the VS Code extension. This also runs automatically during `mt install` if VS Code and npm are detected.

### List Installed Components

```bash
mt list
```

Shows core, each binding, and extension install status.

### Uninstall

```bash
mt uninstall
```

Removes the entire install prefix.

### Custom Prefix

```bash
mt --prefix=/opt/magnesium install
```

The default prefix is `~/.magnesium/`. Set `MAGNESIUM_PREFIX` or pass `--prefix=<path>` to override.

## Run a File

Create `hello.mg`:

```magnesium
print("Hello from Magnesium")
```

Run it:

```bash
magnesium hello.mg
```

## Check Without Running

Use `--check` when you want to compile a file and catch syntax or compile errors without executing top-level code:

```bash
magnesium --check hello.mg
```

On success, Magnesium prints `Syntax OK`.

## Build Bytecode

Magnesium can compile a `.mg` file to `.mgc` bytecode:

```bash
magnesium build hello.mg
```

This writes `hello.mgc`. You can run that file directly:

```bash
magnesium hello.mgc
```

Bytecode is versioned. When the VM bytecode format changes, old `.mgc` files must be rebuilt.

## Time a Script

Use `--time` for quick local measurement:

```bash
magnesium --time hello.mg
```

For real performance work, use the benchmark runner in `scripts/run_bench.sh` because it runs the benchmark set consistently and rebuilds compiled artifacts.

Next: [Quick Start](quick-start.md).
