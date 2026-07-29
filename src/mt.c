#ifndef _WIN32
#define _XOPEN_SOURCE 700
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <limits.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
/* Windows _mkdir takes a single argument; ignore the POSIX mode parameter */
#define mkdir(path, mode) _mkdir(path)
#define access _access
#define chmod(path, mode) ((void)(path), (void)(mode), 0)
#ifndef F_OK
#define F_OK 0
#endif
#ifndef S_ISDIR
#define S_ISDIR(mode) (((mode) & _S_IFMT) == _S_IFDIR)
#endif
#else
#include <unistd.h>
#include <sys/wait.h>
#endif

#define MT_VERSION "1.1.0"
#define REPO_URL "https://github.com/EmeraldStudiosG/magnesium-dev.git"
#define DEFAULT_BRANCH "main"

static char *g_prefix = NULL;

#define MAX_CLEANUP_DIRS 8
static char *g_cleanup_dirs[MAX_CLEANUP_DIRS];
static int g_cleanup_count = 0;

static void die(const char *fmt, ...);

static char *shell_quote(const char *value) {
#ifdef _WIN32
    if (strchr(value, '"')) die("paths containing a double quote are not supported");
    char *quoted = malloc(strlen(value) + 3);
    if (!quoted) die("out of memory");
    sprintf(quoted, "\"%s\"", value);
#else
    size_t size = 3;
    for (const char *p = value; *p; p++) size += *p == '\'' ? 4 : 1;
    char *quoted = malloc(size);
    if (!quoted) die("out of memory");
    char *out = quoted;
    *out++ = '\'';
    for (const char *p = value; *p; p++) {
        if (*p == '\'') {
            memcpy(out, "'\\''", 4);
            out += 4;
        } else {
            *out++ = *p;
        }
    }
    *out++ = '\'';
    *out = '\0';
#endif
    return quoted;
}

static int remove_tree(const char *path) {
    char *quoted = shell_quote(path);
#ifdef _WIN32
    const char *format = "if exist %s rmdir /s /q %s";
    int command_length = snprintf(NULL, 0, format, quoted, quoted);
#else
    const char *format = "rm -rf -- %s";
    int command_length = snprintf(NULL, 0, format, quoted);
#endif
    if (command_length < 0) {
        free(quoted);
        return -1;
    }

    size_t command_size = (size_t)command_length + 1;
    char *cmd = malloc(command_size);
    if (!cmd) {
        free(quoted);
        die("out of memory");
    }
#ifdef _WIN32
    int written = snprintf(cmd, command_size, format, quoted, quoted);
#else
    int written = snprintf(cmd, command_size, format, quoted);
#endif
    if (written != command_length) {
        free(cmd);
        free(quoted);
        return -1;
    }

    int result = system(cmd);
    free(cmd);
    free(quoted);
    return result;
}

static void cleanup_temp_dirs(void) {
    for (int i = 0; i < g_cleanup_count; i++) {
        if (g_cleanup_dirs[i]) {
            remove_tree(g_cleanup_dirs[i]);
            free(g_cleanup_dirs[i]);
            g_cleanup_dirs[i] = NULL;
        }
    }
    g_cleanup_count = 0;
}

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "mt: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    cleanup_temp_dirs();
    exit(1);
}

static void info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("mt: ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

static char *xstrdup(const char *s) {
    size_t length = strlen(s);
    char *r = malloc(length + 1);
    if (!r) die("out of memory");
    memcpy(r, s, length + 1);
    return r;
}

static char *join_path(const char *base, const char *relative) {
    size_t base_length = strlen(base);
    size_t relative_length = strlen(relative);
    int separator = base_length > 0 &&
                    base[base_length - 1] != '/' &&
                    base[base_length - 1] != '\\';
    char *result = malloc(base_length + (size_t)separator + relative_length + 1);
    if (!result) die("out of memory");
    memcpy(result, base, base_length);
    size_t offset = base_length;
    if (separator) result[offset++] = '/';
    memcpy(result + offset, relative, relative_length + 1);
    return result;
}

static void register_cleanup_dir(const char *dir) {
    if (g_cleanup_count >= MAX_CLEANUP_DIRS) return;
    g_cleanup_dirs[g_cleanup_count++] = xstrdup(dir);
}

static char *default_prefix(void) {
#ifdef _WIN32
    const char *home = getenv("USERPROFILE");
    if (!home) home = getenv("HOME");
#else
    const char *home = getenv("HOME");
#endif
    if (!home) die("HOME not set");
    char *buf = malloc(strlen(home) + 32);
    if (!buf) die("out of memory");
    sprintf(buf, "%s/.magnesium", home);
    return buf;
}

static const char *prefix(void) {
    if (!g_prefix) {
        const char *configured = getenv("MAGNESIUM_PREFIX");
        g_prefix = configured && configured[0] ? xstrdup(configured) : default_prefix();
    }
    return g_prefix;
}

static void mkdir_p(const char *path) {
    char *tmp = xstrdup(path);
#ifdef _WIN32
    for (char *p = tmp; *p; p++) {
        if (*p == '\\') *p = '/';
    }
#endif
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            if (p == tmp + 2 && tmp[1] == ':') continue;
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
    free(tmp);
}

static int run_cmd(const char *cmd) {
    int ret = system(cmd);
    if (ret == -1) return -1;
#ifdef _WIN32
    return ret;
#else
    if (WIFEXITED(ret)) return WEXITSTATUS(ret);
    if (WIFSIGNALED(ret)) return 128 + WTERMSIG(ret);
    return -1;
#endif
}

static int cmd_exists(const char *name) {
    char *cmd = malloc(strlen(name) + 32);
#ifdef _WIN32
    sprintf(cmd, "where %s >NUL 2>NUL", name);
#else
    sprintf(cmd, "which %s 2>/dev/null", name);
#endif
    int ret = system(cmd);
    free(cmd);
    return ret == 0;
}

static const char *library_name(void) {
#ifdef _WIN32
    return "libmagnesium.dll";
#elif defined(__APPLE__)
    return "libmagnesium.dylib";
#else
    return "libmagnesium.so";
#endif
}

static const char *interpreter_name(void) {
#ifdef _WIN32
    return "magnesium.exe";
#else
    return "magnesium";
#endif
}

static const char *tool_name(void) {
#ifdef _WIN32
    return "mt.exe";
#else
    return "mt";
#endif
}

static const char *temp_root(void) {
    const char *result = getenv("MT_TMPDIR");
    if (!result) result = getenv("TMPDIR");
#ifdef _WIN32
    if (!result) result = getenv("TEMP");
    if (!result) result = getenv("TMP");
#endif
    return result ? result : "/tmp";
}

static char *make_temp_dir(const char *tag) {
#ifdef _WIN32
    char base[MAX_PATH];
    DWORD length = GetFullPathNameA(temp_root(), MAX_PATH, base, NULL);
    if (length == 0 || length >= MAX_PATH) die("invalid temporary directory");
    char temp_file[MAX_PATH];
    if (!GetTempFileNameA(base, "mgt", 0, temp_file)) die("cannot allocate temp path");
    DeleteFileA(temp_file);
    if (!CreateDirectoryA(temp_file, NULL)) die("cannot create temp dir");
    (void)tag;
    return xstrdup(temp_file);
#else
    const char *root = temp_root();
    char *path = malloc(strlen(root) + strlen(tag) + 16);
    if (!path) die("out of memory");
    sprintf(path, "%s/%s-XXXXXX", root, tag);
    if (!mkdtemp(path)) die("cannot create temp dir");
    return path;
#endif
}

static int copy_file(const char *source, const char *destination) {
    FILE *input = fopen(source, "rb");
    if (!input) return -1;
    FILE *output = fopen(destination, "wb");
    if (!output) {
        fclose(input);
        return -1;
    }

    char buffer[16384];
    size_t count;
    int failed = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), input)) > 0) {
        if (fwrite(buffer, 1, count, output) != count) {
            failed = 1;
            break;
        }
    }
    if (ferror(input)) failed = 1;
    if (fclose(output) != 0) failed = 1;
    fclose(input);
    if (!failed) chmod(destination, 0755);
    return failed ? -1 : 0;
}

static int copy_tree(const char *source, const char *destination) {
    char *source_contents = malloc(strlen(source) + 4);
    if (!source_contents) die("out of memory");
#ifdef _WIN32
    sprintf(source_contents, "%s/*", source);
#else
    sprintf(source_contents, "%s/.", source);
#endif
    char *quoted_source = shell_quote(source_contents);
    char *quoted_destination = shell_quote(destination);
    char *cmd = malloc(strlen(quoted_source) + strlen(quoted_destination) + 64);
    if (!cmd) die("out of memory");
#ifdef _WIN32
    sprintf(cmd, "xcopy %s %s /E /I /Y /Q >NUL", quoted_source, quoted_destination);
#else
    sprintf(cmd, "cp -R %s %s", quoted_source, quoted_destination);
#endif
    int result = run_cmd(cmd);
    free(cmd);
    free(quoted_destination);
    free(quoted_source);
    free(source_contents);
    return result;
}

static void write_pkg_config(const char *install_prefix) {
    char *directory = malloc(strlen(install_prefix) + 32);
    char *path = malloc(strlen(install_prefix) + 64);
    if (!directory || !path) die("out of memory");
    sprintf(directory, "%s/lib/pkgconfig", install_prefix);
    mkdir_p(directory);
    sprintf(path, "%s/magnesium.pc", directory);

    FILE *file = fopen(path, "w");
    if (!file) die("cannot write %s", path);
    fprintf(file,
            "prefix=%s\n"
            "libdir=${prefix}/lib\n"
            "includedir=${prefix}/include\n\n"
            "Name: magnesium\n"
            "Description: Magnesium language VM\n"
            "Version: %s\n"
            "Libs: -L${libdir} -lmagnesium\n",
            install_prefix, MT_VERSION);
#ifdef _WIN32
    fprintf(file, "Libs.private: -lws2_32\n");
#elif defined(__APPLE__)
    fprintf(file, "Libs.private: -lm -pthread\n");
#else
    fprintf(file, "Libs.private: -lm -ldl -pthread\n");
#endif
    fprintf(file, "Cflags: -I${includedir}\n");
    fclose(file);
    free(path);
    free(directory);
}

static int core_installed(void) {
    char *path = malloc(strlen(prefix()) + 64);
    sprintf(path, "%s/lib/%s", prefix(), library_name());
    int exists = access(path, F_OK) == 0;
    free(path);
    return exists;
}

static char *read_version_file(void) {
    char *path = malloc(strlen(prefix()) + 16);
    sprintf(path, "%s/VERSION", prefix());
    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return xstrdup("unknown");
    char buf[64];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return xstrdup("unknown"); }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return xstrdup(buf);
}

static void write_version_file(void) {
    char *path = malloc(strlen(prefix()) + 16);
    sprintf(path, "%s/VERSION", prefix());
    FILE *f = fopen(path, "w");
    if (!f) die("cannot write VERSION file");
    fprintf(f, "%s\n", MT_VERSION);
    fclose(f);
    free(path);
}

static int extension_installed(void) {
    char *path = malloc(strlen(prefix()) + 64);
    sprintf(path, "%s/share/magnesium/extension-installed", prefix());
    int exists = access(path, F_OK) == 0;
    free(path);
    return exists;
}

static int do_install_extension_from_source(const char *lsp_dir) {
    if (!cmd_exists("code")) {
        info("vscode not found, skipping extension install");
        return 0;
    }
    if (!cmd_exists("npm")) {
        info("npm not found, skipping extension build");
        return 0;
    }

    info("building VS Code extension...");
    char *cmd = malloc(strlen(lsp_dir) + 512);
    char *quoted_dir = shell_quote(lsp_dir);

#ifdef _WIN32
    sprintf(cmd, "cd /d %s && npm install 2>&1", quoted_dir);
#else
    sprintf(cmd, "cd %s && npm install 2>&1", quoted_dir);
#endif
    if (run_cmd(cmd) != 0) {
        info("npm install failed, skipping extension");
        free(cmd);
        free(quoted_dir);
        return 0;
    }

#ifdef _WIN32
    sprintf(cmd, "cd /d %s && npx tsc 2>&1", quoted_dir);
#else
    sprintf(cmd, "cd %s && npx tsc 2>&1", quoted_dir);
#endif
    if (run_cmd(cmd) != 0) {
        info("typescript compilation failed, skipping extension");
        free(cmd);
        free(quoted_dir);
        return 0;
    }

    int packaged = 0;
    if (cmd_exists("vsce")) {
#ifdef _WIN32
        sprintf(cmd, "cd /d %s && vsce package --allow-missing-repository --out magnesium.vsix 2>&1", quoted_dir);
#else
        sprintf(cmd, "cd %s && vsce package --allow-missing-repository --out magnesium.vsix 2>&1", quoted_dir);
#endif
        if (run_cmd(cmd) == 0) packaged = 1;
    }
    if (!packaged) {
#ifdef _WIN32
        sprintf(cmd, "cd /d %s && npx @vscode/vsce package --allow-missing-repository --out magnesium.vsix 2>&1", quoted_dir);
#else
        sprintf(cmd, "cd %s && npx @vscode/vsce package --allow-missing-repository --out magnesium.vsix 2>&1", quoted_dir);
#endif
        if (run_cmd(cmd) == 0) packaged = 1;
    }

    int installed = 0;
    if (packaged) {
#ifdef _WIN32
        sprintf(cmd, "cd /d %s && code --install-extension magnesium.vsix 2>&1", quoted_dir);
#else
        sprintf(cmd, "cd %s && code --install-extension magnesium.vsix 2>&1", quoted_dir);
#endif
        if (run_cmd(cmd) == 0) {
            info("VS Code extension installed");
            installed = 1;
        } else {
            info("VS Code rejected the extension package");
        }
    } else {
        info("could not package extension automatically");
    }

    free(cmd);
    free(quoted_dir);
    return installed;
}

static void do_install_extension(void) {
    if (!core_installed()) {
        die("magnesium core not installed. Run 'mt install' first.");
    }
    if (!cmd_exists("git")) die("git is required");
    if (!cmd_exists("code")) die("VS Code (code) is required");
    if (!cmd_exists("npm")) die("npm is required to build the extension");

    char *clone_dir = make_temp_dir("mt-ext");
    register_cleanup_dir(clone_dir);

    info("downloading extension source...");
    char *cmd = malloc(strlen(clone_dir) * 2 + strlen(REPO_URL) + 256);
    char *checkout_dir = join_path(clone_dir, "mg");
    char *quoted_checkout = shell_quote(checkout_dir);

    sprintf(cmd, "git clone --depth 1 --branch %s --filter=blob:none --sparse %s %s 2>&1",
            DEFAULT_BRANCH, REPO_URL, quoted_checkout);
    if (run_cmd(cmd) != 0) die("git clone failed");

#ifdef _WIN32
    sprintf(cmd, "cd /d %s && git sparse-checkout set lsp 2>&1", quoted_checkout);
#else
    sprintf(cmd, "cd %s && git sparse-checkout set lsp 2>&1", quoted_checkout);
#endif
    if (run_cmd(cmd) != 0) die("sparse checkout failed");

    char *lsp_dir = join_path(checkout_dir, "lsp/magnesium-vscode");

    int installed = do_install_extension_from_source(lsp_dir);

    char *pfx = xstrdup(prefix());
    if (installed) {
        char *marker = malloc(strlen(pfx) + 64);
        sprintf(marker, "%s/share/magnesium", pfx);
        mkdir_p(marker);
        sprintf(marker, "%s/share/magnesium/extension-installed", pfx);
        FILE *f = fopen(marker, "w");
        if (f) { fprintf(f, "1\n"); fclose(f); }
        free(marker);
    }
    free(lsp_dir);
    free(quoted_checkout);
    free(checkout_dir);
    free(cmd);
    free(clone_dir);
    free(pfx);

    cleanup_temp_dirs();
    g_cleanup_count = 0;
}

static void do_install_core(void) {
    if (!cmd_exists("git")) die("git is required");
#ifdef _WIN32
    if (!cmd_exists("clang")) die("LLVM-MinGW clang is required");
    const char *make_tool = cmd_exists("mingw32-make") ? "mingw32-make" :
                            (cmd_exists("make") ? "make" : NULL);
#else
    if (!cmd_exists("gcc") && !cmd_exists("cc") && !cmd_exists("clang"))
        die("a C compiler is required");
    const char *make_tool = cmd_exists("make") ? "make" : NULL;
#endif
    if (!make_tool) die("make is required");

    char *clone_dir = make_temp_dir("mt-build");
    register_cleanup_dir(clone_dir);

    info("cloning repository...");
    char *cmd = malloc(strlen(clone_dir) * 2 + strlen(REPO_URL) + 256);
    char *checkout_dir = join_path(clone_dir, "magnesium");
    char *quoted_checkout = shell_quote(checkout_dir);
    sprintf(cmd, "git clone --depth 1 --branch %s %s %s 2>&1",
            DEFAULT_BRANCH, REPO_URL, quoted_checkout);
    if (run_cmd(cmd) != 0) die("git clone failed");

    info("building...");
    sprintf(cmd, "%s -C %s 2>&1", make_tool, quoted_checkout);
    if (run_cmd(cmd) != 0) die("build failed");

    sprintf(cmd, "%s -C %s lib 2>&1", make_tool, quoted_checkout);
    if (run_cmd(cmd) != 0) die("library build failed");

    char *pfx = xstrdup(prefix());
    mkdir_p(pfx);

    char *sub = malloc(strlen(pfx) + 32);
    sprintf(sub, "%s/lib", pfx); mkdir_p(sub);
    sprintf(sub, "%s/lib/pkgconfig", pfx); mkdir_p(sub);
    sprintf(sub, "%s/include", pfx); mkdir_p(sub);
    sprintf(sub, "%s/bin", pfx); mkdir_p(sub);
    free(sub);

    char *src = malloc(strlen(clone_dir) + 32);
    char *dst = malloc(strlen(pfx) + 32);

    sprintf(src, "%s/%s", checkout_dir, interpreter_name());
    sprintf(dst, "%s/bin/%s", pfx, interpreter_name());
    if (copy_file(src, dst) != 0) die("cannot install interpreter");

    sprintf(src, "%s/%s", checkout_dir, library_name());
    sprintf(dst, "%s/lib/%s", pfx, library_name());
    if (copy_file(src, dst) != 0) die("cannot install shared library");

    sprintf(src, "%s/%s", checkout_dir, tool_name());
    sprintf(dst, "%s/bin/%s", pfx, tool_name());
    int copied_tool = copy_file(src, dst) == 0;
#ifndef _WIN32
    if (!copied_tool) {
        char self[512];
        ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len > 0) {
            self[len] = '\0';
            copied_tool = copy_file(self, dst) == 0;
        }
    }
#endif
    if (!copied_tool) die("cannot install toolchain manager");

    sprintf(src, "%s/src/magnesium.h", checkout_dir);
    sprintf(dst, "%s/include/magnesium.h", pfx);
    if (copy_file(src, dst) != 0) die("cannot install C header");

    free(src); free(dst);

    write_version_file();
    write_pkg_config(pfx);

    char *lsp_dir = join_path(checkout_dir, "lsp/magnesium-vscode");
    struct stat st;
    if (stat(lsp_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (do_install_extension_from_source(lsp_dir)) {
            char *marker = malloc(strlen(pfx) + 64);
            sprintf(marker, "%s/share/magnesium", pfx);
            mkdir_p(marker);
            sprintf(marker, "%s/share/magnesium/extension-installed", pfx);
            FILE *f = fopen(marker, "w");
            if (f) { fprintf(f, "1\n"); fclose(f); }
            free(marker);
        }
    }
    free(lsp_dir);

    info("installed to %s/", pfx);
    info("  bin/%s    - interpreter", interpreter_name());
    info("  bin/%s           - toolchain manager", tool_name());
    info("  lib/%s - shared library", library_name());
    info("  include/magnesium.h - C header");

    char *bin_dir = malloc(strlen(pfx) + 8);
    sprintf(bin_dir, "%s/bin", pfx);
    const char *path_env = getenv("PATH");
    if (!path_env || !strstr(path_env, bin_dir)) {
#ifdef _WIN32
        info("add %s to your PATH", bin_dir);
#else
        info("add to PATH: export PATH=\"%s:$PATH\"", bin_dir);
#endif
    }
    free(bin_dir);

    free(cmd); free(quoted_checkout); free(checkout_dir); free(clone_dir);
    free(pfx);

    cleanup_temp_dirs();
    g_cleanup_count = 0;
}

static void do_install_binding(const char *binding) {
    if (!core_installed()) {
        die("magnesium core not installed. Run 'mt install' first.");
    }

    const char *bindings[] = {"rust", "cpp", "csharp", NULL};
    int valid = 0;
    for (int i = 0; bindings[i]; i++) {
        if (strcmp(binding, bindings[i]) == 0) { valid = 1; break; }
    }
    if (!valid) die("unknown binding '%s' (available: rust, cpp, csharp)", binding);

    if (!cmd_exists("git")) die("git is required");

    char *clone_dir = make_temp_dir("mt-binding");
    register_cleanup_dir(clone_dir);

    info("downloading %s binding...", binding);
    char *cmd = malloc(strlen(clone_dir) * 2 + strlen(REPO_URL) + 512);
    char *checkout_dir = join_path(clone_dir, "mg");
    char *quoted_checkout = shell_quote(checkout_dir);

    sprintf(cmd, "git clone --depth 1 --branch %s --filter=blob:none --sparse %s %s 2>&1",
            DEFAULT_BRANCH, REPO_URL, quoted_checkout);
    if (run_cmd(cmd) != 0) die("git clone failed");

#ifdef _WIN32
    sprintf(cmd, "cd /d %s && git sparse-checkout set bindings/%s src 2>&1",
            quoted_checkout, binding);
#else
    sprintf(cmd, "cd %s && git sparse-checkout set bindings/%s src 2>&1",
            quoted_checkout, binding);
#endif
    if (run_cmd(cmd) != 0) die("sparse checkout failed");

    char *pfx = xstrdup(prefix());
    char *dest = malloc(strlen(pfx) + strlen(binding) + 32);
    sprintf(dest, "%s/share/magnesium/bindings/%s", pfx, binding);
    mkdir_p(dest);

    char *bindings_dir = join_path(checkout_dir, "bindings");
    char *src_dir = join_path(bindings_dir, binding);
    free(bindings_dir);

    if (copy_tree(src_dir, dest) != 0) die("cannot install binding files");

    if (strcmp(binding, "rust") == 0 || strcmp(binding, "cpp") == 0) {
        char *shared_src = malloc(strlen(pfx) + 32);
        char *repo_src = malloc(strlen(clone_dir) + 32);
        if (!shared_src || !repo_src) die("out of memory");
        sprintf(shared_src, "%s/share/magnesium/src", pfx);
        mkdir_p(shared_src);
        sprintf(repo_src, "%s/src", checkout_dir);
        if (copy_tree(repo_src, shared_src) != 0)
            die("cannot install native binding sources");
        free(repo_src);
        free(shared_src);
    }

    free(src_dir); free(dest);

    info("installed %s binding to %s/share/magnesium/bindings/%s/", pfx, pfx, binding);

    if (strcmp(binding, "rust") == 0) {
        info("");
        info("to use in a Rust project, add to Cargo.toml:");
        info("  [dependencies.magnesium]");
        info("  path = \"%s/share/magnesium/bindings/rust/magnesium\"", pfx);
        info("  features = [\"system\"]");
        info("  default-features = false");
        info("");
        info("then build with:");
        info("  PKG_CONFIG_PATH=\"%s/lib/pkgconfig\" cargo build", pfx);
    } else if (strcmp(binding, "cpp") == 0) {
        info("");
        info("to use in a C++ project with CMake:");
        info("  add_subdirectory(\"%s/share/magnesium/bindings/cpp\" magnesium-bindings)", pfx);
        info("  target_link_libraries(your_target PRIVATE magnesium)");
    } else if (strcmp(binding, "csharp") == 0) {
        info("");
        info("to use in a C# project:");
        info("  dotnet add reference %s/share/magnesium/bindings/csharp/Magnesium/Magnesium.csproj", pfx);
        info("ensure %s/lib/ is on your platform's shared-library search path", pfx);
    }

    free(pfx);
    free(quoted_checkout);
    free(checkout_dir);
    free(cmd); free(clone_dir);

    cleanup_temp_dirs();
    g_cleanup_count = 0;
}

static void do_list(void) {
    const char *pfx = prefix();
    printf("prefix: %s\n", pfx);

    char *version = read_version_file();
    printf("version: %s\n", version);
    free(version);

    printf("core: %s\n", core_installed() ? "installed" : "not installed");

    const char *bindings[] = {"rust", "cpp", "csharp"};
    for (int i = 0; i < 3; i++) {
        char *path = malloc(strlen(pfx) + strlen(bindings[i]) + 64);
        sprintf(path, "%s/share/magnesium/bindings/%s", pfx, bindings[i]);
        printf("%s: %s\n", bindings[i], access(path, F_OK) == 0 ? "installed" : "not installed");
        free(path);
    }

    printf("extension: %s\n", extension_installed() ? "installed" : "not installed");
}

static void do_version(void) {
    printf("mt %s\n", MT_VERSION);
}

static int unsafe_uninstall_prefix(const char *path) {
    if (!path || !path[0] || strcmp(path, ".") == 0 || strcmp(path, "..") == 0)
        return 1;
    int navigation_only = 1;
    for (const char *p = path; *p; p++) {
        if (*p != '.' && *p != '/' && *p != '\\') {
            navigation_only = 0;
            break;
        }
    }
    if (navigation_only) return 1;
    size_t length = strlen(path);
    while (length > 1 && (path[length - 1] == '/' || path[length - 1] == '\\'))
        length--;
    if (length == 1 && (path[0] == '/' || path[0] == '\\')) return 1;
#ifdef _WIN32
    if (length == 2 && path[1] == ':') return 1;
    char resolved[MAX_PATH];
    char current[MAX_PATH];
    DWORD resolved_length = GetFullPathNameA(path, MAX_PATH, resolved, NULL);
    DWORD current_length = GetCurrentDirectoryA(MAX_PATH, current);
    if (resolved_length > 0 && resolved_length < MAX_PATH &&
        current_length > 0 && current_length < MAX_PATH &&
        _stricmp(resolved, current) == 0)
        return 1;
#else
    char resolved[PATH_MAX];
    char current[PATH_MAX];
    if (realpath(path, resolved) && getcwd(current, sizeof(current)) &&
        (strcmp(resolved, "/") == 0 || strcmp(resolved, current) == 0))
        return 1;
#endif
    return 0;
}

static void do_uninstall(void) {
    const char *pfx = prefix();
    if (unsafe_uninstall_prefix(pfx))
        die("refusing to uninstall unsafe prefix '%s'", pfx);
    info("removing %s/", pfx);
    if (remove_tree(pfx) != 0)
        die("could not remove install prefix '%s'", pfx);
    info("uninstalled");
}

static void usage(void) {
    printf("mt - Magnesium Toolchain %s\n", MT_VERSION);
    printf("\n");
    printf("Usage:\n");
    printf("  mt install              Install magnesium core + VS Code extension\n");
    printf("  mt install <binding>    Install a binding (rust, cpp, csharp)\n");
    printf("  mt install extension    Install VS Code extension only\n");
    printf("  mt list                 List installed components\n");
    printf("  mt version              Print mt version\n");
    printf("  mt uninstall            Remove all installed components\n");
    printf("  mt prefix               Print install prefix\n");
    printf("\n");
    printf("Options:\n");
    printf("  --prefix=<path>         Set install prefix (default: ~/.magnesium)\n");
    printf("\n");
    printf("Components are downloaded from:\n");
    printf("  %s\n", REPO_URL);
}

int main(int argc, char **argv) {
    atexit(cleanup_temp_dirs);

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--prefix=", 9) == 0) {
            if (argv[i][9] == '\0') die("--prefix requires a non-empty path");
            g_prefix = xstrdup(argv[i] + 9);
        }
    }

    const char *cmd = NULL;
    const char *subcmd = NULL;
    int operand_count = 0;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--prefix=", 9) == 0) continue;
        operand_count++;
        if (!cmd) cmd = argv[i];
        else if (!subcmd) subcmd = argv[i];
    }

    if (!cmd) { usage(); return 0; }
    if (operand_count > 2) {
        fprintf(stderr, "mt: too many arguments\n");
        return 1;
    }

    if (strcmp(cmd, "install") == 0) {
        if (subcmd && strcmp(subcmd, "extension") == 0) {
            do_install_extension();
        } else if (subcmd) {
            do_install_binding(subcmd);
        } else {
            do_install_core();
        }
    } else if (strcmp(cmd, "list") == 0) {
        if (subcmd) die("'list' does not accept an argument");
        do_list();
    } else if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        if (subcmd) die("'version' does not accept an argument");
        do_version();
    } else if (strcmp(cmd, "uninstall") == 0) {
        if (subcmd) die("'uninstall' does not accept an argument");
        do_uninstall();
    } else if (strcmp(cmd, "prefix") == 0) {
        if (subcmd) die("'prefix' does not accept an argument");
        printf("%s\n", prefix());
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        if (subcmd) die("'help' does not accept an argument");
        usage();
    } else {
        fprintf(stderr, "mt: unknown command '%s'\n", cmd);
        usage();
        return 1;
    }

    return 0;
}
