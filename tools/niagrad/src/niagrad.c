/* Copyright: Apkudo LLC 2014: See LICENSE file. */

#include <sys/types.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>

#include "str.h"

#define STATE_DIR "/tmp"
#define SYSLOG_IDENT "niagra"
#define FD_PREFIX " --fd "
#define FD_PREFIX_SIZE (sizeof FD_PREFIX)
#define ENV_PREFIX " --env "
#define ENV_PREFIX_SIZE (sizeof ENV_PREFIX)
#define FILE_PREFIX " --file "
#define FILE_PREFIX_SIZE (sizeof FILE_PREFIX)
#define INT_STRING_LEN 10
#define FD_ARG_LEN (FD_PREFIX_SIZE + MAX_FD_NAME + INT_STRING_LEN)
#define ENV_ARG_LEN (ENV_PREFIX_SIZE + MAX_ENV_NAME)
#define FILE_ARG_LEN (FILE_PREFIX_SIZE + MAX_FILEKEY_NAME + INT_STRING_LEN)
#define APP_OPTION_ARG_LEN (MAX_APP_OPTION_NAME + MAX_APP_OPTION_VALUE + 2)
#define INT_STRING_LEN 10
#define MAX_LINE_SIZE 4096
#define MAX_COMMAND_LINE 1024
#define MAX_FD_NAME 64
#define MAX_FD_TYPE 64
#define MAX_ENV_NAME 64
#define MAX_APP_OPTION_NAME 64
#define MAX_APP_OPTION_VALUE 256
#define MAX_FDS 10
#define MAX_FILES 10
#define MAX_FILE_NAME 1024
#define MAX_FILEKEY_NAME 64
#define MAX_APP_OPTIONS 10
#define MAX_TIME_STRING 26
#define NUM_SOCK_OPTIONS 6
#define NUM_FILE_OPTIONS 2
#define NO_PID 0

/* Maximum number of node instances to spawn. One per core is probably good. */
#define MAX_COPIES 10

/* Maximum number of times to migrate before old servers just get killed. */
#define MAX_MIGRATE_BACKLOG 4

enum fd_type { SOCKET_FD, FILE_FD };

struct fd_socket {
    int ip_ver;
    struct in_addr addr;
    uint16_t port;
    int backlog;
};

struct fd_file {
    char filename[MAX_FILE_NAME];
};

struct fd {
    char name[MAX_FD_NAME];
    char type[MAX_FD_TYPE];
    int fd;
    enum fd_type fd_type;
    union {
        struct fd_socket sock;
        struct fd_file file;
    } x;
};

struct file {
    char key[MAX_FILEKEY_NAME];
    char name[MAX_FILE_NAME];
    int fd;
};

struct app_option {
    char name[MAX_APP_OPTION_NAME];
    char value[MAX_APP_OPTION_VALUE];
};

static void parse_config_file(void);
static void update_command_line(void);
static char *get_parent_dir(const char *file);
static void change_dir(void);

static void create_sockets(void);
static int create_socket(struct in_addr addr, uint16_t port, int backlog);
static void install_signal_handlers(void);
static int lookup_fd_by_name(const char *name);

static void open_files(void);
static int lookup_file_by_key(const char *name);

static void drop_privs(void);

static int find_server(pid_t pid);
static void migrate_server(int server, pid_t pid);
static void migrate_servers(void);
static void restart_servers(void);
static void spawn_server(int server);
static void spawn_servers(void);
static void terminate_servers(void);
static void terminate_server(int server);

static void clear_backlog_server(pid_t pid);
static void set_backlog_server(int server, pid_t pid);
static void terminate_backlog_servers(int backlog_index);
static void shift_backlog_servers(void);

static void output_state(pid_t caller);

#if defined(DEBUG)
static void fprint_fd_socket(FILE *f, struct fd *fd);
#endif

static pid_t niagra_pid;
static const char *config_file_name;
static const char *config_file_dir;
static const char *config_logfile;
static char server_command[MAX_COMMAND_LINE];
static char config_environment[MAX_ENV_NAME];
static struct fd fds[MAX_FDS];
static int num_fds;
static struct file files[MAX_FILES];
static int num_files;
static struct app_option app_options[MAX_APP_OPTIONS];
static int num_app_options;
static int copies = 1;
static pid_t servers[MAX_COPIES];
static pid_t backlog_servers[MAX_MIGRATE_BACKLOG][MAX_COPIES];
static bool debug_mode = false;
static bool no_respawn = false;
static bool fast_spawn_protect = false;
static struct timeval last_sigint_time;
static struct timeval last_spawn_time;
static int stat_restart_request_count;
static int stat_migrate_request_count;
static int stat_state_request_count;
static int stat_restart_node_expected_count;
static int stat_restart_node_unexpected_count;
static int stat_migrate_node_count;
static int stat_backlog_node_count;
static char stat_start_time[MAX_TIME_STRING];
static char stat_restart_last_request_time[MAX_TIME_STRING];
static char stat_migrate_last_request_time[MAX_TIME_STRING];
static char stat_migrate_last_node_time[MAX_TIME_STRING];
static char stat_restart_last_node_expected_time[MAX_TIME_STRING];
static char stat_restart_last_node_unexpected_time[MAX_TIME_STRING];

static void
usage(void)
{
    printf("niagrad: [-d] [-n] config [logfile]\n");
    exit(EXIT_FAILURE);
}

static void
daemonize(void)
{
    pid_t child;
    int fd;

    if ((child = fork()) == -1) {
        syslog(LOG_ERR, "error forking: %m");
        exit(EXIT_FAILURE);
    }

    if (child == 0) {
        (void) close(STDIN_FILENO);
        (void) close(STDOUT_FILENO);
        (void) close(STDERR_FILENO);

        /* Open /dev/null */
        /* I really don't like this code. It should likely be refactored or removed. */
        fd = open("/dev/null", O_RDONLY);
        if (fd != STDIN_FILENO) {
            syslog(LOG_ALERT, "Expected file to be opened with fd %d, not %d", STDIN_FILENO, fd);
            exit(EXIT_FAILURE);
        }

        fd = open(config_logfile, O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd != STDOUT_FILENO) {
            syslog(LOG_ALERT, "Expected file to be opened with fd %d, not %d", STDOUT_FILENO, fd);
            exit(EXIT_FAILURE);
        }

        fd = dup2(fd, STDERR_FILENO);
        if (fd != STDERR_FILENO) {
            syslog(LOG_ALERT, "Expected file to be opened with fd %d, not %d", STDERR_FILENO, fd);
            exit(EXIT_FAILURE);
        }

        (void) umask(027);

        if (setsid() == -1) {
            syslog(LOG_ALERT, "unable to become session leader: %m");
            exit(EXIT_FAILURE);
        }

    } else {
        exit(EXIT_SUCCESS);
    }
}

static char *
get_parent_dir(const char *file)
{
    char *r, *c = strrchr(file, '/');
    if (c == NULL) {
        r = ".";
    } else if (c == file) {
        r = "/";
    } else {
        r = malloc(strlen(file) + 1);
        strcpy(r, file);
        c = strrchr(r, '/');
        *c = '\0';
    }
    return r;
}

/* Change pwd location of config file. */
static void
change_dir(void)
{
    if (chdir(config_file_dir) != 0) {
        syslog(LOG_ALERT, "unable to change directory: %m");
        exit(EXIT_FAILURE);
    }
}

static void
drop_privs(void)
{
    /* TODO: implement. */
}

static void
store_time(char *buf)
{
    time_t now = time(0);
    str_copy(buf, asctime(localtime(&now)), MAX_TIME_STRING);
    /* Remove trailing newline. */
    buf[strlen(buf) - 1] = '\0';
}

int
main(int argc, char **argv)
{
    pid_t pid;
    int status;
    int ch;
    int logopt = LOG_NDELAY;

    store_time(stat_start_time);

    while ((ch = getopt(argc, argv, "dn")) != -1) {
        switch (ch) {
        case 'd':
            debug_mode = true;
            break;
        case 'n':
            no_respawn = true;
            break;
        case '?':
        default:
            usage();
        }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1 && argc != 2) {
        usage();
    }

    config_file_name = argv[0];
    config_file_dir = get_parent_dir(config_file_name);

    if (argc == 2) {
        config_logfile = argv[1];
    } else {
        config_logfile = (debug_mode ? "stdout" : "niagra.log");
    }

    if (!debug_mode) {
        daemonize();
    }

    logopt |= LOG_PERROR;

    openlog(SYSLOG_IDENT, logopt, LOG_DAEMON);

    niagra_pid = getpid();
    syslog(LOG_INFO, "niagrad started: %ld", (long) niagra_pid);

    install_signal_handlers();

    parse_config_file();
    if (str_isempty(server_command)) {
        syslog(LOG_ERR, "no command specified.");
        exit(EXIT_FAILURE);
    }

    change_dir();

    create_sockets();

    open_files();

    update_command_line();

    drop_privs();

    spawn_servers();

    for (;;) {
        bool respawn = !no_respawn;
        pid = waitpid(-1, &status, 0);

        if (pid == -1) {
            syslog(LOG_ERR, "error waiting for process: %m");
            exit(EXIT_FAILURE);
        }

        if (WIFEXITED(status)) {
            switch (WEXITSTATUS(status)) {
            case 126:
            case 127:
                /* we treat 126 and 127 as errors from the shell itself */
                syslog(LOG_ERR, "process exited with shell error: pid: %ld status: %d", (long) pid,
                       WEXITSTATUS(status));
                respawn = false;
                break;
            default:
                syslog(LOG_INFO, "process exited. pid: %ld status: %d", (long) pid, WEXITSTATUS(status));
                break;
            }
        } else if (WIFSIGNALED(status)) {
            syslog(LOG_INFO, "process signalled. pid: %ld status: %d", (long) pid, WTERMSIG(status));
        } else {
            syslog(LOG_ERR, "error: unexpected status for pid: %ld", (long) pid);
            exit(EXIT_FAILURE);
        }

        /* If the PID is an active PID, then we should respawn it. */
        int server;
        if ((server = find_server(pid)) >= 0) {
            stat_restart_node_unexpected_count += 1;
            store_time(stat_restart_last_node_unexpected_time);
            syslog(LOG_ERR, "server %d (pid %d) terminated unexpectedly by signal", server, pid);
            if (respawn) {
                syslog(LOG_ERR, "server %d (pid %d) respawning", server, pid);
                spawn_server(server);
            } else {
                /* Child died, but we've been asked not to respawn. Remove the pid from servers. */
                servers[server] = NO_PID;
            }
        } else {
            /* Backlog process exited, find it and clear it. */
            clear_backlog_server(pid);
        }
    }

    return EXIT_FAILURE;
}

/* SIGUSR1 migrates all servers (zero-downtime restart). */
static void
sigusr1_handler(int signum, siginfo_t *siginfo, void *context)
{
    pid_t from_pid = getpid();
    if (from_pid != niagra_pid) {
        syslog(LOG_ERR, "SIGUSR1: from pid %d, not niagra, doing nothing", from_pid);
        return;
    }

    syslog(LOG_INFO, "SIGUSR1: migrating all servers");
    migrate_servers();
}

/* SIGINT restarts all servers (possible-downtime restart). */
static void
sigint_handler(int signum, siginfo_t *siginfo, void *context)
{
    pid_t from_pid = getpid();
    if (from_pid != niagra_pid) {
        syslog(LOG_ERR, "SIGINT: from pid %d, not niagra, doing nothing", from_pid);
        return;
    }

    struct timeval new_time;
    (void) gettimeofday(&new_time, NULL);
    if (new_time.tv_sec == last_sigint_time.tv_sec) {
        syslog(LOG_INFO, "SIGINT(fast): terminate all servers & exit");
        terminate_servers();
        exit(EXIT_SUCCESS);
    }
    last_sigint_time = new_time;

    syslog(LOG_INFO, "SIGINT: restarting (not migrate) all servers");
    restart_servers();
}

/* SIGTERM terminates all servers and exits (downtime!). */
static void
sigterm_handler(int signum, siginfo_t *siginfo, void *context)
{
    pid_t from_pid = getpid();
    if (from_pid != niagra_pid) {
        syslog(LOG_ERR, "SIGTERM: from pid %d, not niagra, doing nothing", from_pid);
        return;
    }

    syslog(LOG_INFO, "SIGTERM: terminate all servers & exit");
    terminate_servers();
    exit(EXIT_FAILURE);
}

/* SIGUSR2 outputs state. */
static void
sigusr2_handler(int signum, siginfo_t *siginfo, void *context)
{
    pid_t from_pid = getpid();
    if (from_pid != niagra_pid) {
        syslog(LOG_ERR, "SIGUSR2: from pid %d, not niagra, doing nothing", from_pid);
        return;
    }

    syslog(LOG_INFO, "SIGUSR2: outputting state");
    output_state(siginfo->si_pid);
}

static void
install_signal_handlers(void)
{
    int r;
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* Restart functions if interrupted by handler */

    sa.sa_sigaction = sigusr1_handler;
    r = sigaction(SIGUSR1, &sa, NULL);
    if (r == -1) {
        syslog(LOG_ERR, "error installing handler: %m");
        exit(EXIT_FAILURE);
    }

    sa.sa_sigaction = sigterm_handler;
    r = sigaction(SIGTERM, &sa, NULL);
    if (r == -1) {
        syslog(LOG_ERR, "error installing handler: %m");
        exit(EXIT_FAILURE);
    }

    sa.sa_sigaction = sigusr2_handler;
    r = sigaction(SIGUSR2, &sa, NULL);
    if (r == -1) {
        syslog(LOG_ERR, "error installing handler: %m");
        exit(EXIT_FAILURE);
    }

    if (debug_mode) {
        sa.sa_sigaction = sigint_handler;
        r = sigaction(SIGINT, &sa, NULL);

        if (r == -1) {
            syslog(LOG_ERR, "error installing handler: %m");
            exit(EXIT_FAILURE);
        }
    }
}

static void
parse_config_file(void)
{
    FILE *f;
    int i, n, r;

    static char line[MAX_LINE_SIZE];
    char *command_value[2];
    char *socket_parts[NUM_SOCK_OPTIONS];
    char *file_parts[NUM_FILE_OPTIONS];

    f = fopen(config_file_name, "r");

    if (f == NULL) {
        syslog(LOG_ERR, "opening config file: %m");
        exit(EXIT_FAILURE);
    }

    i = 0;
    while (i++, (n = str_readline(f, line, sizeof line)) > 0) {

        if (line[0] == '#') {
            /* Comment line */
            continue;
        }

        r = str_split(line, ':', command_value, 2);

        if (r < 2) {
            /* Must be a command portion */
            n = -1;
            break;
        }

        command_value[1] = str_strip(command_value[1], ' ');

        if (strcmp(command_value[0], "command") == 0) {
            if (!str_isempty(server_command)) {
                syslog(LOG_INFO, "command already set.");
                n = -1;
                break;
            }

            r = str_copy(server_command, command_value[1], sizeof server_command);

            if (r == -1) {
                syslog(LOG_INFO, "command too long.");
                n = -1;
                break;
            }

        } else if (strcmp(command_value[0], "file") == 0) {
            struct file *file;

            r = str_split(command_value[1], ' ', file_parts, NUM_FILE_OPTIONS);
            if (r != NUM_FILE_OPTIONS) {
                syslog(LOG_INFO, "Incorrect number of fields (%d) for file options. Should be %d fields.",
                       r, NUM_FILE_OPTIONS);
                n = -1;
                break;
            }

            if (num_files >= MAX_FILES) {
                syslog(LOG_INFO, "Too many files defined. A maximum of %d is allowed", MAX_FILES);
                n = -1;
                break;
            }

            file = &files[num_files];

            if (lookup_file_by_key(file_parts[0]) != -1) {
                syslog(LOG_INFO, "duplicate file key: %s", file_parts[0]);
                n = -1;
                break;
            }

            r = str_copy(file->key, file_parts[0], sizeof file->key);
            if (r == -1) {
                syslog(LOG_INFO, "file key too long");
                n = -1;
                break;
            }

            r = str_copy(file->name, file_parts[1], sizeof file->name);
            if (r == -1) {
                syslog(LOG_INFO, "file name too long");
                n = -1;
                break;
            }

            num_files++;

        } else if (strcmp(command_value[0], "socket") == 0) {
            struct fd *fd;

            r = str_split(command_value[1], ' ', socket_parts, NUM_SOCK_OPTIONS);
            if (r != NUM_SOCK_OPTIONS) {
                syslog(LOG_INFO, "Incorrect number of fields (%d) for socket options. Should be %d fields.",
                       r, NUM_SOCK_OPTIONS);
                n = -1;
                break;
            }

            if (num_fds >= MAX_FDS) {
                syslog(LOG_INFO, "Too many fds defined. A maximum of %d is allowed", MAX_FDS);
                n = -1;
                break;
            }

            fd = &fds[num_fds];

            if (lookup_fd_by_name(socket_parts[0]) != -1) {
                syslog(LOG_INFO, "duplicate fd name: %s", socket_parts[0]);
                n = -1;
                break;
            }

            r = str_copy(fd->name, socket_parts[0], sizeof fd->name);
            if (r == -1) {
                syslog(LOG_INFO, "socket name too long");
                n = -1;
                break;
            }

            r = str_copy(fd->type, socket_parts[1], sizeof fd->type);
            if (r == -1) {
                syslog(LOG_INFO, "socket type too long");
                n = -1;
                break;
            }

            if (strcmp(socket_parts[2], "4") == 0) {
                fd->x.sock.ip_ver = 4;
            } else if (strcmp(socket_parts[2], "6") == 0) {
                fd->x.sock.ip_ver = 6;
            } else {
                syslog(LOG_INFO, "IP version must be '4' or '6'");
                n = -1;
                break;
            }

            r = inet_aton(socket_parts[3], &fd->x.sock.addr);
            if (r == 0) {
                syslog(LOG_INFO, "invalid network address");
                n = -1;
                break;
            }

            r = str_uint16(socket_parts[4], &fd->x.sock.port);
            if (r == -1) {
                syslog(LOG_INFO, "invalid port number");
                n = -1;
                break;
            }

            r = str_int(socket_parts[5], &fd->x.sock.backlog);
            if (r == -1) {
                syslog(LOG_INFO, "invalid backlog");
                n = -1;
                break;
            }

            fd->fd_type = SOCKET_FD;

            num_fds++;

        } else if (strcmp(command_value[0], "user") == 0) {
            syslog(LOG_INFO, "WARNING: Got user command: %s - not implemented", command_value[1]);

        } else if (strcmp(command_value[0], "environment") == 0) {

            if (!str_isempty(config_environment)) {
                syslog(LOG_INFO, "environment already set.");
                n = -1;
                break;
            }

            r = str_copy(config_environment, command_value[1], sizeof config_environment);

            if (r == -1) {
                syslog(LOG_INFO, "environment too long.");
                n = -1;
                break;
            }

        } else if (strcmp(command_value[0], "copies") == 0) {
            int c = atoi(command_value[1]);
            if (c <= 0) {
                syslog(LOG_INFO, "WARNING: Got invalid copies command: '%s', defaulting to 1",
                       command_value[1]);
            } else if (c > MAX_COPIES) {
                syslog(LOG_INFO, "WARNING: copies command %d exceeds maximum of %d: defaulting to %d", c,
                       MAX_COPIES, MAX_COPIES);
                copies = MAX_COPIES;
            } else {
                copies = c;
            }

        } else if (strncmp(command_value[0], "app-", 4) == 0) {
            struct app_option *app_option;

            if (num_app_options >= MAX_APP_OPTIONS) {
                syslog(LOG_INFO, "Too many app options defined. A maximum of %d is allowed", MAX_APP_OPTIONS);
                n = -1;
                break;
            }

            app_option = &app_options[num_app_options];

            r = str_copy(app_option->name, command_value[0], sizeof app_option->name);
            if (r == -1) {
                syslog(LOG_INFO, "app option name too long");
                n = -1;
                break;
            }

            r = str_copy(app_option->value, command_value[1], sizeof app_option->value);
            if (r == -1) {
                syslog(LOG_INFO, "app option value too long");
                n = -1;
                break;
            }

            num_app_options++;

        } else {
            /* Invalid command */
            syslog(LOG_INFO, "Invalid command: '%s'", command_value[0]);
            n = -1;
            break;
        }
    }

    if (n != 0) {
        syslog(LOG_INFO, "Error on line: %d", i);
        exit(EXIT_FAILURE);
    }

    /* if there is an error on close, we don't care */
    (void) fclose(f);
}

static void
create_sockets(void) {
    int i;
    for (i = 0; i < num_fds; i++) {
        struct fd *fd = &fds[i];
        if (fd->fd_type != SOCKET_FD) {
            continue;
        }
        fd->fd = create_socket(fd->x.sock.addr, fd->x.sock.port, fd->x.sock.backlog);
    }
}

static int
create_socket(struct in_addr addr, uint16_t port, int backlog)
{
    int s;
    int r;
    struct sockaddr_in sockaddr;
    const int flags = 1;

    /* create, bind and listen on the socket */
    s = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (s == -1) {
        /* FIXME: look at errno and provide better error handling */
        syslog(LOG_ERR, "error creating socket: %m");
        exit(EXIT_FAILURE);
    }

    /* Set up socket os that it is a reusable address */
    r  = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const void *)&flags, sizeof flags);
    if (r != 0) {
        syslog(LOG_ERR, "error setting re-use addr option: %m");
        exit(EXIT_FAILURE);
    }

    /* Ensure the socket is non-blocking like node expects */
    r = fcntl(s, F_SETFL, O_NONBLOCK);
    if (r == -1) {
        syslog(LOG_ERR, "error setting non-blocking: %m");
        exit(EXIT_FAILURE);
    }

    /* Set up the socket address structure */
    memset(&sockaddr, 0, sizeof sockaddr);

    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons(port);
    sockaddr.sin_addr = addr;

    r = bind(s, (struct sockaddr *) &sockaddr, sizeof sockaddr);

    if (r != 0) {
        syslog(LOG_ERR, "error binding socket: %m");
        exit(EXIT_FAILURE);
    }

    r = listen(s, backlog);

    if (r != 0) {
        syslog(LOG_ERR, "error listening on socket: %m");
        exit(EXIT_FAILURE);
    }

    return s;
}

static int
lookup_fd_by_name(const char *name)
{
    int i;
    for (i = 0; i < num_fds; i++) {
        struct fd *fd = &fds[i];
        if (strcmp(name, fd->name) == 0) {
            break;
        }
    }

    if (i == num_fds) {
        i = -1;
    }

    return i;
}

static void
open_files(void)
{
    int i;
    for (i = 0; i < num_files; i++) {
        struct file *file = &files[i];
        file->fd = open(file->name, O_RDONLY);
        if (file->fd == -1) {
            syslog(LOG_ERR, "error opening file %s: %m", file->name);
            exit(EXIT_FAILURE);
        }
    }
}

static int
lookup_file_by_key(const char *key)
{
    int i;
    for (i = 0; i < num_files; i++) {
        struct file *file = &files[i];
        if (strcmp(key, file->key) == 0) {
            break;
        }
    }

    if (i == num_files) {
        i = -1;
    }

    return i;
}

static void
update_command_line(void)
{
    static char fd_arg[FD_ARG_LEN], env_arg[ENV_ARG_LEN], file_arg[FILE_ARG_LEN],
        app_option_arg[APP_OPTION_ARG_LEN];
    int i, r;

    for (i = 0; i < num_fds; i++) {
        struct fd *fd = &fds[i];

        r = snprintf(fd_arg, sizeof fd_arg, FD_PREFIX "%s,%s,%d", fd->name, fd->type, fd->fd);
        if (r >= (int)(sizeof fd_arg)) {
            syslog(LOG_INFO, "Unable to format fd argument (%d - %zd)", r, sizeof fd_arg);
            exit(EXIT_FAILURE);
        }

        r = str_concat(server_command, fd_arg, sizeof server_command);

        if (r == -1) {
            syslog(LOG_INFO, "server command buffer too small");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < num_files; i++) {
        struct file *file = &files[i];

        r = snprintf(file_arg, sizeof file_arg, FILE_PREFIX "%s,%d", file->key, file->fd);
        if (r >= (int)(sizeof file_arg)) {
            syslog(LOG_INFO, "Unable to format file argument (%d - %zd)", r, sizeof file_arg);
            exit(EXIT_FAILURE);
        }

        r = str_concat(server_command, file_arg, sizeof server_command);

        if (r == -1) {
            syslog(LOG_INFO, "server command buffer too small");
            exit(EXIT_FAILURE);
        }
    }

    for (i = 0; i < num_app_options; i++) {
        struct app_option *app_option = &app_options[i];

        r = snprintf(app_option_arg, sizeof app_option_arg, " --%s %s", app_option->name, app_option->value);
        if (r >= (int)(sizeof app_option_arg)) {
            syslog(LOG_INFO, "Unable to format app_option argument (%d - %zd)", r, sizeof app_option_arg);
            exit(EXIT_FAILURE);
        }

        r = str_concat(server_command, app_option_arg, sizeof server_command);

        if (r == -1) {
            syslog(LOG_INFO, "server command buffer too small");
            exit(EXIT_FAILURE);
        }
    }

    if (!str_isempty(config_environment)) {
        r = snprintf(env_arg, sizeof env_arg, ENV_PREFIX "%s", config_environment);
        if (r >= (int)(sizeof env_arg)) {
            syslog(LOG_INFO, "Unable to format env argument (%d - %zd)", r, sizeof env_arg);
            exit(EXIT_FAILURE);
        }

        r = str_concat(server_command, env_arg, sizeof server_command);

        if (r == -1) {
            syslog(LOG_INFO, "server command buffer too small");
            exit(EXIT_FAILURE);
        }
    }

}

static void
restart_servers(void)
{
    stat_restart_request_count += 1;
    stat_restart_node_expected_count += copies;
    store_time(stat_restart_last_node_expected_time);
    store_time(stat_restart_last_request_time);

    terminate_servers();
    spawn_servers();
}

/* Find the backlog server identified by pid and clear it.
   It is not an error if it can't be found: it's likely that it was a final backlogged
   server that was force-killed. */
static void
clear_backlog_server(pid_t pid)
{
    int i, j;
    for (j = 0; j < MAX_MIGRATE_BACKLOG; j++) {
        for (i = 0; i < copies; i++) {
            if (backlog_servers[j][i] == pid) {
                stat_backlog_node_count -= 1;
                store_time(stat_migrate_last_node_time);
                backlog_servers[j][i] = 0;
                break;
            }
        }
    }
}

static void
set_backlog_server(int server, pid_t pid)
{
    stat_backlog_node_count += 1;
    backlog_servers[0][server] = pid;
}

/* Terminate all final backlogged servers. */
static void
terminate_backlog_servers(int backlog_index)
{
    int i, r;
    pid_t pid;
    pid_t *dead_servers = backlog_servers[backlog_index];

    for (i = 0; i < copies; i++) {

        pid = dead_servers[i];

        if (pid != NO_PID) {
            stat_backlog_node_count -= 1;

            syslog(LOG_INFO, "old server %d (pid %d) at backlog position %d going down", i, pid,
                   backlog_index);

            r = kill(pid, SIGTERM);

            if (r != 0) {
                syslog(LOG_ERR, "couldn't kill old server %d (pid %d) at backlog position %d: %m", i,
                       pid, backlog_index);
            } else {
                syslog(LOG_INFO, "old server %d (pid %d) at backlog position %d successfully terminated",
                       i, pid, backlog_index);
            }
        }

        dead_servers[i] = NO_PID;
    }
}

/* Shift all remaining backlog servers. */
static void
shift_backlog_servers(void)
{
    int i, j;
    for (j = MAX_MIGRATE_BACKLOG - 1; j > 0; j--) {
        for (i = 0; i < copies; i++) {
            backlog_servers[j][i] = backlog_servers[j-1][i];
        }
    }
}

/* Send sigusr2 to old server identified by pid.
   Server handles sigusr2, finishes handling all existing connections, and closes. */
static void
migrate_server(int server, pid_t pid)
{
    int r;
    syslog(LOG_INFO, "migrating old server %d (pid %d)", server, pid);

    stat_migrate_node_count += 1;

    /* Add to front backlog. */
    set_backlog_server(server, pid);

    r = kill(pid, SIGUSR2);
    if (r != 0) {
        syslog(LOG_ERR, "couldn't migrate old server %d (pid %d): %m", server, pid);
    }
}

static void
migrate_servers(void)
{
    int i;
    pid_t pid;

    syslog(LOG_INFO, "migrating all servers");

    stat_migrate_request_count += 1;
    store_time(stat_migrate_last_request_time);

    /* Kill last backlog servers and shift all remaining backlog servers. */
    terminate_backlog_servers(MAX_MIGRATE_BACKLOG - 1);
    shift_backlog_servers();
 
    /* Spawn the new server and migrate the old server to front of backlog. */
    for (i = 0; i < copies; i++) {
        pid = servers[i];
        spawn_server(i);
        if (pid != NO_PID) {
            migrate_server(i, pid);
        }
    }

    syslog(LOG_INFO, "completed migrating all servers");
}

static void
terminate_server(int server)
{
    pid_t pid = servers[server];

    syslog(LOG_INFO, "server %d (pid %d) going down", server, pid);

    servers[server] = NO_PID;

    int r;
    r = kill(pid, SIGTERM);

    if (r != 0) {
        syslog(LOG_ERR, "couldn't kill server %d (pid %d): %m", server, pid);
    } else {
        syslog(LOG_INFO, "server %d (pid %d) successfully terminated", server, pid);
    }
}

static void
terminate_servers(void)
{
    syslog(LOG_INFO, "all servers going down");

    int i;
    for (i = 0; i < copies; i++) {
        if (servers[i] != NO_PID) {
            terminate_server(i);
        }
    }

    for (i = 0; i < MAX_MIGRATE_BACKLOG; i++) {
        terminate_backlog_servers(i);
    }

    syslog(LOG_INFO, "completed terminating all servers");
}

static void
spawn_server(int server)
{
    pid_t pid;

    /* if we spawn too quickly, just exit */
    struct timeval new_time;
    (void) gettimeofday(&new_time, NULL);
    if (fast_spawn_protect && new_time.tv_sec == last_spawn_time.tv_sec) {
        syslog(LOG_ERR, "Spawning too fast!");
        exit(EXIT_FAILURE);
    }
    last_spawn_time = new_time;

    pid = fork();

    if (pid == 0) {
        /* Child process */
        syslog(LOG_INFO, "spawning server %d with command: '%s'", server, server_command);
        (void) execl("/bin/bash", "/bin/bash", "-c", server_command, NULL);

        syslog(LOG_ERR, "execl: %m");
        exit(EXIT_FAILURE);
    } else {
        /* Parent process, cache child pid */
        syslog(LOG_INFO, "server %d (pid %d) spawned", server, pid);
        servers[server] = pid;
    }
}

static void
spawn_servers(void)
{
    int i;
    for (i = 0; i < copies; i++) {
        spawn_server(i);
    }
}

static int
find_server(pid_t pid) {
    int i, found = -1;
    for (i = 0; i < copies; i++) {
        if (servers[i] == pid) {
            found = i;
            break;
        }
    }
    return found;
}

static void
output_state(pid_t caller)
{
    FILE *state_file;
    char state_filename[MAX_FILE_NAME];
    int r, i, j;

    stat_state_request_count += 1;

    sprintf(state_filename, "%s/niagra-%d-%d.state", STATE_DIR, niagra_pid, caller);

    syslog(LOG_INFO, "state outputting to %s", state_filename);

    state_file = fopen(state_filename, "w");

    fprintf(state_file, "{\n");

    fprintf(state_file, "\"%s\": \"%d\",\n", "pid", niagra_pid);
    fprintf(state_file, "\"%s\": \"%s\",\n", "start_time", stat_start_time);
    fprintf(state_file, "\"%s\": \"%s\",\n", "mode", (debug_mode ? "debug" : "production"));
    fprintf(state_file, "\"%s\": \"%s\",\n", "respawn", (no_respawn ? "no" : "yes"));
    fprintf(state_file, "\"%s\": \"%s\",\n", "config", config_file_name);
    fprintf(state_file, "\"%s\": \"%s\",\n", "log", config_logfile);
    fprintf(state_file, "\"%s\": \"%d\",\n", "copies", copies);
    fprintf(state_file, "\"%s\": \"%s\",\n", "command", server_command);
    fprintf(state_file, "\"%s\": \"%s\",\n", "environment", config_environment);

    fprintf(state_file, "\"%s\": {\n", "sockets");
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "count", num_fds);
    fprintf(state_file, "\t\"%s\": [\n", "details");
    for (i = 0; i < num_fds; i++) {
        fprintf(state_file, "\t\t{\n");
        fprintf(state_file, "\t\t\"%s\": \"%s\",\n", "name", fds[i].name);
        fprintf(state_file, "\t\t\"%s\": \"%d\",\n", "ipver", fds[i].x.sock.ip_ver);
        fprintf(state_file, "\t\t\"%s\": \"%s\",\n", "addr", inet_ntoa(fds[i].x.sock.addr));
        fprintf(state_file, "\t\t\"%s\": \"%i\",\n", "port", fds[i].x.sock.port);
        fprintf(state_file, "\t\t\"%s\": \"%i\",\n", "backlog", fds[i].x.sock.backlog);
        fprintf(state_file, "\t\t},\n");
    }
    fprintf(state_file, "\t]\n");
    fprintf(state_file, "},\n");

    fprintf(state_file, "\"%s\": {\n", "nodes");
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "count", copies);
    fprintf(state_file, "\t\"%s\": [", "pids");
    for (i = 0, r = 0; i < copies; i++) {
        if (servers[i] != NO_PID) {
            if (r == 1) {
                fprintf(state_file, ", ");
            }
            r = 1;
            fprintf(state_file, "%d", servers[i]);
        }
    }
    fprintf(state_file, "],\n");
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "backlog_count", stat_backlog_node_count);
    fprintf(state_file, "\t\"%s\": [", "backlog_pids");
    for (i = 0, r = 0; i < MAX_MIGRATE_BACKLOG; i++) {
        for (j = 0; j < copies; j++) {
            if (backlog_servers[i][j] != NO_PID) {
                if (r == 1) {
                    fprintf(state_file, ", ");
                }
                r = 1;
                fprintf(state_file, "%d", backlog_servers[i][j]);
            }
        }
    }
    fprintf(state_file, "]\n");
    fprintf(state_file, "},\n");


    fprintf(state_file, "\"%s\": {\n", "migrations");
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "requests", stat_migrate_request_count);
    fprintf(state_file, "\t\"%s\": \"%s\",\n", "last_request_time", stat_migrate_last_request_time);
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "nodes_requested", stat_migrate_node_count);
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "nodes_completed", stat_migrate_node_count - stat_backlog_node_count);
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "nodes_uncompleted", stat_backlog_node_count);
    fprintf(state_file, "\t\"%s\": \"%s\"\n", "last_node_time", stat_migrate_last_node_time);
    fprintf(state_file, "},\n");


    fprintf(state_file, "\"%s\": {\n", "restarts");
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "requests", stat_restart_request_count);
    fprintf(state_file, "\t\"%s\": \"%s\",\n", "last_request_time", stat_restart_last_request_time);
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "nodes_expected", stat_restart_node_expected_count);
    fprintf(state_file, "\t\"%s\": \"%s\",\n", "last_node_expected_time", stat_restart_last_node_expected_time);
    fprintf(state_file, "\t\"%s\": \"%d\",\n", "nodes_unexpected", stat_restart_node_unexpected_count);
    fprintf(state_file, "\t\"%s\": \"%s\"\n", "last_node_unexpected_time", stat_restart_last_node_unexpected_time);
    fprintf(state_file, "},\n");


    fprintf(state_file, "}\n");

    fclose(state_file);

    syslog(LOG_INFO, "state outputted to %s, signalling caller %d", state_filename, caller);

    r = kill(caller, SIGUSR2);

    if (r != 0) {
        syslog(LOG_ERR, "couldn't signal output state caller %d: %m", caller);
    }

}

#if defined(DEBUG)
static void
fprint_fd_socket(FILE *f, struct fd *fd)
{
    fprintf(f, "   %s/%d: IPv%d %s:%d backlog %d\n",
            fd->name, fd->fd,
            fd->x.sock.ip_ver, inet_ntoa(fd->x.sock.addr),
            fd->x.sock.port, fd->x.sock.backlog);
}
#endif
