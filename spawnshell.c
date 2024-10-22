/* $begin shellmain */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>

#define MAXARGS 128
#define MAXLINE 8192 /* Max text line length */

typedef enum { 
    IS_SIMPLE,
    IS_PIPE,
    IS_INPUT_REDIR,
    IS_OUTPUT_REDIR,
    IS_INPUT_OUTPUT_REDIR,
    IS_SEQ,
    IS_ANDIF,
    IS_ORIF
} Mode; /* simple command, |, >, <, ;, && */

typedef struct { 
    char *argv[MAXARGS]; /* Argument list */
    int argc; /* Number of args */
    int bg; /* Background job? */
    Mode mode; /* Handle special cases | > < ; */
} parsed_args; 

extern char **environ; /* Defined by libc */

/* Function prototypes */
void unix_error(char *msg);
void eval(char *cmdline);
parsed_args parseline(char *buf);
int builtin_command(char **argv, pid_t pid, int status);
char** split_argument_list(parsed_args* parsed_line, int indexToSplit);
char** truncate_argument_list(char** argv, int indexToTruncate);
int setup_output_redir(char** argv, posix_spawn_file_actions_t* actions);
int setup_input_redir(char** argv, posix_spawn_file_actions_t* actions);
int handle_pipe(parsed_args parsed_line, pid_t* pid, int* status);
void signal_handler(int sig);
int exec_cmd(char **argv, posix_spawn_file_actions_t *actions, pid_t *pid, int *status, int bg);
int find_index(char **argv, char *target); 

int main(int argc, char *argv[])
{
    char cmdline[MAXLINE]; /* Command line */
    
    signal(SIGINT, signal_handler);
    signal(SIGTSTP, signal_handler);
    signal(SIGCHLD, signal_handler);
    while (1) {
        char *result;
        /* Read */
        printf("CS361 >");
        result = fgets(cmdline, MAXLINE, stdin);
        if (result == NULL && ferror(stdin)) {
            fprintf(stderr, "fatal fgets error\n");
            exit(EXIT_FAILURE);
        }

        if (feof(stdin))
            exit(EXIT_SUCCESS);

        /* Evaluate */
        eval(cmdline);
    }
}

void unix_error(char *msg) /* Unix-style error */
{
    fprintf(stderr, "%s: %s\n", msg, strerror(errno));
    exit(EXIT_FAILURE);
}
/* $end shellmain */

/* $begin eval */
/* eval - Evaluate a command line */
void eval(char *cmdline)
{
    char buf[MAXLINE];   /* Holds modified command line */
    static pid_t pid;           /* Process id */
    static int status;          /* Process status */
    posix_spawn_file_actions_t actions; /* used in performing spawn operations */
    posix_spawn_file_actions_init(&actions); 

    strcpy(buf, cmdline);
    parsed_args parsed_line = parseline(buf);
    if (parsed_line.argv[0] == NULL) /* Ignore empty lines */
        return;

    /* Not a bultin command */
    if (!builtin_command(parsed_line.argv, pid, status)) {
        char** argv_second_command = NULL;
        int output_chevron_index = 0;
        int input_chevron_index = 0;
        switch (parsed_line.mode) {
        case IS_SIMPLE: /* cmd argv1 argv2 ... */
            if (!exec_cmd(parsed_line.argv, &actions, &pid, &status, parsed_line.bg))
                return;
            break;
        case IS_PIPE: /* command1 args | command2 args */
            if (!handle_pipe(parsed_line, &pid, &status)) {
                return;
            }
            break;
        case IS_OUTPUT_REDIR: /* command args > output_redirection */
            output_chevron_index = setup_output_redir(parsed_line.argv, &actions);
            if (!exec_cmd(truncate_argument_list(parsed_line.argv, output_chevron_index),
                 &actions, &pid, &status, parsed_line.bg))
                return;
            break;
        case IS_INPUT_REDIR: /* command args < input_redirection */
            input_chevron_index = setup_input_redir(parsed_line.argv, &actions);
            if (!exec_cmd(truncate_argument_list(parsed_line.argv, input_chevron_index), &actions, &pid, &status, parsed_line.bg))
                return;
            break;
        case IS_INPUT_OUTPUT_REDIR: /* command args < input_redirection > output_redirection */
            input_chevron_index = setup_input_redir(parsed_line.argv, &actions);
            output_chevron_index = setup_output_redir(parsed_line.argv, &actions);
            int first_chevron_index = ((input_chevron_index < output_chevron_index) ? input_chevron_index : output_chevron_index);
            if (!exec_cmd(truncate_argument_list(parsed_line.argv, first_chevron_index), &actions, &pid, &status, parsed_line.bg))
                return;
            break;
        case IS_SEQ: /* command1 args ; command2 args */
            int semicolonIndex = find_index(parsed_line.argv, ";");
            argv_second_command = split_argument_list(&parsed_line, semicolonIndex);

            if (!exec_cmd(parsed_line.argv, &actions, &pid, &status, parsed_line.bg))
                return;
            if (!exec_cmd(argv_second_command, &actions, &pid, &status, parsed_line.bg))
                return;
            break;
        case IS_ANDIF: /* command1 args && command2 args */
            int andifIndex = find_index(parsed_line.argv, "&&");
            argv_second_command = split_argument_list(&parsed_line, andifIndex);
            if (!exec_cmd(parsed_line.argv, &actions, &pid, &status, parsed_line.bg) || (status != 0))
                return;
            if (!exec_cmd(argv_second_command, &actions, &pid, &status, parsed_line.bg))
                return;
            break;
        case IS_ORIF: /* command1 args || command2 args */
            int orifIndex = find_index(parsed_line.argv, "||");
            argv_second_command = split_argument_list(&parsed_line, orifIndex);

            if (!exec_cmd(parsed_line.argv, &actions, &pid, &status, parsed_line.bg) || (status == 0))
                return;
            if (!exec_cmd(argv_second_command, &actions, &pid, &status, parsed_line.bg))
                return;
            break;
        }

        if (parsed_line.bg)
            printf("%d %s", pid, cmdline);
    }

    posix_spawn_file_actions_destroy(&actions);
    return;
}

/* Split argument list into two argument lists. */
char** split_argument_list(parsed_args* parsed_line, int indexToSplit) {
    truncate_argument_list(parsed_line->argv, indexToSplit);
    return parsed_line->argv + indexToSplit + 1;
}

/* Effectively truncates the argument list to end just before the specified index. */
char** truncate_argument_list(char** argv, int indexToTruncate) {
    argv[indexToTruncate] = NULL;
    return argv;
}

int setup_output_redir(char** argv, posix_spawn_file_actions_t* actions) {
    int output_chevron_index = find_index(argv, ">");
    char* output_path = argv[output_chevron_index + 1];
    if (posix_spawn_file_actions_addopen(actions, 1, output_path, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR) != 0)
        unix_error("Unable to open file for writing.");
    return output_chevron_index;
}

int setup_input_redir(char** argv, posix_spawn_file_actions_t* actions) {
    int input_chevron_index = find_index(argv, "<");
    char* input_path = argv[input_chevron_index + 1];
    if (posix_spawn_file_actions_addopen(actions, 0, input_path, O_RDONLY, S_IRUSR | S_IWUSR) != 0)
        unix_error("Unable to open file for reading. File may not exist.");
    return input_chevron_index;
}

int handle_pipe(parsed_args parsed_line, pid_t* pid, int* status) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0)
        return -1;
    posix_spawn_file_actions_t actions1;
    posix_spawn_file_actions_t actions2;
    pid_t pid1;
    pid_t pid2;
    
    posix_spawn_file_actions_init(&actions1);
    posix_spawn_file_actions_init(&actions2);
    posix_spawn_file_actions_adddup2(&actions1, pipe_fds[1], STDOUT_FILENO);  // Make the output of the first one...
    posix_spawn_file_actions_adddup2(&actions2, pipe_fds[0], STDIN_FILENO); // ...into the input of the second one.

    posix_spawn_file_actions_addclose(&actions1, pipe_fds[0]); // Close the first process's access to the reader end of the pipe.
    posix_spawn_file_actions_addclose(&actions2, pipe_fds[1]); // Close the second process's access to the writer end of the pipe.

    int pipe_index = find_index(parsed_line.argv, "|");
    char** argv_second_command = split_argument_list(&parsed_line, pipe_index);
    if (!exec_cmd(parsed_line.argv, &actions1, &pid1, status, 1)) // The first command is forced to run in the background
        return -1;
    if (!exec_cmd(argv_second_command, &actions2, &pid2, status, 1)) // As is the second (we will wait for both of them to finish next).
        return -1;
    close(pipe_fds[0]);
    close(pipe_fds[1]);
    if (!parsed_line.bg) { // If the command was run in the background 
        // (with a "&" at the end, noticed by parseline())...
        waitpid(pid1, status, 0); // ...wait for the processes to finish:
        waitpid(pid2, status, 0);
    }
    posix_spawn_file_actions_destroy(&actions1);
    posix_spawn_file_actions_destroy(&actions2);
    *pid = pid2;
    return 1;
}

/* If first arg is a builtin command, run it and return true */
int builtin_command(char **argv, pid_t pid, int status)
{
    if (!strcmp(argv[0], "exit")) /* exit command */
        exit(EXIT_SUCCESS);
    if (!strcmp(argv[0], "&")) /* Ignore singleton & */
        return 1;
    if (!strcmp(argv[0], "?")) {
        printf("\npid:%d status:%d\n", pid, status >> 8);
        return 1;
    }

    return 0; /* Not a builtin command */
}
/* $end eval */

/* $begin exec_cmd */
/* Run commands using posix_spawnp */
int exec_cmd(char **argv, posix_spawn_file_actions_t *actions, pid_t *pid, int *status, int bg)
{
    // Lab 5: use posix_spawnp to execute commands and when it fails,
    // print an error, "spawn failed", using the perror function
    int ret = posix_spawnp(pid, argv[0], actions, NULL, argv, environ);
    if (ret != 0)
        perror("spawn failed");
    // Lab 5: when posix_spawnp is ready, uncomment me
    if (!bg) {
        if (waitpid(*pid, status, 0) < 0) 
            unix_error("waitfg: waitpid error");
    }

    return 1;
}
/* $end exec_cmd */

/* signal handler */
void signal_handler(int sig)
{
    if (sig == SIGINT) {
        printf("\ncaught sigint\nCS361 >");
        fflush(stdout);
        return;
    } 
    if (sig == SIGTSTP) {
        printf("\ncaught sigtstp\nCS361 >");
        fflush(stdout);
        return;
    }
    if (sig == SIGCHLD) {
        int child_status;
        wait(&child_status);
    }
}

/* finds index of the matching target in the arguments */
int find_index(char **argv, char *target)
{
    for (int i = 0; argv[i] != NULL; i++) {
        if (!strcmp(argv[i], target))
            return i;
    }
    
    return 0;
}

/* $begin parseline */
/* parseline - Parse the command line and build the argv array */
parsed_args parseline(char *buf)
{
    char *delim; /* Points to first space delimiter */
    parsed_args pa;

    buf[strlen(buf) - 1] = ' ';   /* Replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) { /* Ignore leading spaces */
        buf++;
    }

    /* Build the argv list */
    pa.argc = 0;
    while ((delim = strchr(buf, ' '))) {
        pa.argv[pa.argc++] = buf;
        *delim = '\0';
        buf = delim + 1;

        while (*buf && (*buf == ' ')) { /* Ignore spaces */
            buf++;
        }
    }
    pa.argv[pa.argc] = NULL;

    if (pa.argc == 0) /* Ignore blank line */
        return pa;

    /* Should the job run in the background? */
    if ((pa.bg = (*pa.argv[pa.argc - 1] == '&')) != 0)
        pa.argv[--pa.argc] = NULL;

    /* Detect various command modes */
    pa.mode = IS_SIMPLE;
    if (find_index(pa.argv, "|"))
        pa.mode = IS_PIPE;
    else if (find_index(pa.argv, ";")) 
        pa.mode = IS_SEQ; 
    else if (find_index(pa.argv, "&&"))
        pa.mode = IS_ANDIF;
    else if (find_index(pa.argv, "||"))
        pa.mode = IS_ORIF;
    else {
        if (find_index(pa.argv, "<")) 
            pa.mode = IS_INPUT_REDIR;
        if (find_index(pa.argv, ">")) {
            if (pa.mode == IS_INPUT_REDIR)
                pa.mode = IS_INPUT_OUTPUT_REDIR;
            else
                pa.mode = IS_OUTPUT_REDIR; 
        }
    }

    return pa;
}
/* $end parseline */
