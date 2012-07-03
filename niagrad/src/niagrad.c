/* Copyright: Apkudo Inc. 2012: See LICENSE file. */

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
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>

#include "str.h"

#define OUTPUT_LOG "output.log"
#define SYSLOG_IDENT "niagra"
#define FD_PREFIX " --fd "
#define FD_PREFIX_SIZE (sizeof FD_PREFIX)
#define INT_STRING_LEN 10
#define FD_ARG_LEN (FD_PREFIX_SIZE + MAX_FD_NAME + INT_STRING_LEN)
#define INT_STRING_LEN 10
#define MAX_LINE_SIZE 4096
#define MAX_COMMAND_LINE 1024
#define MAX_FD_NAME 64
#define MAX_FDS 10
#define MAX_FILES 10
#define MAX_FILE_NAME 1024
#define NUM_SOCK_OPTIONS 5

/* Maximum number of node instances to spawn. One per core is probably good. */
#define MAX_COPIES 10

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
    int fd;
    enum fd_type fd_type;
    union {
        struct fd_socket sock;
        struct fd_file file;
    } x;
};

static void parse_config_file(void);
static void update_command_line(void);

static void create_sockets(void);
static int create_socket(struct in_addr addr, uint16_t port, int backlog);
static void install_signal_handlers(void);
static int lookup_fd_by_name(const char *name);

static int find_server(pid_t pid);
static void migrate_server(int server, pid_t pid);
static void migrate_servers(void);
static void spawn_server(int server);
static void spawn_servers(void);
static void terminate_servers(void);
static void terminate_server(int server);


#if defined(DEBUG)
static void fprint_fd_socket(FILE *f, struct fd *fd);
#endif

static pid_t niagra_pid;
static const char *config_file_name;
static char server_command[MAX_COMMAND_LINE];
static struct fd fds[MAX_FDS];
static int num_fds;
static int copies = 1;
static pid_t servers[MAX_COPIES];
static bool debug_mode = false;
static bool no_respawn = false;
static bool fast_spawn_protect = false;
static struct timeval last_sigint_time;
static struct timeval last_spawn_time;

static void
usage(void)
{
    printf("niagrad: [-d] config\n");
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

        fd = open(OUTPUT_LOG, O_WRONLY | O_CREAT | O_APPEND, 0666);
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
#if 0
        if (chdir("/") != 0) {
            syslog(LOG_ALERT, "unable to change directory: %m");
            exit(EXIT_FAILURE);
        }
#endif
    } else {
        exit(EXIT_SUCCESS);
    }
}

int
main(int argc, char **argv)
{
    pid_t pid;
    int status;
    int ch;
    int logopt = LOG_NDELAY;

    while ((ch = getopt(argc, argv, "d")) != -1) {
    switch (ch) {
    case 'd':
        debug_mode = true;
        break;
    case '?':
    default:
        usage();
    }
    }
    argc -= optind;
    argv += optind;

    if (argc != 1) {
    usage();
    }

    /* If we aren't in debug mode, we need to daemonize */
    if (!debug_mode) {
        daemonize();
    }

    config_file_name = argv[0];

    if (debug_mode) {
        logopt |= LOG_PERROR;
    }

    openlog(SYSLOG_IDENT, logopt, LOG_DAEMON);

    niagra_pid = getpid();
    syslog(LOG_INFO, "niagrad started: %ld", (long) niagra_pid);

    install_signal_handlers();
    parse_config_file();
    create_sockets();

    if (str_isempty(server_command)) {
        syslog(LOG_ERR, "no command specified.");
        exit(EXIT_FAILURE);
    }

    update_command_line();
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
                syslog(LOG_ERR, "process exited with shell error: pid: %ld status: %d", (long) pid, WEXITSTATUS(status));
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
            if (respawn) {
                syslog(LOG_ERR, "server %d (pid %d) terminated by signal, respawning", server, pid);
                spawn_server(server);
            } else {
                break;
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
sigusr1_handler(int signum)
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
sigint_handler(int signum)
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
    terminate_servers();
    spawn_servers();
}

/* SIGTERM terminates all servers and exits (downtime!). */
static void
sigterm_handler(int signum)
{
    pid_t from_pid = getpid();
    if (from_pid != niagra_pid) {
        syslog(LOG_ERR, "SIGTERM: from pid %d, not niagra, doing nothing", from_pid);
        return;
    }

    syslog(LOG_INFO, "SIGTERM: from niagra, terminate all servers & exit");
    terminate_servers();
    exit(EXIT_FAILURE);
}

static void
install_signal_handlers(void)
{
    int r;
    struct sigaction sa;

    sa.sa_handler = sigusr1_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* Restart functions if interrupted by handler */
    r = sigaction(SIGUSR1, &sa, NULL);

    if (r == -1) {
        syslog(LOG_ERR, "error installing handler: %m");
        exit(EXIT_FAILURE);
    }

    sa.sa_handler = sigterm_handler;
    r = sigaction(SIGTERM, &sa, NULL);

    if (r == -1) {
        syslog(LOG_ERR, "error installing handler: %m");
        exit(EXIT_FAILURE);
    }


    if (debug_mode) {
        sa.sa_handler = sigint_handler;
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
            /* Must be a command portition */
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
        } else if (strcmp(command_value[0], "socket") == 0) {
            struct fd *fd;

            r = str_split(command_value[1], ' ', socket_parts, NUM_SOCK_OPTIONS);
            if (r != NUM_SOCK_OPTIONS) {
                syslog(LOG_INFO, "Incorrect number of fields (%d) for socket options. Should be %d fields.", r, NUM_SOCK_OPTIONS);
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

            if (strcmp(socket_parts[1], "4") == 0) {
                fd->x.sock.ip_ver = 4;
            } else if (strcmp(socket_parts[1], "6") == 0) {
                fd->x.sock.ip_ver = 6;
            } else {
                syslog(LOG_INFO, "IP version must be '4' or '6'");
                n = -1;
                break;
            }

            r = inet_aton(socket_parts[2], &fd->x.sock.addr);
            if (r == 0) {
                syslog(LOG_INFO, "invalid network address");
                n = -1;
                break;
            }

            r = str_uint16(socket_parts[3], &fd->x.sock.port);
            if (r == -1) {
                syslog(LOG_INFO, "invalid port number");
                n = -1;
                break;
            }

            r = str_int(socket_parts[4], &fd->x.sock.backlog);
            if (r == -1) {
                syslog(LOG_INFO, "invalid backlog");
                n = -1;
                break;
            }

            fd->fd_type = SOCKET_FD;

            num_fds++;

        } else if (strcmp(command_value[0], "user") == 0) {
            syslog(LOG_INFO, "WARNING: Got user command: %s - not implemented", command_value[1]);
        } else if (strcmp(command_value[0], "copies") == 0) {
            int c = atoi(command_value[1]);
            if (c <= 0) {
                syslog(LOG_INFO, "WARNING: Got invalid copies command: '%s', defaulting to 1", command_value[1]);
            } else if (c > MAX_COPIES) {
                syslog(LOG_INFO, "WARNING: copies command %d exceeds maximum of %d: defaulting to %d", c, MAX_COPIES, MAX_COPIES);
                copies = MAX_COPIES;
            } else {
                copies = c;
            }

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

    (void) fclose(f); /* if there is an error on close, we don't care */
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
update_command_line(void)
{
    static char fd_arg[FD_ARG_LEN];
    int i, r;

    for (i = 0; i < num_fds; i++) {
        struct fd *fd = &fds[i];

        r = snprintf(fd_arg, sizeof fd_arg, FD_PREFIX "%s,%d", fd->name, fd->fd);
        if (r >= sizeof fd_arg) {
            syslog(LOG_INFO, "Unable to format fd argument (%d - %zd)", r, sizeof fd_arg);
            exit(EXIT_FAILURE);
        }

        r = str_concat(server_command, fd_arg, sizeof server_command);

        if (r == -1) {
            syslog(LOG_INFO, "server command buffer too small");
            exit(EXIT_FAILURE);
        }
    }
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
                backlog_servers[j][i] = 0;
                break;
            }
        }
    }
}

static void
set_backlog_server(int server, pid_t pid)
{
    backlog_servers[0][server] = pid;
}

/* Terminate all final backlogged servers. */
static void
terminate_last_backlog_servers(void)
{
    int i, r;
    pid_t pid;
    int last_backlog_position = MAX_MIGRATE_BACKLOG - 1;
    pid_t *dead_servers = backlog_servers[last_backlog_position];

    for (i = 0; i < copies; i++) {

        pid = dead_servers[i];

        if (pid > 0) {
            syslog(LOG_INFO, "very old server %d (pid %d) at backlog position %d going down", i, pid, last_backlog_position);

            r = kill(pid, SIGTERM);

            if (r != 0) {
                syslog(LOG_ERR, "couldn't kill very old server %d (pid %d) at backlog position %d: %m", i, pid, last_backlog_position);
            } else {
                syslog(LOG_INFO, "very old server %d (pid %d) at backlog position %d successfully terminated", i, pid, last_backlog_position);
            }

        }

        dead_servers[i] = 0;
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

    /* Kill last backlog servers and shift all remaining backlog servers. */
    terminate_last_backlog_servers();
    shift_backlog_servers();
 
    /* Spawn the new server and migrate the old server to front of backlog. */
    for (i = 0; i < copies; i++) {
        pid = servers[i];
        spawn_server(i);
        if (pid > 0) {
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

    servers[server] = 0;

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
        terminate_server(i);
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
        (void) execl("/bin/sh", "/bin/sh", "-c", server_command, NULL);

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
