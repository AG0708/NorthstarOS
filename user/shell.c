#include <northstar_user.h>

#define LINE_MAX 1024
#define TOKEN_MAX 128
#define TOKEN_TEXT_MAX 256
#define COMMAND_MAX 16
#define COMMAND_ARGS_MAX 32

enum token_type { TOKEN_WORD, TOKEN_PIPE, TOKEN_BACKGROUND, TOKEN_IN, TOKEN_OUT,
                  TOKEN_APPEND };

struct token {
    enum token_type type;
    char text[TOKEN_TEXT_MAX];
};

struct command {
    char *argv[COMMAND_ARGS_MAX + 1];
    int argc;
    char *input;
    char *output;
    int append;
};

static int last_status;

static uint64_t user_pointer(const void *value)
{
    return (uint64_t)(uintptr_t)value;
}

static int read_line(char *line, size_t capacity)
{
    size_t length = 0;
    while (length + 1u < capacity) {
        char byte;
        int64_t result = read(STDIN_FILENO, &byte, 1);
        if (result == 0)
            return length == 0 ? 0 : (int)length;
        if (result < 0)
            return -1;
        if (byte == '\r' || byte == '\n') {
            (void)write(STDOUT_FILENO, "\n", 1);
            break;
        }
        if (byte == '\b' || byte == 0x7f) {
            if (length != 0) {
                --length;
                (void)write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        if ((unsigned char)byte < 0x20 && byte != '\t')
            continue;
        line[length++] = byte;
        (void)write(STDOUT_FILENO, &byte, 1);
    }
    line[length] = '\0';
    return (int)length;
}

static int append_character(char *output, size_t *length, char character)
{
    if (*length + 1u >= TOKEN_TEXT_MAX)
        return -1;
    output[(*length)++] = character;
    output[*length] = '\0';
    return 0;
}

static int append_string(char *output, size_t *length, const char *string)
{
    while (*string != '\0') {
        if (append_character(output, length, *string++) != 0)
            return -1;
    }
    return 0;
}

static int expand_variable(const char **cursor, char *output, size_t *length)
{
    char name[64];
    size_t count = 0;
    const char *value;
    ++*cursor;
    if (**cursor == '?') {
        char status[16];
        ++*cursor;
        snprintf(status, sizeof(status), "%d", last_status);
        return append_string(output, length, status);
    }
    if (**cursor == '{') {
        ++*cursor;
        while (**cursor != '\0' && **cursor != '}' && count + 1u < sizeof(name))
            name[count++] = *(*cursor)++;
        if (**cursor != '}')
            return -1;
        ++*cursor;
    } else {
        while (((**cursor >= 'a' && **cursor <= 'z') ||
                (**cursor >= 'A' && **cursor <= 'Z') ||
                (**cursor >= '0' && **cursor <= '9') || **cursor == '_') &&
               count + 1u < sizeof(name))
            name[count++] = *(*cursor)++;
    }
    if (count == 0)
        return append_character(output, length, '$');
    name[count] = '\0';
    value = getenv(name);
    return value == NULL ? 0 : append_string(output, length, value);
}

static int lex(const char *line, struct token tokens[TOKEN_MAX], int *out_count)
{
    int count = 0;
    const char *cursor = line;
    while (*cursor != '\0') {
        struct token *token;
        size_t length = 0;
        int quote = 0;
        while (*cursor == ' ' || *cursor == '\t')
            ++cursor;
        if (*cursor == '\0' || *cursor == '#')
            break;
        if (count == TOKEN_MAX)
            return -1;
        token = &tokens[count++];
        token->text[0] = '\0';
        if (*cursor == '|') {
            token->type = TOKEN_PIPE;
            ++cursor;
            continue;
        }
        if (*cursor == '&') {
            token->type = TOKEN_BACKGROUND;
            ++cursor;
            continue;
        }
        if (*cursor == '<') {
            token->type = TOKEN_IN;
            ++cursor;
            continue;
        }
        if (*cursor == '>') {
            ++cursor;
            token->type = *cursor == '>' ? TOKEN_APPEND : TOKEN_OUT;
            if (*cursor == '>')
                ++cursor;
            continue;
        }
        token->type = TOKEN_WORD;
        while (*cursor != '\0') {
            char character = *cursor;
            if (quote == 0 && (character == ' ' || character == '\t' ||
                               character == '|' || character == '&' ||
                               character == '<' || character == '>'))
                break;
            if (character == '\'' && quote != '"') {
                quote = quote == '\'' ? 0 : '\'';
                ++cursor;
                continue;
            }
            if (character == '"' && quote != '\'') {
                quote = quote == '"' ? 0 : '"';
                ++cursor;
                continue;
            }
            if (character == '\\' && quote != '\'') {
                ++cursor;
                if (*cursor == '\0')
                    return -1;
                if (append_character(token->text, &length, *cursor++) != 0)
                    return -1;
                continue;
            }
            if (character == '$' && quote != '\'') {
                if (expand_variable(&cursor, token->text, &length) != 0)
                    return -1;
                continue;
            }
            if (append_character(token->text, &length, character) != 0)
                return -1;
            ++cursor;
        }
        if (quote != 0)
            return -1;
    }
    *out_count = count;
    return 0;
}

static int parse(struct token tokens[TOKEN_MAX], int token_count,
                 struct command commands[COMMAND_MAX], int *out_commands,
                 int *out_background)
{
    int command_count = 1;
    int background = 0;
    memset(commands, 0, sizeof(*commands) * COMMAND_MAX);
    for (int index = 0; index < token_count; ++index) {
        struct command *command = &commands[command_count - 1];
        struct token *token = &tokens[index];
        if (token->type == TOKEN_WORD) {
            if (command->argc == COMMAND_ARGS_MAX)
                return -1;
            command->argv[command->argc++] = token->text;
            command->argv[command->argc] = NULL;
        } else if (token->type == TOKEN_PIPE) {
            if (command->argc == 0 || command_count == COMMAND_MAX)
                return -1;
            ++command_count;
        } else if (token->type == TOKEN_BACKGROUND) {
            if (index + 1 != token_count)
                return -1;
            background = 1;
        } else {
            if (++index >= token_count || tokens[index].type != TOKEN_WORD)
                return -1;
            if (token->type == TOKEN_IN) {
                if (command->input != NULL)
                    return -1;
                command->input = tokens[index].text;
            } else {
                if (command->output != NULL)
                    return -1;
                command->output = tokens[index].text;
                command->append = token->type == TOKEN_APPEND;
            }
        }
    }
    if (token_count != 0 && commands[command_count - 1].argc == 0)
        return -1;
    *out_commands = token_count == 0 ? 0 : command_count;
    *out_background = background;
    return 0;
}

static int builtin(struct command *command, int *should_exit)
{
    if (strcmp(command->argv[0], "cd") == 0) {
        const char *path = command->argc > 1 ? command->argv[1] : getenv("HOME");
        if (path == NULL)
            path = "/";
        if (chdir(path) != 0) {
            dprintf(STDERR_FILENO, "sh: cd: %s: errno %d\n", path, errno);
            return 1;
        }
        return 0;
    }
    if (strcmp(command->argv[0], "pwd") == 0) {
        char path[NS_PATH_MAX];
        if (getcwd(path, sizeof(path)) != 0) {
            dprintf(STDERR_FILENO, "sh: pwd: errno %d\n", errno);
            return 1;
        }
        puts(path);
        return 0;
    }
    if (strcmp(command->argv[0], "exit") == 0) {
        *should_exit = 1;
        return command->argc > 1 ? (int)strtol(command->argv[1], NULL, 10) :
                                   last_status;
    }
    if (strcmp(command->argv[0], "status") == 0) {
        printf("%d\n", last_status);
        return 0;
    }
    if (strcmp(command->argv[0], "wait") == 0) {
        int status = 0;
        int pid;
        if (command->argc != 1) {
            dprintf(STDERR_FILENO, "sh: wait: no arguments supported\n");
            return 2;
        }
        pid = waitpid(-1, &status, 0);
        if (pid < 0) {
            dprintf(STDERR_FILENO, "sh: wait: errno %d\n", errno);
            return 127;
        }
        printf("[%d] waited (%d)\n", pid, status);
        return status;
    }
    if (strcmp(command->argv[0], "help") == 0) {
        puts("builtins: cd pwd status wait help exit");
        puts("features: quotes, $VAR, pipelines, <, >, >>, and trailing &");
        return 0;
    }
    return -1;
}

static void close_pipes(int pipes[COMMAND_MAX - 1][2], int count)
{
    for (int index = 0; index < count; ++index) {
        if (pipes[index][0] >= 0)
            (void)close(pipes[index][0]);
        if (pipes[index][1] >= 0)
            (void)close(pipes[index][1]);
        pipes[index][0] = pipes[index][1] = -1;
    }
}

static int execute_pipeline(struct command commands[COMMAND_MAX],
                            int command_count, int background)
{
    int pipes[COMMAND_MAX - 1][2];
    int pids[COMMAND_MAX];
    int spawned = 0;
    static const char *const default_environment[] = {
        "PATH=/bin", "HOME=/", "TERM=northstar", NULL};
    for (int index = 0; index < COMMAND_MAX - 1; ++index)
        pipes[index][0] = pipes[index][1] = -1;
    for (int index = 0; index + 1 < command_count; ++index) {
        if (pipe(pipes[index]) != 0) {
            dprintf(STDERR_FILENO, "sh: pipe: errno %d\n", errno);
            close_pipes(pipes, command_count - 1);
            return 1;
        }
    }
    for (int index = 0; index < command_count; ++index) {
        struct ns_spawn_action actions[NS_ARG_MAX];
        uint32_t action_count = 0;
        char path[NS_PATH_MAX];
        struct command *command = &commands[index];
        if (strchr(command->argv[0], '/') != NULL)
            snprintf(path, sizeof(path), "%s", command->argv[0]);
        else
            snprintf(path, sizeof(path), "/bin/%s", command->argv[0]);
        if (index != 0)
            actions[action_count++] = (struct ns_spawn_action){
                .type = NS_SPAWN_DUP2, .fd = STDIN_FILENO,
                .source_fd = pipes[index - 1][0]};
        if (index + 1 < command_count)
            actions[action_count++] = (struct ns_spawn_action){
                .type = NS_SPAWN_DUP2, .fd = STDOUT_FILENO,
                .source_fd = pipes[index][1]};
        for (int pipe_index = 0; pipe_index + 1 < command_count; ++pipe_index) {
            actions[action_count++] = (struct ns_spawn_action){
                .type = NS_SPAWN_CLOSE, .fd = pipes[pipe_index][0]};
            actions[action_count++] = (struct ns_spawn_action){
                .type = NS_SPAWN_CLOSE, .fd = pipes[pipe_index][1]};
        }
        if (command->input != NULL)
            actions[action_count++] = (struct ns_spawn_action){
                .type = NS_SPAWN_OPEN, .fd = STDIN_FILENO,
                .flags = NS_O_RDONLY, .path = user_pointer(command->input)};
        if (command->output != NULL)
            actions[action_count++] = (struct ns_spawn_action){
                .type = NS_SPAWN_OPEN, .fd = STDOUT_FILENO,
                .flags = NS_O_WRONLY | NS_O_CREAT |
                         (command->append ? NS_O_APPEND : NS_O_TRUNC),
                .path = user_pointer(command->output)};
        struct ns_spawn_args arguments = {
            .path = user_pointer(path),
            .argv = user_pointer(command->argv),
            .envp = user_pointer(environ != NULL ? environ :
                                 (char **)default_environment),
            .actions = user_pointer(actions),
            .action_count = action_count,
        };
        int pid = spawn(&arguments);
        if (pid < 0) {
            dprintf(STDERR_FILENO, "sh: %s: spawn failed: errno %d\n",
                    command->argv[0], errno);
            break;
        }
        pids[spawned++] = pid;
    }
    close_pipes(pipes, command_count - 1);
    if (spawned != command_count) {
        for (int index = 0; index < spawned; ++index)
            (void)waitpid(pids[index], NULL, 0);
        return 127;
    }
    if (background) {
        printf("[");
        for (int index = 0; index < spawned; ++index)
            printf("%s%d", index == 0 ? "" : " ", pids[index]);
        puts("]");
        return 0;
    }
    int status = 0;
    for (int index = 0; index < spawned; ++index) {
        int child_status = 0;
        if (waitpid(pids[index], &child_status, 0) < 0) {
            dprintf(STDERR_FILENO, "sh: wait: errno %d\n", errno);
            status = 127;
        } else if (index + 1 == spawned) {
            status = child_status;
        }
    }
    return status;
}

static void reap_background(void)
{
    for (;;) {
        int status;
        int pid = waitpid(-1, &status, NS_WNOHANG);
        if (pid <= 0)
            return;
        printf("[%d] done (%d)\n", pid, status);
    }
}

int main(void)
{
    char line[LINE_MAX];
    struct token tokens[TOKEN_MAX];
    struct command commands[COMMAND_MAX];
    puts("NorthstarOS shell. Type 'help' for commands.");
    for (;;) {
        int token_count;
        int command_count;
        int background;
        int should_exit = 0;
        reap_background();
        (void)write(STDOUT_FILENO, "northstar$ ", 11);
        int length = read_line(line, sizeof(line));
        if (length == 0)
            return last_status;
        if (length < 0) {
            dprintf(STDERR_FILENO, "sh: read: errno %d\n", errno);
            continue;
        }
        if (lex(line, tokens, &token_count) != 0 ||
            parse(tokens, token_count, commands, &command_count,
                  &background) != 0) {
            dprintf(STDERR_FILENO, "sh: syntax error\n");
            last_status = 2;
            continue;
        }
        if (command_count == 0)
            continue;
        if (command_count == 1 && !background && commands[0].input == NULL &&
            commands[0].output == NULL) {
            int result = builtin(&commands[0], &should_exit);
            if (result >= 0) {
                last_status = result;
                if (should_exit)
                    return last_status;
                continue;
            }
        }
        last_status = execute_pipeline(commands, command_count, background);
    }
}
