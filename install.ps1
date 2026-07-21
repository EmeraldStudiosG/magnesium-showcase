Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$RepoUrl = "https://github.com/EmeraldStudiosG/magnesium-dev.git"
$InstallDir = if ($env:MAGNESIUM_DIR) { $env:MAGNESIUM_DIR } else { "$env:USERPROFILE\.magnesium" }
$BinDir = if ($env:MAGNESIUM_BIN_DIR) { $env:MAGNESIUM_BIN_DIR } else { "$env:USERPROFILE\.local\bin" }
$Branch = if ($env:MAGNESIUM_BRANCH) { $env:MAGNESIUM_BRANCH } else { "main" }

Write-Host "=== Magnesium Installer (Windows) ===" -ForegroundColor Cyan

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Host "Error: git is required. Install from https://git-scm.com" -ForegroundColor Red
    exit 1
}

$isWindows = ($env:OS -eq "Windows_NT")
if ($isWindows) {
    if (-not (Get-Command clang -ErrorAction SilentlyContinue)) {
        Write-Host "Error: LLVM-MinGW (clang) is required to build Magnesium on Windows." -ForegroundColor Red
        Write-Host "Install: winget install MartinStorsjo.LLVM-MinGW.UCRT" -ForegroundColor Red
        Write-Host "MSVC (cl.exe) is not supported: Magnesium uses computed-goto dispatch (labels-as-values)." -ForegroundColor Red
        exit 1
    }
} else {
    if (-not (Get-Command gcc -ErrorAction SilentlyContinue) -and
        -not (Get-Command clang -ErrorAction SilentlyContinue)) {
        Write-Host "Error: a C compiler (gcc or clang) is required." -ForegroundColor Red
        exit 1
    }
}

Write-Host "Cloning repository..."
$TmpDir = Join-Path $env:TEMP "magnesium-install-$(Get-Random)"
git clone --depth 1 --branch $Branch $RepoUrl "$TmpDir\magnesium"

Write-Host "Building magnesium..."
Push-Location "$TmpDir\magnesium"

if ($isWindows) {
    $cc = "clang"
} else {
    $cc = if (Get-Command gcc -ErrorAction SilentlyContinue) { "gcc" } else { "clang" }
}

$makeCmd = $null
if (Get-Command mingw32-make -ErrorAction SilentlyContinue) { $makeCmd = "mingw32-make" }
elseif (Get-Command make -ErrorAction SilentlyContinue) { $makeCmd = "make" }

if ($makeCmd) {
    & $makeCmd CC=$cc
    & $makeCmd CC=$cc lib
    & $makeCmd CC=$cc mt
} else {
    if ($env:OS -eq "Windows_NT") {
        & $cc -Wall -Wextra -std=c11 -O3 -Isrc -D_CRT_SECURE_NO_WARNINGS src/main.c src/lexer.c src/parser.c src/compiler.c src/typecheck.c src/object.c src/gc.c src/vm.c src/serialize.c src/lsp.c -o magnesium.exe
        & $cc -Wall -Wextra -std=c11 -O3 -Isrc -D_CRT_SECURE_NO_WARNINGS -fPIC src/lexer.c src/parser.c src/compiler.c src/typecheck.c src/object.c src/gc.c src/vm.c src/serialize.c src/lsp.c src/mg_ffi_glue.c -o libmagnesium.dll -shared -lm -lws2_32
        & $cc -Wall -Wextra -std=c11 -O3 -Isrc -D_CRT_SECURE_NO_WARNINGS src/mt.c -o mt.exe
    } else {
        & $cc -Wall -Wextra -std=c11 -O3 -Isrc -D_GNU_SOURCE src/main.c src/lexer.c src/parser.c src/compiler.c src/typecheck.c src/object.c src/gc.c src/vm.c src/serialize.c src/lsp.c -o magnesium -lm -ldl -pthread
        & $cc -Wall -Wextra -std=c11 -O3 -Isrc -D_GNU_SOURCE -fPIC src/lexer.c src/parser.c src/compiler.c src/typecheck.c src/object.c src/gc.c src/vm.c src/serialize.c src/lsp.c src/mg_ffi_glue.c -o libmagnesium.so -shared -lm -ldl -pthread
        & $cc -Wall -Wextra -std=c11 -O3 -Isrc -D_GNU_SOURCE src/mt.c -o mt
    }
}

New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
New-Item -ItemType Directory -Force -Path "$InstallDir\bin" | Out-Null
New-Item -ItemType Directory -Force -Path "$InstallDir\lib" | Out-Null
New-Item -ItemType Directory -Force -Path "$InstallDir\include" | Out-Null

Copy-Item "magnesium.exe" "$InstallDir\bin\magnesium.exe" -ErrorAction SilentlyContinue
Copy-Item "magnesium" "$InstallDir\bin\magnesium" -ErrorAction SilentlyContinue
Copy-Item "mt.exe" "$InstallDir\bin\mt.exe" -ErrorAction SilentlyContinue
Copy-Item "mt" "$InstallDir\bin\mt" -ErrorAction SilentlyContinue
# Makefile produces magnesium.dll on Windows but libmagnesium.{so,dylib} on Unix.
# Always install under the libmagnesium.* name so bindings, pkg-config, and docs resolve it.
if ($env:OS -eq "Windows_NT") {
    $libSources = @("libmagnesium.dll", "magnesium.dll")
    $libDest = "$InstallDir\lib\libmagnesium.dll"
} else {
    $libSources = @("libmagnesium.so", "libmagnesium.dylib")
    $libDest = "$InstallDir\lib\libmagnesium.so"
}
foreach ($libSrc in $libSources) {
    if (Test-Path $libSrc) { Copy-Item $libSrc $libDest -Force; break }
}
Copy-Item "src\magnesium.h" "$InstallDir\include\magnesium.h"

New-Item -ItemType Directory -Force -Path $BinDir | Out-Null

$exePath = Join-Path $InstallDir "bin\magnesium.exe"
if (Test-Path $exePath) {
    $shortcut = Join-Path $BinDir "magnesium.exe"
    Copy-Item $exePath $shortcut -Force
    Write-Host ""
    Write-Host "Installed:" -ForegroundColor Green
    Write-Host "  $InstallDir\bin\magnesium.exe  - interpreter" -ForegroundColor Green
    Write-Host "  $InstallDir\bin\mt.exe         - toolchain manager" -ForegroundColor Green
    Write-Host "  $InstallDir\lib\libmagnesium.dll - shared library" -ForegroundColor Green
    Write-Host "  $InstallDir\include\magnesium.h  - C header" -ForegroundColor Green
    Write-Host "  Also linked at $shortcut" -ForegroundColor Green

    $pathDir = $BinDir
    $currentPath = [Environment]::GetEnvironmentVariable("Path", "User")
    if ($currentPath -notlike "*$pathDir*") {
        [Environment]::SetEnvironmentVariable("Path", "$currentPath;$pathDir", "User")
        Write-Host "Added $pathDir to user PATH" -ForegroundColor Yellow
    }
} else {
    Write-Host "Error: build failed - no executable found" -ForegroundColor Red
    Pop-Location
    Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
    exit 1
}

Write-Host ""
Write-Host "To install bindings, run:" -ForegroundColor White
Write-Host "  mt install rust" -ForegroundColor White
Write-Host "  mt install cpp" -ForegroundColor White
Write-Host "  mt install csharp" -ForegroundColor White
Write-Host "  mt install extension" -ForegroundColor White

# VS Code extension
if (Get-Command code -ErrorAction SilentlyContinue) {
    Write-Host ""
    Write-Host "Installing VS Code extension..." -ForegroundColor Cyan
    Push-Location "$TmpDir\magnesium\lsp\magnesium-vscode"
    npm install 2>$null
    npx tsc 2>$null
    if (Get-Command vsce -ErrorAction SilentlyContinue) {
        vsce package --allow-missing-repository 2>$null
    } else {
        npx @vscode/vsce package --allow-missing-repository 2>$null
    }
    $vsix = Get-ChildItem "*.vsix" -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($vsix) {
        code --install-extension $vsix.FullName
        Write-Host "VS Code extension installed!" -ForegroundColor Green
    } else {
        Write-Host "Could not package VS Code extension automatically." -ForegroundColor Yellow
        Write-Host "To install manually: open VS Code > Extensions > ... > Install from VSIX" -ForegroundColor Yellow
    }
    Pop-Location
} else {
    Write-Host ""
    Write-Host "VS Code not found. Skipping extension install." -ForegroundColor Yellow
    Write-Host "Run 'mt install extension' after installing VS Code." -ForegroundColor Yellow
}

Pop-Location

Write-Host ""
Write-Host "Done! Open a new terminal and run: magnesium --version" -ForegroundColor Green

Remove-Item -Recurse -Force $TmpDir -ErrorAction SilentlyContinue
