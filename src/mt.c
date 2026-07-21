#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#include <signal.h>

#ifdef _WIN32
#include <direct.h>
/* Windows _mkdir takes a single argument; ignore the POSIX mode parameter */
#define mkdir(path, mode) _mkdir(path)
#else
#include <sys/wait.h>
#endif

#define MT_VERSION "1.0.0"
#define REPO_URL "https://github.com/EmeraldStudiosG/magnesium-dev.git"
#define DEFAULT_BRANCH "main"

static char *g_prefix = NULL;

#define MAX_CLEANUP_DIRS 8
static char *g_cleanup_dirs[MAX_CLEANUP_DIRS];
static int g_cleanup_count = 0;

static void cleanup_temp_dirs(void) {
    for (int i = 0; i < g_cleanup_count; i++) {
        if (g_cleanup_dirs[i]) {
            char *cmd = malloc(strlen(g_cleanup_dirs[i]) + 16);
            sprintf(cmd, "rm -rf %s", g_cleanup_dirs[i]);
            system(cmd);
            free(cmd);
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
    char *r = strdup(s);
    if (!r) die("out of memory");
    return r;
}

static void register_cleanup_dir(const char *dir) {
    if (g_cleanup_count >= MAX_CLEANUP_DIRS) return;
    g_cleanup_dirs[g_cleanup_count++] = xstrdup(dir);
}

static char *default_prefix(void) {
    const char *home = getenv("HOME");
    if (!home) die("HOME not set");
    char *buf = malloc(strlen(home) + 32);
    if (!buf) die("out of memory");
    sprintf(buf, "%s/.magnesium", home);
    return buf;
}

static const char *prefix(void) {
    if (!g_prefix) g_prefix = default_prefix();
    return g_prefix;
}

static void mkdir_p(const char *path) {
    char *tmp = xstrdup(path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
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
    return WEXITSTATUS(ret);
#endif
}

static int cmd_exists(const char *name) {
    char *cmd = malloc(strlen(name) + 8);
    sprintf(cmd, "which %s 2>/dev/null", name);
    int ret = system(cmd);
    free(cmd);
    return ret == 0;
}

static int core_installed(void) {
    char *path = malloc(strlen(prefix()) + 64);
    sprintf(path, "%s/lib/libmagnesium.so", prefix());
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

static void do_install_extension_from_source(const char *lsp_dir) {
    if (!cmd_exists("code")) {
        info("vscode not found, skipping extension install");
        return;
    }
    if (!cmd_exists("npm")) {
        info("npm not found, skipping extension build");
        return;
    }

    info("building VS Code extension...");
    char *cmd = malloc(strlen(lsp_dir) + 256);

    sprintf(cmd, "cd %s && npm install 2>&1", lsp_dir);
    if (run_cmd(cmd) != 0) {
        info("npm install failed, skipping extension");
        free(cmd);
        return;
    }

    sprintf(cmd, "cd %s && npx tsc 2>&1", lsp_dir);
    if (run_cmd(cmd) != 0) {
        info("typescript compilation failed, skipping extension");
        free(cmd);
        return;
    }

    int packaged = 0;
    if (cmd_exists("vsce")) {
        sprintf(cmd, "cd %s && vsce package --allow-missing-repository 2>&1", lsp_dir);
        if (run_cmd(cmd) == 0) packaged = 1;
    }
    if (!packaged) {
        sprintf(cmd, "cd %s && npx @vscode/vsce package --allow-missing-repository 2>&1", lsp_dir);
        if (run_cmd(cmd) == 0) packaged = 1;
    }

    if (packaged) {
        sprintf(cmd, "cd %s && code --install-extension $(ls -t *.vsix 2>/dev/null | head -1) 2>&1", lsp_dir);
        run_cmd(cmd);
        info("VS Code extension installed");
    } else {
        info("could not package extension automatically");
    }

    free(cmd);
}

static void do_install_extension(void) {
    if (!core_installed()) {
        die("magnesium core not installed. Run 'mt install' first.");
    }
    if (!cmd_exists("git")) die("git is required");
    if (!cmd_exists("code")) die("VS Code (code) is required");
    if (!cmd_exists("npm")) die("npm is required to build the extension");

    char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";

    char *clone_dir = malloc(strlen(tmpdir) + 32);
    sprintf(clone_dir, "%s/mt-ext-XXXXXX", tmpdir);
    if (!mkdtemp(clone_dir)) die("cannot create temp dir");
    register_cleanup_dir(clone_dir);

    info("downloading extension source...");
    char *cmd = malloc(strlen(clone_dir) + strlen(REPO_URL) + 256);

    sprintf(cmd, "git clone --depth 1 --branch %s --filter=blob:none --sparse %s %s/mg 2>&1",
            DEFAULT_BRANCH, REPO_URL, clone_dir);
    if (run_cmd(cmd) != 0) die("git clone failed");

    sprintf(cmd, "cd %s/mg && git sparse-checkout set lsp 2>&1", clone_dir);
    if (run_cmd(cmd) != 0) die("sparse checkout failed");

    char *lsp_dir = malloc(strlen(clone_dir) + 64);
    sprintf(lsp_dir, "%s/mg/lsp/magnesium-vscode", clone_dir);

    do_install_extension_from_source(lsp_dir);

    char *pfx = xstrdup(prefix());
    char *marker = malloc(strlen(pfx) + 64);
    sprintf(marker, "%s/share/magnesium", pfx);
    mkdir_p(marker);
    sprintf(marker, "%s/share/magnesium/extension-installed", pfx);
    FILE *f = fopen(marker, "w");
    if (f) { fprintf(f, "1\n"); fclose(f); }
    free(marker);
    free(lsp_dir);
    free(cmd);
    free(clone_dir);
    free(pfx);

    cleanup_temp_dirs();
    g_cleanup_count = 0;
}

static void do_install_core(void) {
    if (!cmd_exists("git")) die("git is required");
    if (!cmd_exists("gcc") && !cmd_exists("cc")) die("a C compiler is required");

    char *tmpdir = getenv("MT_TMPDIR");
    if (!tmpdir) {
        tmpdir = getenv("TMPDIR");
        if (!tmpdir) tmpdir = "/tmp";
    }

    char *clone_dir = malloc(strlen(tmpdir) + 32);
    sprintf(clone_dir, "%s/mt-build-XXXXXX", tmpdir);
    if (!mkdtemp(clone_dir)) die("cannot create temp dir");
    register_cleanup_dir(clone_dir);

    info("cloning repository...");
    char *cmd = malloc(strlen(clone_dir) + strlen(REPO_URL) + 128);
    sprintf(cmd, "git clone --depth 1 --branch %s %s %s/magnesium 2>&1", DEFAULT_BRANCH, REPO_URL, clone_dir);
    if (run_cmd(cmd) != 0) die("git clone failed");
    free(cmd);

    info("building...");
    cmd = malloc(strlen(clone_dir) + 128);
    sprintf(cmd, "make -C %s/magnesium 2>&1", clone_dir);
    if (run_cmd(cmd) != 0) die("build failed");

    sprintf(cmd, "make -C %s/magnesium lib 2>&1", clone_dir);
    if (run_cmd(cmd) != 0) die("library build failed");

    char *pfx = xstrdup(prefix());
    mkdir_p(pfx);

    char *sub = malloc(strlen(pfx) + 32);
    sprintf(sub, "%s/lib", pfx); mkdir_p(sub);
    sprintf(sub, "%s/include", pfx); mkdir_p(sub);
    sprintf(sub, "%s/bin", pfx); mkdir_p(sub);
    free(sub);

    char *src = malloc(strlen(clone_dir) + 32);
    char *dst = malloc(strlen(pfx) + 32);

    sprintf(src, "%s/magnesium/magnesium", clone_dir);
    sprintf(dst, "%s/bin/magnesium", pfx);
    sprintf(cmd, "cp %s %s", src, dst); run_cmd(cmd);
    chmod(dst, 0755);

    sprintf(src, "%s/magnesium/libmagnesium.so", clone_dir);
    sprintf(dst, "%s/lib/libmagnesium.so", pfx);
    sprintf(cmd, "cp %s %s", src, dst); run_cmd(cmd);

    sprintf(src, "%s/magnesium/mt", clone_dir);
    sprintf(dst, "%s/bin/mt", pfx);
    sprintf(cmd, "cp %s %s 2>/dev/null || true", src, dst); run_cmd(cmd);
#ifndef _WIN32
    {
        char self[512];
        ssize_t len = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (len > 0) {
            self[len] = '\0';
            sprintf(cmd, "cp %s %s", self, dst); run_cmd(cmd);
        }
    }
#endif
    chmod(dst, 0755);

    sprintf(src, "%s/magnesium/src/magnesium.h", clone_dir);
    sprintf(dst, "%s/include/magnesium.h", pfx);
    sprintf(cmd, "cp %s %s", src, dst); run_cmd(cmd);

    free(src); free(dst);

    write_version_file();

    char *lsp_dir = malloc(strlen(clone_dir) + 64);
    sprintf(lsp_dir, "%s/magnesium/lsp/magnesium-vscode", clone_dir);
    struct stat st;
    if (stat(lsp_dir, &st) == 0 && S_ISDIR(st.st_mode)) {
        do_install_extension_from_source(lsp_dir);
        char *marker = malloc(strlen(pfx) + 64);
        sprintf(marker, "%s/share/magnesium", pfx);
        mkdir_p(marker);
        sprintf(marker, "%s/share/magnesium/extension-installed", pfx);
        FILE *f = fopen(marker, "w");
        if (f) { fprintf(f, "1\n"); fclose(f); }
        free(marker);
    }
    free(lsp_dir);

    info("installed to %s/", pfx);
    info("  bin/magnesium    - interpreter");
    info("  bin/mt           - toolchain manager");
    info("  lib/libmagnesium.so - shared library");
    info("  include/magnesium.h - C header");

    char *bin_dir = malloc(strlen(pfx) + 8);
    sprintf(bin_dir, "%s/bin", pfx);
    const char *path_env = getenv("PATH");
    if (!path_env || !strstr(path_env, bin_dir)) {
        info("add to PATH: export PATH=\"%s:$PATH\"", bin_dir);
    }
    free(bin_dir);

    free(cmd); free(clone_dir);
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

    char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";

    char *clone_dir = malloc(strlen(tmpdir) + 32);
    sprintf(clone_dir, "%s/mt-binding-XXXXXX", tmpdir);
    if (!mkdtemp(clone_dir)) die("cannot create temp dir");
    register_cleanup_dir(clone_dir);

    info("downloading %s binding...", binding);
    char *cmd = malloc(strlen(clone_dir) + strlen(REPO_URL) + 256);

    sprintf(cmd, "git clone --depth 1 --branch %s --filter=blob:none --sparse %s %s/mg 2>&1",
            DEFAULT_BRANCH, REPO_URL, clone_dir);
    if (run_cmd(cmd) != 0) die("git clone failed");

    sprintf(cmd, "cd %s/mg && git sparse-checkout set bindings/%s 2>&1", clone_dir, binding);
    if (run_cmd(cmd) != 0) die("sparse checkout failed");

    char *pfx = xstrdup(prefix());
    char *dest = malloc(strlen(pfx) + strlen(binding) + 32);
    sprintf(dest, "%s/share/magnesium/bindings/%s", pfx, binding);
    mkdir_p(dest);

    char *src_dir = malloc(strlen(clone_dir) + 64);
    sprintf(src_dir, "%s/mg/bindings/%s", clone_dir, binding);

    sprintf(cmd, "cp -r %s/* %s/ 2>/dev/null; cp -r %s/.[!.]* %s/ 2>/dev/null", src_dir, dest, src_dir, dest);
    run_cmd(cmd);

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
        info("  set(Magnesium_DIR \"%s/share/magnesium/bindings/cpp\")", pfx);
        info("  find_package(Magnesium REQUIRED)");
        info("  target_link_libraries(your_target magnesium)");
    } else if (strcmp(binding, "csharp") == 0) {
        info("");
        info("to use in a C# project:");
        info("  dotnet add reference %s/share/magnesium/bindings/csharp/Magnesium/Magnesium.csproj", pfx);
        info("ensure %s/lib/ is in your LD_LIBRARY_PATH", pfx);
    }

    free(pfx);
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

static void do_uninstall(void) {
    const char *pfx = prefix();
    char *cmd = malloc(strlen(pfx) + 32);
    sprintf(cmd, "rm -rf %s", pfx);
    info("removing %s/", pfx);
    run_cmd(cmd);
    free(cmd);
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
            g_prefix = xstrdup(argv[i] + 9);
        }
    }

    if (argc < 2) { usage(); return 0; }

    const char *cmd = NULL;
    const char *subcmd = NULL;
    const char *arg3 = NULL;
    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--prefix=", 9) == 0) continue;
        if (!cmd) { cmd = argv[i]; }
        else if (!subcmd) { subcmd = argv[i]; }
        else if (!arg3) { arg3 = argv[i]; }
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
        do_list();
    } else if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0 || strcmp(cmd, "-v") == 0) {
        do_version();
    } else if (strcmp(cmd, "uninstall") == 0) {
        do_uninstall();
    } else if (strcmp(cmd, "prefix") == 0) {
        printf("%s\n", prefix());
    } else if (strcmp(cmd, "help") == 0 || strcmp(cmd, "--help") == 0 || strcmp(cmd, "-h") == 0) {
        usage();
    } else {
        fprintf(stderr, "mt: unknown command '%s'\n", cmd);
        usage();
        return 1;
    }

    return 0;
}
