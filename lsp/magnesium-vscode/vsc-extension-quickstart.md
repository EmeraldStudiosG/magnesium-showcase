# Magnesium VS Code Extension Quickstart

This extension provides syntax highlighting and real-time diagnostics for the Magnesium programming language.

## Prerequisites

1.  **Magnesium CLI**: Ensure the `magnesium` binary is built and available in your system's `PATH`.
2.  **Node.js & NPM**: Required to build the extension.

## How to Build and Run

1.  **Open the folder**: Open `lsp/magnesium-vscode` in a new VS Code window.
2.  **Install dependencies**:
    ```bash
    npm install
    ```
3.  **Compile**: Press `F5` to start a new "Extension Development Host" window.
4.  **Test**: Open any `.mg` file. You should see:
    *   Syntax highlighting for keywords, strings, and globals (`@`).
    *   Red squiggles (errors) appearing ~500ms after you stop typing if there's a syntax error.

## Key Features

*   **Real-time Diagnostics**: Uses `magnesium -c -` to check your code via `stdin` without needing to save the file.
*   **TextMate Grammar**: Full support for Magnesium's modern syntax, including interpolated strings (`_""`).
*   **Language Configuration**: Supports standard comment toggling (`//`) and bracket matching.
