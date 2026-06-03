#define _GNU_SOURCE
#include "config.h"

#include "json.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_CONFIG_SIZE (1024 * 1024)

enum config_type {
    CONFIG_NULLABLE_STRING,
    CONFIG_STRING,
    CONFIG_POLICY,
};

struct config_option {
    const char *name;
    enum config_type type;
    size_t offset;
    const char *description;
    const char *choices;
};

#define OPTION(name, type, field, description, choices)                                                                \
    {name, type, offsetof(struct lager_config, field), description, choices}

static const struct config_option options[] = {
    OPTION("kernel", CONFIG_NULLABLE_STRING, kernel, "Guest kernel path. null discovers an installed 4 KiB kernel.",
           "null or an absolute path"),
    OPTION("modules_dir", CONFIG_NULLABLE_STRING, modules_dir,
           "Guest kernel module tree. null infers it from the kernel name.", "null or an absolute path"),
    OPTION("resolution", CONFIG_STRING, resolution, "Guest mode and QEMU GTK window size.",
           "WIDTHxHEIGHT or an empty string"),
    OPTION("input", CONFIG_STRING, input,
           "Virtual pointing device type. Tablet gives seamless cursor tracking; mouse gives raw relative input.",
           "tablet or mouse"),
    OPTION("gpu_compat", CONFIG_POLICY, gpu_compat, "Patch the guest GPU module when mixed page sizes require it.",
           "auto, on, or off"),

};

#define OPTION_COUNT (sizeof(options) / sizeof(options[0]))

struct config_state {
    struct lager_config cfg;
    bool present[OPTION_COUNT];
};

void free_lager_config(struct lager_config *cfg)
{
    free(cfg->kernel);
    free(cfg->modules_dir);
    free(cfg->resolution);
    free(cfg->input);
    memset(cfg, 0, sizeof(*cfg));
}

static void init_lager_config(struct lager_config *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->gpu_compat = FEATURE_AUTO;
    cfg->resolution = xstrdup("1024x768");
    cfg->input = xstrdup("tablet");
}

static const struct config_option *find_option(const char *name, size_t len, size_t *index_out)
{
    size_t i;

    for (i = 0; i < OPTION_COUNT; i++) {
        if (strlen(options[i].name) == len && !memcmp(options[i].name, name, len)) {
            if (index_out)
                *index_out = i;
            return &options[i];
        }
    }
    return NULL;
}

static void *option_field(struct lager_config *cfg, const struct config_option *option)
{
    return (unsigned char *)cfg + option->offset;
}

static const void *const_option_field(const struct lager_config *cfg, const struct config_option *option)
{
    return (const unsigned char *)cfg + option->offset;
}

static char *dup_json_string(struct json_value_s *value, const char *name, bool nullable)
{
    struct json_string_s *string;

    if (json_value_is_null(value)) {
        if (nullable)
            return NULL;
        die("config key \"%s\" cannot be null", name);
    }
    string = json_value_as_string(value);
    if (!string || memchr(string->string, '\0', string->string_size))
        die("config key \"%s\" must be a string", name);
    return xstrndup(string->string, string->string_size);
}

static enum feature_policy parse_policy(const char *value, const char *name)
{
    if (!strcmp(value, "off"))
        return FEATURE_OFF;
    if (!strcmp(value, "auto"))
        return FEATURE_AUTO;
    if (!strcmp(value, "on"))
        return FEATURE_ON;
    die("config key \"%s\" must be \"off\", \"auto\", or \"on\"", name);
}

static const char *policy_name(enum feature_policy policy)
{
    switch (policy) {
    case FEATURE_OFF:
        return "off";
    case FEATURE_AUTO:
        return "auto";
    case FEATURE_ON:
        return "on";
    }
    die("invalid feature policy");
}

static void apply_option(struct lager_config *cfg, const struct config_option *option, struct json_value_s *value)
{
    void *field = option_field(cfg, option);

    switch (option->type) {
    case CONFIG_NULLABLE_STRING:
        free(*(char **)field);
        *(char **)field = dup_json_string(value, option->name, true);
        break;
    case CONFIG_STRING:
        free(*(char **)field);
        *(char **)field = dup_json_string(value, option->name, false);
        break;
    case CONFIG_POLICY: {
        char *text = dup_json_string(value, option->name, false);

        *(enum feature_policy *)field = parse_policy(text, option->name);
        free(text);
        break;
    }
    }
}

static void apply_config(struct config_state *state, struct json_value_s *root)
{
    struct json_object_s *object = json_value_as_object(root);
    struct json_object_element_s *element;

    if (!object)
        die("lager config root must be an object");
    for (element = object->start; element; element = element->next) {
        const struct config_option *option;
        size_t index;

        option = find_option(element->name->string, element->name->string_size, &index);
        if (!option)
            die("unknown config key: %.*s", (int)element->name->string_size, element->name->string);
        apply_option(&state->cfg, option, element->value);
        state->present[index] = true;
    }
}

static char *config_dir(void)
{
    const char *home = getenv("HOME");

    if (!home || !*home)
        die("HOME is not set; cannot locate ~/.config/lager/config.json");
    return xasprintf("%s/.config/lager", home);
}

static char *config_path(void)
{
    char *dir = config_dir();
    char *path = xasprintf("%s/config.json", dir);

    free(dir);
    return path;
}

static void ensure_config_dir(void)
{
    const char *home = getenv("HOME");
    char *base;
    char *dir;

    if (!home || !*home)
        die("HOME is not set; cannot locate ~/.config/lager/config.json");
    base = xasprintf("%s/.config", home);
    dir = xasprintf("%s/lager", base);
    mkdir_ok(base, 0755);
    mkdir_ok(dir, 0755);
    free(base);
    free(dir);
}

static void write_json_string(FILE *file, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)value;

    fputc('"', file);
    while (*cursor) {
        switch (*cursor) {
        case '"':
            fputs("\\\"", file);
            break;
        case '\\':
            fputs("\\\\", file);
            break;
        case '\b':
            fputs("\\b", file);
            break;
        case '\f':
            fputs("\\f", file);
            break;
        case '\n':
            fputs("\\n", file);
            break;
        case '\r':
            fputs("\\r", file);
            break;
        case '\t':
            fputs("\\t", file);
            break;
        default:
            if (*cursor < 0x20)
                fprintf(file, "\\u%04x", *cursor);
            else
                fputc(*cursor, file);
            break;
        }
        cursor++;
    }
    fputc('"', file);
}

static void write_option_value(FILE *file, const struct lager_config *cfg, const struct config_option *option)
{
    const void *field = const_option_field(cfg, option);

    switch (option->type) {
    case CONFIG_NULLABLE_STRING:
        if (*(char *const *)field)
            write_json_string(file, *(char *const *)field);
        else
            fputs("null", file);
        break;
    case CONFIG_STRING:
        write_json_string(file, *(char *const *)field);
        break;
    case CONFIG_POLICY:
        write_json_string(file, policy_name(*(const enum feature_policy *)field));
        break;
    }
}

static void write_config_object(FILE *file, const struct config_state *state, bool only_present)
{
    bool wrote = false;
    size_t i;

    fputs("{\n", file);
    for (i = 0; i < OPTION_COUNT; i++) {
        if (only_present && !state->present[i])
            continue;
        if (wrote)
            fputs(",\n", file);
        fputs("  ", file);
        write_json_string(file, options[i].name);
        fputs(": ", file);
        write_option_value(file, &state->cfg, &options[i]);
        wrote = true;
    }
    fputs(wrote ? "\n}\n" : "}\n", file);
}

static void save_config(const char *path, const struct config_state *state)
{
    char *temporary = xasprintf("%s.tmp.XXXXXX", path);
    int fd = mkstemp(temporary);
    FILE *file;

    if (fd < 0)
        die("create temporary config: %s", strerror(errno));
    if (fchmod(fd, 0644) < 0)
        die("chmod %s: %s", temporary, strerror(errno));
    file = fdopen(fd, "w");
    if (!file)
        die("open temporary config stream: %s", strerror(errno));
    write_config_object(file, state, true);
    if (fclose(file) < 0)
        die("close %s: %s", temporary, strerror(errno));
    if (rename(temporary, path) < 0)
        die("rename %s to %s: %s", temporary, path, strerror(errno));
    free(temporary);
}

static void parse_config_file(struct config_state *state, const char *path)
{
    struct json_parse_result_s result = {0};
    struct json_value_s *root;
    char *text;
    size_t size;

    text = read_file(path, MAX_CONFIG_SIZE, &size);
    root = json_parse_ex(text, size, json_parse_flags_default, NULL, NULL, &result);
    if (!root)
        die("parse %s:%zu:%zu: JSON error %zu", path, result.error_line_no, result.error_row_no, result.error);
    apply_config(state, root);
    free(root);
    free(text);
}

static void prepare_config_file(struct config_state *state, const char *path)
{
    ensure_config_dir();
    if (path_exists(path))
        return;
    save_config(path, state);
    fprintf(stderr, "lager: created config: %s\n", path);
}

static struct config_state load_config_state(void)
{
    struct config_state state = {0};
    char *path = config_path();

    init_lager_config(&state.cfg);
    prepare_config_file(&state, path);
    parse_config_file(&state, path);
    free(path);
    return state;
}

void load_lager_config(struct lager_config *cfg)
{
    struct config_state state = load_config_state();

    *cfg = state.cfg;
}

static void set_cli_option(struct config_state *state, const struct config_option *option, size_t index,
                           const char *value)
{
    void *field = option_field(&state->cfg, option);

    switch (option->type) {
    case CONFIG_NULLABLE_STRING:
        free(*(char **)field);
        *(char **)field = !strcmp(value, "null") ? NULL : xstrdup(value);
        break;
    case CONFIG_STRING:
        free(*(char **)field);
        *(char **)field = xstrdup(value);
        break;
    case CONFIG_POLICY:
        *(enum feature_policy *)field = parse_policy(value, option->name);
        break;
    }
    state->present[index] = true;
}

static void copy_option(struct lager_config *target, const struct lager_config *source,
                        const struct config_option *option)
{
    void *to = option_field(target, option);
    const void *from = const_option_field(source, option);

    switch (option->type) {
    case CONFIG_NULLABLE_STRING:
    case CONFIG_STRING:
        free(*(char **)to);
        *(char **)to = *(char *const *)from ? xstrdup(*(char *const *)from) : NULL;
        break;
    case CONFIG_POLICY:
        *(enum feature_policy *)to = *(const enum feature_policy *)from;
        break;
    }
}

static void print_one_option(const struct lager_config *cfg, const struct lager_config *defaults,
                             const struct config_option *option)
{
    printf("%s = ", option->name);
    write_option_value(stdout, cfg, option);
    printf("\n  %s\n  Choices: %s\n  Default: ", option->description, option->choices);
    write_option_value(stdout, defaults, option);
    printf("\n");
}

static void print_config_help(const struct config_state *state, const char *name)
{
    struct lager_config defaults;
    const struct config_option *option;
    size_t i;

    init_lager_config(&defaults);
    if (name) {
        option = find_option(name, strlen(name), NULL);
        if (!option)
            die("unknown config key: %s", name);
        print_one_option(&state->cfg, &defaults, option);
    } else {
        for (i = 0; i < OPTION_COUNT; i++) {
            if (i)
                printf("\n");
            print_one_option(&state->cfg, &defaults, &options[i]);
        }
    }
    free_lager_config(&defaults);
}

static void print_config_usage(void)
{
    fprintf(stderr, "usage: lager -config [--json | --list | --edit]\n"
                    "       lager -config KEY [VALUE]\n"
                    "       lager -config --unset KEY\n");
}

int handle_lager_config_cli(int argc, char **argv)
{
    struct config_state state;
    const struct config_option *option;
    char *path;
    size_t index;

    if (argc < 2 || strcmp(argv[1], "-config"))
        return -1;
    state = load_config_state();
    path = config_path();
    if (argc == 2) {
        print_config_help(&state, NULL);
    } else if (argc == 3 && !strcmp(argv[2], "--json")) {
        write_config_object(stdout, &state, false);
    } else if (argc == 3 && !strcmp(argv[2], "--list")) {
        for (index = 0; index < OPTION_COUNT; index++) {
            printf("%s = ", options[index].name);
            write_option_value(stdout, &state.cfg, &options[index]);
            printf("\n");
        }
    } else if (argc == 3 && !strcmp(argv[2], "--edit")) {
        const char *editor = getenv("EDITOR");

        if (!editor || !*editor)
            editor = "vi";
        execlp(editor, editor, path, (char *)NULL);
        die("exec %s: %s", editor, strerror(errno));
    } else if (argc == 3) {
        print_config_help(&state, argv[2]);
    } else if (argc == 4 && !strcmp(argv[2], "--unset")) {
        struct lager_config defaults;

        option = find_option(argv[3], strlen(argv[3]), &index);
        if (!option)
            die("unknown config key: %s", argv[3]);
        init_lager_config(&defaults);
        copy_option(&state.cfg, &defaults, option);
        state.present[index] = false;
        free_lager_config(&defaults);
        save_config(path, &state);
    } else if (argc == 4) {
        option = find_option(argv[2], strlen(argv[2]), &index);
        if (!option)
            die("unknown config key: %s", argv[2]);
        set_cli_option(&state, option, index, argv[3]);
        save_config(path, &state);
    } else {
        print_config_usage();
        free(path);
        free_lager_config(&state.cfg);
        return EXIT_FAILURE;
    }
    free(path);
    free_lager_config(&state.cfg);
    return EXIT_SUCCESS;
}
