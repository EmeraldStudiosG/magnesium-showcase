/* Entry Point */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "magnesium.h"
#include "lsp.h"

static void print_usage(void) {
    printf("Magnesium v%s\n", MG_VERSION_STRING);
    printf("Usage: magnesium [options] <script.mg> | <script.mgc> | -\n");
    printf("Options:\n");
    printf("  -v, --version    Print version info\n");
    printf("  -c, --check      Syntax check (compile without executing)\n");
    printf("  -t, --time       Time the execution\n");
    printf("  --lsp            Start language server (LSP protocol over stdio)\n");
    printf("  build <script>   Compile to bytecode (.mgc)\n");
}

static char *read_file(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        fprintf(stderr, "Could not open file \"%s\".\n", path);
        exit(74);
    }

    if (fseek(file, 0L, SEEK_END) != 0) {
        fprintf(stderr, "Could not seek file \"%s\".\n", path);
        fclose(file);
        exit(74);
    }
    long end = ftell(file);
    if (end < 0 || (unsigned long)end > (unsigned long)(SIZE_MAX - 1)) {
        fprintf(stderr, "Could not determine the size of \"%s\".\n", path);
        fclose(file);
        exit(74);
    }
    size_t file_size = (size_t)end;
    if (fseek(file, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "Could not rewind file \"%s\".\n", path);
        fclose(file);
        exit(74);
    }

    char *buffer = (char *)malloc(file_size + 1);
    if (!buffer) {
        fprintf(stderr, "Not enough memory to read \"%s\".\n", path);
        fclose(file);
        exit(74);
    }

    size_t bytes_read = fread(buffer, sizeof(char), file_size, file);
    if (bytes_read < file_size) {
        fprintf(stderr, "Could not read file \"%s\".\n", path);
        fclose(file);
        free(buffer);
        exit(74);
    }
    if (memchr(buffer, '\0', file_size) != NULL) {
        fprintf(stderr, "Source file \"%s\" contains an embedded NUL byte.\n", path);
        fclose(file);
        free(buffer);
        exit(65);
    }

    buffer[bytes_read] = '\0';
    if (fclose(file) != 0) {
        fprintf(stderr, "Could not close file \"%s\".\n", path);
        free(buffer);
        exit(74);
    }
    return buffer;
}

static char *read_stdin(bool *contains_nul) {
    size_t capacity = 1024;
    size_t count = 0;
    char *buffer = malloc(capacity);
    if (!buffer) return NULL;
    if (contains_nul) *contains_nul = false;

    int c;
    while ((c = getchar()) != EOF) {
        if (count + 1 >= capacity) {
            if (capacity > SIZE_MAX / 2) {
                free(buffer);
                return NULL;
            }
            capacity *= 2;
            char *new_buf = realloc(buffer, capacity);
            if (!new_buf) { free(buffer); return NULL; }
            buffer = new_buf;
        }
        if (c == '\0' && contains_nul) *contains_nul = true;
        buffer[count++] = (char)c;
    }
    if (ferror(stdin)) {
        free(buffer);
        return NULL;
    }
    buffer[count] = '\0';
    return buffer;
}

static bool ends_with(const char *str, const char *suffix) {
    if (!str || !suffix) return false;
    size_t len_str = strlen(str);
    size_t len_suffix = strlen(suffix);
    if (len_suffix > len_str) return false;
    return strncmp(str + len_str - len_suffix, suffix, len_suffix) == 0;
}

static char *build_output_path(const char *source_path) {
    size_t length = strlen(source_path);
    bool replace_extension = ends_with(source_path, ".mg");
    size_t stem_length = replace_extension ? length - 3 : length;
    if (stem_length > SIZE_MAX - 5) return NULL;
    char *output = (char *)malloc(stem_length + 5);
    if (!output) return NULL;
    memcpy(output, source_path, stem_length);
    memcpy(output + stem_length, ".mgc", 5);
    return output;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    bool check_only = false;
    bool time_execution = false;
    const char *script_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "version") == 0) {
            printf("Magnesium v%s\n", MG_VERSION_STRING);
            return 0;
        } else if (strcmp(argv[i], "-c") == 0 || strcmp(argv[i], "--check") == 0) {
            check_only = true;
        } else if (strcmp(argv[i], "--lsp") == 0) {
            return mg_lsp_run() ? 0 : 1;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--time") == 0) {
            time_execution = true;
        } else if (strcmp(argv[i], "build") == 0) {
            if (i + 1 < argc) {
                const char *src_path = argv[i+1];
                if (i + 2 != argc) {
                    fprintf(stderr, "Error: build accepts exactly one script path.\n");
                    return 1;
                }
                char *out_path = build_output_path(src_path);
                if (!out_path) {
                    fprintf(stderr, "Out of memory creating output path.\n");
                    return 1;
                }

                char *source = read_file(src_path);
                VM *vm = vm_new();
                if (!vm) {
                    fprintf(stderr, "Out of memory.\n");
                    free(out_path);
                    free(source);
                    return 1;
                }
                ObjFunction *func = vm_compile_named(vm, source, src_path);
                if (!func) {
                    fprintf(stderr, "Compilation failed.\n");
                    vm_delete(vm);
                    free(out_path);
                    free(source);
                    return 65;
                }

                if (vm_save_bytecode(vm, func, out_path)) {
                    printf("Built %s\n", out_path);
                } else {
                    fprintf(stderr, "Failed to write %s\n", out_path);
                    vm_delete(vm);
                    free(out_path);
                    free(source);
                    return 1;
                }
                vm_delete(vm);
                free(out_path);
                free(source);
                return 0;
            } else {
                fprintf(stderr, "Error: No script provided to build.\n");
                return 1;
            }
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            fprintf(stderr, "Error: Unknown option \"%s\".\n", argv[i]);
            return 1;
        } else if (script_path) {
            fprintf(stderr, "Error: Multiple script paths were provided.\n");
            return 1;
        } else {
            script_path = argv[i];
        }
    }

    if (!script_path) {
        fprintf(stderr, "Error: No script provided.\n");
        print_usage();
        return 1;
    }

    VM *vm = vm_new();
    if (!vm) {
        fprintf(stderr, "Out of memory.\n");
        return 1;
    }
    InterpretResult result;

    if (strcmp(script_path, "-") == 0) {
        bool contains_nul = false;
        char *source = read_stdin(&contains_nul);
        if (!source) {
            fprintf(stderr, "Could not read standard input.\n");
            vm_delete(vm);
            return 74;
        }
        if (contains_nul) {
            fprintf(stderr, "Standard input contains an embedded NUL byte.\n");
            free(source);
            vm_delete(vm);
            return 65;
        }
        if (check_only) {
            ObjFunction *func = vm_compile_named(vm, source, "<stdin>");
            result = func ? INTERPRET_OK : INTERPRET_COMPILE_ERROR;
            if (result == INTERPRET_OK) printf("Syntax OK\n");
        } else if (time_execution) {
            clock_t start = clock();
            result = vm_interpret_named(vm, source, "<stdin>");
            clock_t end = clock();
            printf("\nExecution time: %.4f seconds\n",
                   ((double)(end - start)) / CLOCKS_PER_SEC);
        } else {
            result = vm_interpret_named(vm, source, "<stdin>");
        }
        free(source);
    } else if (ends_with(script_path, ".mgc")) {
        ObjFunction *func = vm_load_bytecode(vm, script_path);
        if (!func) {
            fprintf(stderr, "Could not load bytecode from %s\n", script_path);
            vm_delete(vm);
            return 1;
        }
        if (check_only) {
            result = INTERPRET_OK;
            printf("Bytecode OK\n");
        } else if (time_execution) {
            clock_t start = clock();
            result = vm_run_function(vm, func);
            clock_t end = clock();
            printf("\nExecution time: %.4f seconds\n", ((double)(end - start)) / CLOCKS_PER_SEC);
        } else {
            result = vm_run_function(vm, func);
        }
    } else {
        char *source = read_file(script_path);
        if (check_only) {
            ObjFunction *func = vm_compile_named(vm, source, script_path);
            result = func ? INTERPRET_OK : INTERPRET_COMPILE_ERROR;
            if (result == INTERPRET_OK) printf("Syntax OK\n");
        } else if (time_execution) {
            clock_t start = clock();
            result = vm_interpret_named(vm, source, script_path);
            clock_t end = clock();
            printf("\nExecution time: %.4f seconds\n", ((double)(end - start)) / CLOCKS_PER_SEC);
        } else {
            result = vm_interpret_named(vm, source, script_path);
        }
        free(source);
    }

    vm_delete(vm);

    switch (result) {
        case INTERPRET_COMPILE_ERROR: return 65;
        case INTERPRET_RUNTIME_ERROR: return 70;
        case INTERPRET_YIELD: return 70;
        case INTERPRET_OK: return 0;
    }
    return 0;
}
