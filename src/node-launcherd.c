/* Copyright: Ben Leslie 2011: See LICENSE file. */

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
#include <sys/time.h>
#include <sys/wait.h>

#include "str.h"

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


static void create_sockets(void);
static int create_socket(struct in_addr addr, uint16_t port, int backlog);
static void notify_server(void);
static void terminate_server(void);
static void spawn_server(void);
static void parse_config_file(void);
static void open_file(void);
static void install_signal_handlers(void);
static void update_command_line(void);
static int lookup_fd_by_name(const char *name);

#if defined(DEBUG)
static void fprint_fd_socket(FILE *f, struct fd *fd);
#endif

static const char *config_file_name;
static char server_command[MAX_COMMAND_LINE];
static struct fd fds[MAX_FDS];
static int num_fds;
static pid_t current_server;
static bool debug_mode = false;
static bool fast_spawn_protect = false;
static struct timeval last_singint_time;
static struct timeval last_spawn_time;

static void
usage(void)
{
    printf("node-launcherd: [-d] config\n");
    exit(EXIT_FAILURE);
}

int
main(int argc, char **argv)
{
    pid_t pid;
    int status;
    int ch;

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

    config_file_name = argv[0];

    if (debug_mode) {
	fprintf(stderr, "node-launcherd started: %ld\n", (long) getpid());
    }

    install_signal_handlers();
    parse_config_file();
    open_file();
    create_sockets();

    if (str_isempty(server_command)) {
	fprintf(stderr, "No command specified.\n");
	abort();
    }

    update_command_line();
    spawn_server();

    for (;;) {
	bool respawn = true;
	pid = waitpid(-1, &status, 0);

	if (pid == -1) {
	    perror("error waiting for process");
	    abort();
	}

	if (WIFEXITED(status)) {
	    switch (WEXITSTATUS(status)) {
	    case 126:
	    case 127:
		/* we treat 126 and 127 as errors from the shell itself */
		respawn = false;
		break;
	    default:
		/*
		  printf("process exitted. PID: %ld STATUS: %d\n", (long) pid, WEXITSTATUS(status));
		*/
		break;
	    }
	} else if (WIFSIGNALED(status)) {
	    printf("process signalled. PID: %ld STATUS: %d\n", (long) pid, WTERMSIG(status));
	} else {
	    fprintf(stderr, "error: Unexecpted status for PID: %ld\n", (long) pid);
	    abort();
	}

	if (pid == current_server) {
	    /* If the PID is an active PID, then we should respawn it. */
	    if (respawn) {
		spawn_server();
	    } else {
		break;
	    }
	} else {
	    /* old process exitted, we don't really care */
	}
	/* Otherwise it is an old process and we can just let it exit */
    }

    return EXIT_SUCCESS;
}

static void
handler(int signum)
{
    fprintf(stderr, "SIGUSR1: respawn server\n");

    notify_server();
    spawn_server();
}

static void
sigint_handler(int signum)
{
    struct timeval new_time;
    (void) gettimeofday(&new_time, NULL);
    if (new_time.tv_sec == last_singint_time.tv_sec) {
	fprintf(stderr, "SIGINT(fast): terminate server & exit\n");
	terminate_server();
	exit(EXIT_FAILURE);
    }

    last_singint_time = new_time;

    fprintf(stderr, "SIGINT: restart server\n");
    terminate_server();
    spawn_server();
}

static void
sigterm_handler(int signum)
{
    fprintf(stderr, "SIGTERM: terminate server & exit\n");
    terminate_server();
    exit(EXIT_FAILURE);
}

static void
install_signal_handlers(void)
{
    int r;
    struct sigaction sa;

    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART; /* Restart functions if interrupted by handler */
    r = sigaction(SIGUSR1, &sa, NULL);

    if (r == -1) {
	perror("error installing handler");
	abort();
    }

    sa.sa_handler = sigterm_handler;
    r = sigaction(SIGTERM, &sa, NULL);

    if (r == -1) {
	perror("error installing handler");
	abort();
    }


    if (debug_mode) {
	sa.sa_handler = sigint_handler;
	r = sigaction(SIGINT, &sa, NULL);

	if (r == -1) {
	    perror("error installing handler");
	    abort();
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
	perror("opening config file");
	abort();
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
		fprintf(stderr, "command already set.\n");
		n = -1;
		break;
	    }

	    r = str_copy(server_command, command_value[1], sizeof server_command);

	    if (r == -1) {
		fprintf(stderr, "command too long.\n");
		n = -1;
		break;
	    }
	} else if (strcmp(command_value[0], "socket") == 0) {
	    struct fd *fd;

	    r = str_split(command_value[1], ' ', socket_parts, NUM_SOCK_OPTIONS);
	    if (r != NUM_SOCK_OPTIONS) {
		fprintf(stderr, "Incorrect number of fields (%d) for socket options. Should be %d fields.\n", r, NUM_SOCK_OPTIONS);
		n = -1;
		break;
	    }

	    if (num_fds >= MAX_FDS) {
		fprintf(stderr, "Too many fds defined. A maximum of %d is allowed\n", MAX_FDS);
		n = -1;
		break;
	    }

	    fd = &fds[num_fds];

	    if (lookup_fd_by_name(socket_parts[0]) != -1) {
		fprintf(stderr, "duplicate fd name: %s\n", socket_parts[0]);
		n = -1;
		break;
	    }


	    r = str_copy(fd->name, socket_parts[0], sizeof fd->name);
	    if (r == -1) {
		fprintf(stderr, "socket name too long\n");
		n = -1;
		break;
	    }

	    if (strcmp(socket_parts[1], "4") == 0) {
		fd->x.sock.ip_ver = 4;
	    } else if (strcmp(socket_parts[1], "6") == 0) {
		fd->x.sock.ip_ver = 6;
	    } else {
		fprintf(stderr, "IP version must be '4' or '6'\n");
		n = -1;
		break;
	    }

	    r = inet_aton(socket_parts[2], &fd->x.sock.addr);
	    if (r == 0) {
		fprintf(stderr, "invalid network address\n");
		n = -1;
		break;
	    }

	    r = str_uint16(socket_parts[3], &fd->x.sock.port);
	    if (r == -1) {
		fprintf(stderr, "invalid port number\n");
		n = -1;
		break;
	    }

	    r = str_int(socket_parts[4], &fd->x.sock.backlog);
	    if (r == -1) {
		fprintf(stderr, "invalid backlog\n");
		n = -1;
		break;
	    }

	    fd->fd_type = SOCKET_FD;

	    num_fds++;

	} else if (strcmp(command_value[0], "user") == 0) {
	    printf("WARNING: Got user command: %s - not implemented\n", command_value[1]);
	} else if (strcmp(command_value[0], "copies") == 0) {
	    printf("WARNING: Got copies command: %s - not implemented\n", command_value[1]);
	} else {
	    /* Invalid command */
	    fprintf(stderr, "Invalid command: '%s'\n", command_value[0]);
	    n = -1;
	    break;
	}
    }

    if (n != 0) {
	fprintf(stderr, "Error on line: %d\n", i);
	abort();
    }

    (void) fclose(f); /* if there is an error on close, we don't care */
}

static void
open_file(void)
{

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
	perror("error creating socket");
	abort();
    }

    /* Set up socket os that it is a reusable address */
    r  = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const void *)&flags, sizeof flags);
    if (r != 0) {
	perror("error setting re-use addr option");
	abort();
    }

    /* Ensure the socket is non-blocking like node expects */
    r = fcntl(s, F_SETFL, O_NONBLOCK);
    if (r == -1) {
	perror("error setting non-blocking");
	abort();
    }

    /* Set up the socket address structure */
    memset(&sockaddr, 0, sizeof sockaddr);

    sockaddr.sin_family = AF_INET;
    sockaddr.sin_port = htons(port);
    sockaddr.sin_addr = addr;

    r = bind(s, (struct sockaddr *) &sockaddr, sizeof sockaddr);

    if (r != 0) {
	perror("error binding socket");
	abort();
    }

    r = listen(s, backlog);

    if (r != 0) {
	perror("error listening on socket");
	abort();
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
	    fprintf(stderr, "Unable to format fd argument (%d - %zd)\n", r, sizeof fd_arg);
	    abort();
	}

	r = str_concat(server_command, fd_arg, sizeof server_command);

	if (r == -1) {
	    fprintf(stderr, "server command buffer too small\n");
	    abort();
	}
    }
}

static void
notify_server(void)
{
    int r;

    r = kill(current_server, SIGUSR1);

    if (r != 0) {
	perror("couldn't kill existing server");
	abort();
    }
}

static void
terminate_server(void)
{
    int r;

    r = kill(current_server, SIGTERM);

    if (r != 0) {
	perror("couldn't kill existing server");
	abort();
    }
}

static void
spawn_server(void)
{
    pid_t pid;


    /* if we spawn too quickly, just exit */
    struct timeval new_time;
    (void) gettimeofday(&new_time, NULL);
    if (fast_spawn_protect && new_time.tv_sec == last_spawn_time.tv_sec) {
	fprintf(stderr, "Spawning too fast!\n");
	exit(EXIT_FAILURE);
    }
    last_spawn_time = new_time;


    pid = fork();

    if (pid == 0) {
	/* Child process */
	(void) execl("/bin/sh", "/bin/sh", "-c", server_command, NULL);

	perror("execl");
	abort();
    } else {
	/* Parent process */
	current_server = pid;
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

