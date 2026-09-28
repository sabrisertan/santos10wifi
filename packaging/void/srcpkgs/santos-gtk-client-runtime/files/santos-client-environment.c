#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define DEFAULT_PROFILE "/etc/santos/gtk4-client.ini"
#define CLIENT_DATA_DIRS "/usr/lib/santos-gtk-client-runtime/share:/usr/local/share:/usr/share"
#define DBUS_UPDATE_ACTIVATION_ENVIRONMENT "/usr/bin/dbus-update-activation-environment"

extern char **environ;

static const char *const profile_variables[] = {
    "GDK_BACKEND", "GSK_RENDERER", "GDK_DISABLE", "LD_LIBRARY_PATH", "LD_PRELOAD",
    "LIBEGL", "LIBGLESV2", "HYBRIS_ANDROID_SDK_VERSION", "HYBRIS_LINKER_DIR",
    "HYBRIS_EGLPLATFORM", "HYBRIS_EGLPLATFORM_DIR", "EGL_PLATFORM",
    "SANTOS_EGL_PLATFORM", "ANDROID_ROOT", "ANDROID_DATA", "LD_SHIM_LIBS",
};

static const char *const extra_private_variables[] = {
    "GI_TYPELIB_PATH", "GNOME_SHELL_DATADIR", "GSETTINGS_SCHEMA_DIR", "ICU_DATA",
    "COGL_DEBUG", "STATE_DIR", "SHELL_BIN", "SANTOS_GLES2_TRACE",
};

struct env_entry {
    char *key;
    char *value;
};

struct env_list {
    struct env_entry *items;
    size_t length;
    size_t capacity;
};

struct string_list {
    char **items;
    size_t length;
    size_t capacity;
};

enum run_mode {
    MODE_NONE = 0,
    MODE_EXEC,
    MODE_SHOW,
    MODE_PUBLISH,
};

static void
usage(const char *message)
{
    if (message != NULL)
        fprintf(stderr, "santos-client-environment: %s\n", message);
    fprintf(stderr,
            "usage: santos-client-environment [-h] [--profile PROFILE] "
            "(--publish | --show | --exec COMMAND [ARG...])\n");
    exit(2);
}

static void
input_error(const char *message)
{
    fprintf(stderr, "santos-client-environment: %s\n", message);
    exit(1);
}

static bool
contains_name(const char *const *names, size_t count, const char *name)
{
    for (size_t i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0)
            return true;
    }
    return false;
}

static bool
is_profile_variable(const char *name)
{
    return contains_name(profile_variables,
                         sizeof(profile_variables) / sizeof(profile_variables[0]),
                         name);
}

static bool
is_private_name(const char *name)
{
    return is_profile_variable(name) ||
           contains_name(extra_private_variables,
                         sizeof(extra_private_variables) / sizeof(extra_private_variables[0]),
                         name);
}

static char *
trim(char *text)
{
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text))
        text++;

    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        *--end = '\0';

    return text;
}

static bool
parse_boolean(const char *value, bool *result)
{
    if (strcasecmp(value, "1") == 0 || strcasecmp(value, "yes") == 0 ||
        strcasecmp(value, "true") == 0 || strcasecmp(value, "on") == 0) {
        *result = true;
        return true;
    }
    if (strcasecmp(value, "0") == 0 || strcasecmp(value, "no") == 0 ||
        strcasecmp(value, "false") == 0 || strcasecmp(value, "off") == 0) {
        *result = false;
        return true;
    }
    return false;
}

static void
list_add_entry(struct env_list *list, char *key, char *value)
{
    if (list->length == list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 16 : list->capacity * 2;
        struct env_entry *new_items = realloc(list->items,
                                              new_capacity * sizeof(*new_items));
        if (new_items == NULL)
            input_error("out of memory");
        list->items = new_items;
        list->capacity = new_capacity;
    }

    list->items[list->length].key = key;
    list->items[list->length].value = value;
    list->length++;
}

static struct env_entry *
find_entry(struct env_list *list, const char *key)
{
    for (size_t i = 0; i < list->length; i++) {
        if (strcmp(list->items[i].key, key) == 0)
            return &list->items[i];
    }
    return NULL;
}

static const struct env_entry *
find_entry_const(const struct env_list *list, const char *key)
{
    for (size_t i = 0; i < list->length; i++) {
        if (strcmp(list->items[i].key, key) == 0)
            return &list->items[i];
    }
    return NULL;
}

static void
add_or_replace_entry(struct env_list *list, const char *key, const char *value)
{
    if (find_entry(list, key) != NULL)
        input_error("duplicate profile environment key");

    char *new_key = strdup(key);
    char *new_value = strdup(value);
    if (new_key == NULL || new_value == NULL)
        input_error("out of memory");
    list_add_entry(list, new_key, new_value);
}

static void
load_profile(const char *path, struct env_list *environment,
             bool *enabled, bool *have_enabled)
{
    FILE *stream = fopen(path, "r");
    if (stream == NULL) {
        fprintf(stderr, "santos-client-environment: cannot open %s: %s\n",
                path, strerror(errno));
        exit(1);
    }

    enum { SECTION_NONE, SECTION_PROFILE, SECTION_ENVIRONMENT, SECTION_OTHER } section = SECTION_NONE;
    bool have_profile_section = false;
    bool have_environment_section = false;
    char *line = NULL;
    size_t line_capacity = 0;
    ssize_t line_length;

    while ((line_length = getline(&line, &line_capacity, stream)) >= 0) {
        char *text;
        char *equals;
        char *colon;
        char *separator;
        char *key;
        char *value;

        if (memchr(line, '\0', (size_t)line_length) != NULL) {
            free(line);
            fclose(stream);
            input_error("NUL in profile");
        }

        text = trim(line);
        if (*text == '\0' || *text == '#' || *text == ';')
            continue;

        if (*text == '[') {
            char *end = strrchr(text, ']');
            if (end == NULL || end[1] != '\0') {
                free(line);
                fclose(stream);
                input_error("malformed section header");
            }
            *end = '\0';
            text = trim(text + 1);
            if (strcmp(text, "Profile") == 0) {
                section = SECTION_PROFILE;
                have_profile_section = true;
            } else if (strcmp(text, "Environment") == 0) {
                section = SECTION_ENVIRONMENT;
                have_environment_section = true;
            } else {
                section = SECTION_OTHER;
            }
            continue;
        }

        equals = strchr(text, '=');
        colon = strchr(text, ':');
        if (equals == NULL || (colon != NULL && colon < equals))
            separator = colon;
        else
            separator = equals;
        if (separator == NULL) {
            free(line);
            fclose(stream);
            input_error("malformed profile entry");
        }

        *separator = '\0';
        key = trim(text);
        value = trim(separator + 1);
        if (*key == '\0' || strchr(key, '\n') != NULL || strchr(key, '\r') != NULL) {
            free(line);
            fclose(stream);
            input_error("invalid profile key");
        }
        if (strchr(value, '\n') != NULL || strchr(value, '\r') != NULL) {
            free(line);
            fclose(stream);
            input_error("invalid profile value");
        }

        if (section == SECTION_PROFILE) {
            if (strcmp(key, "Enabled") == 0) {
                bool parsed;
                if (*have_enabled || !parse_boolean(value, &parsed)) {
                    free(line);
                    fclose(stream);
                    input_error("invalid or duplicate Profile.Enabled");
                }
                *enabled = parsed;
                *have_enabled = true;
            }
        } else if (section == SECTION_ENVIRONMENT) {
            add_or_replace_entry(environment, key, value);
        } else if (section == SECTION_NONE) {
            free(line);
            fclose(stream);
            input_error("profile entry outside a section");
        }
    }

    if (ferror(stream)) {
        free(line);
        fclose(stream);
        input_error("error reading profile");
    }
    free(line);
    fclose(stream);

    if (!have_profile_section || !have_enabled)
        input_error("profile is missing Profile.Enabled");
    if (*enabled && !have_environment_section)
        input_error("profile is missing [Environment]");
}

static void
validate_profile(const struct env_list *environment, bool enabled)
{
    if (!enabled)
        return;

    for (size_t i = 0; i < environment->length; i++) {
        const char *key = environment->items[i].key;
        if (!is_profile_variable(key)) {
            fprintf(stderr, "santos-client-environment: unknown profile environment key: %s\n", key);
            exit(2);
        }
    }

    const struct env_entry *hybris = find_entry_const(environment, "HYBRIS_EGLPLATFORM");
    const struct env_entry *egl = find_entry_const(environment, "EGL_PLATFORM");
    if (hybris == NULL || strcmp(hybris->value, "wayland") != 0 ||
        egl == NULL || strcmp(egl->value, "wayland") != 0)
        input_error("client profile must select Wayland");
}

static void
string_list_add(struct string_list *list, char *value)
{
    if (list->length + 1 >= list->capacity) {
        size_t new_capacity = list->capacity == 0 ? 64 : list->capacity * 2;
        char **new_items = realloc(list->items, new_capacity * sizeof(*new_items));
        if (new_items == NULL)
            input_error("out of memory");
        list->items = new_items;
        list->capacity = new_capacity;
    }
    list->items[list->length++] = value;
    list->items[list->length] = NULL;
}

static char *
join_entry(const struct env_entry *entry)
{
    size_t length = strlen(entry->key) + 1 + strlen(entry->value) + 1;
    char *result = malloc(length);
    if (result == NULL)
        input_error("out of memory");
    snprintf(result, length, "%s=%s", entry->key, entry->value);
    return result;
}

static bool
environment_key_is(const char *entry, const char *key)
{
    size_t length = strlen(key);
    size_t entry_length = strlen(entry);
    return entry_length > length && strncmp(entry, key, length) == 0 && entry[length] == '=';
}

static char **
build_environment(const struct env_list *profile, bool apply_profile)
{
    struct string_list list = {0};

    for (char **entry = environ; entry != NULL && *entry != NULL; entry++) {
        const char *equals = strchr(*entry, '=');
        if (equals == NULL)
            continue;
        size_t key_length = (size_t)(equals - *entry);
        char *key = strndup(*entry, key_length);
        if (key == NULL)
            input_error("out of memory");
        bool skip = is_private_name(key) || strcmp(key, "XDG_DATA_DIRS") == 0;
        free(key);
        if (!skip)
            string_list_add(&list, *entry);
    }

    const size_t xdg_length = sizeof("XDG_DATA_DIRS=") + sizeof(CLIENT_DATA_DIRS);
    char *xdg = malloc(xdg_length);
    if (xdg == NULL)
        input_error("out of memory");
    snprintf(xdg, xdg_length, "XDG_DATA_DIRS=%s", CLIENT_DATA_DIRS);
    string_list_add(&list, xdg);

    if (apply_profile) {
        for (size_t i = 0; i < profile->length; i++)
            string_list_add(&list, join_entry(&profile->items[i]));
    }

    return list.items;
}

static int
run_publish(const struct env_list *profile)
{
    if (profile->length == 0)
        return 0;

    struct string_list arguments = {0};
    string_list_add(&arguments, (char *)DBUS_UPDATE_ACTIVATION_ENVIRONMENT);
    for (size_t i = 0; i < profile->length; i++)
        string_list_add(&arguments, join_entry(&profile->items[i]));

    char **environment = build_environment(profile, false);
    pid_t child = fork();
    if (child < 0)
        input_error("fork failed");
    if (child == 0) {
        execve(DBUS_UPDATE_ACTIVATION_ENVIRONMENT, arguments.items, environment);
        _exit(127);
    }

    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            input_error("waitpid failed");
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return 1;
}

static char *
path_from_environment(char **environment)
{
    for (size_t i = 0; environment[i] != NULL; i++) {
        if (environment_key_is(environment[i], "PATH")) {
            const char *value = strchr(environment[i], '=');
            return strdup(value + 1);
        }
    }
    return strdup("/bin:/usr/bin");
}

static void
exec_command(char **command, char **environment)
{
    if (strchr(command[0], '/') != NULL) {
        execve(command[0], command, environment);
        fprintf(stderr, "santos-client-environment: cannot execute %s: %s\n",
                command[0], strerror(errno));
        _exit(127);
    }

    char *path = path_from_environment(environment);
    if (path == NULL)
        input_error("out of memory");
    if (*path == '\0') {
        free(path);
        path = strdup("/bin:/usr/bin");
        if (path == NULL)
            input_error("out of memory");
    }
    char *path_copy = strdup(path);
    if (path_copy == NULL)
        input_error("out of memory");

    char *saveptr = NULL;
    for (char *directory = strtok_r(path_copy, ":", &saveptr);
         directory != NULL;
         directory = strtok_r(NULL, ":", &saveptr)) {
        if (*directory == '\0')
            directory = ".";
        size_t length = strlen(directory) + 1 + strlen(command[0]) + 1;
        char *candidate = malloc(length);
        if (candidate == NULL)
            input_error("out of memory");
        snprintf(candidate, length, "%s/%s", directory, command[0]);
        execve(candidate, command, environment);
        free(candidate);
    }

    fprintf(stderr, "santos-client-environment: cannot execute %s: %s\n",
            command[0], strerror(ENOENT));
    _exit(127);
}

static const char *
match_option(const char *argument, const char *full, const char *const *all,
             size_t count)
{
    if (strcmp(argument, full) == 0)
        return full;
    size_t length = strlen(argument);
    if (length == 0 || strncmp(argument, full, length) != 0)
        return NULL;

    const char *match = NULL;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(argument, all[i], length) == 0) {
            if (match != NULL)
                return NULL;
            match = all[i];
        }
    }
    return match;
}

int
main(int argc, char **argv)
{
    static const char *const options[] = {"--profile", "--publish", "--show", "--exec"};
    const char *profile_path = DEFAULT_PROFILE;
    enum run_mode mode = MODE_NONE;
    char **command = NULL;
    char **inline_command = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("usage: santos-client-environment [-h] [--profile PROFILE] "
                   "(--publish | --show | --exec COMMAND [ARG...])\n");
            return 0;
        }

        if (strncmp(argv[i], "--profile=", 10) == 0) {
            profile_path = argv[i] + 10;
            continue;
        }
        if (strncmp(argv[i], "--exec=", 7) == 0) {
            if (mode != MODE_NONE)
                usage("exactly one of --publish, --show, or --exec is required");
            mode = MODE_EXEC;
            size_t command_count = (size_t)(argc - i);
            inline_command = malloc((command_count + 1) * sizeof(*inline_command));
            if (inline_command == NULL)
                input_error("out of memory");
            inline_command[0] = &argv[i][7];
            for (size_t j = 1; j < command_count; j++)
                inline_command[j] = argv[i + (int)j];
            inline_command[command_count] = NULL;
            command = inline_command;
            if (*command == NULL || **command == '\0')
                usage("--exec needs a command");
            break;
        }

        const char *option = match_option(argv[i], "--profile", options, 4);
        if (option != NULL && strcmp(option, "--profile") == 0) {
            if (i + 1 >= argc)
                usage("--profile needs a path");
            profile_path = argv[++i];
            continue;
        }

        option = match_option(argv[i], "--publish", options, 4);
        if (option != NULL && strcmp(option, "--publish") == 0) {
            if (mode != MODE_NONE)
                usage("exactly one of --publish, --show, or --exec is required");
            mode = MODE_PUBLISH;
            continue;
        }

        option = match_option(argv[i], "--show", options, 4);
        if (option != NULL && strcmp(option, "--show") == 0) {
            if (mode != MODE_NONE)
                usage("exactly one of --publish, --show, or --exec is required");
            mode = MODE_SHOW;
            continue;
        }

        option = match_option(argv[i], "--exec", options, 4);
        if (option != NULL && strcmp(option, "--exec") == 0) {
            if (mode != MODE_NONE)
                usage("exactly one of --publish, --show, or --exec is required");
            mode = MODE_EXEC;
            command = &argv[i + 1];
            if (*command == NULL)
                usage("--exec needs a command");
            break;
        }

        usage("unrecognized argument");
    }

    if (mode == MODE_NONE)
        usage("exactly one of --publish, --show, or --exec is required");

    struct env_list profile = {0};
    bool enabled = false;
    bool have_enabled = false;
    load_profile(profile_path, &profile, &enabled, &have_enabled);
    validate_profile(&profile, enabled);

    if (mode == MODE_SHOW) {
        if (enabled) {
            for (size_t i = 0; i < profile.length; i++)
                printf("%s=%s\n", profile.items[i].key, profile.items[i].value);
        }
        return 0;
    }

    if (mode == MODE_PUBLISH)
        return run_publish(enabled ? &profile : &(struct env_list){0});

    char **environment = build_environment(&profile, enabled);
    exec_command(command, environment);
    return 127;
}
