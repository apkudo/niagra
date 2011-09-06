/* Copyright: Ben Leslie 2011: See LICENSE file. */

#include <sys/types.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>

#define MAX_COMMAND_LINE 1024
#define DEFAULT_ADDR INADDR_ANY
#define DEFAULT_PORT 1337
#define DEFAULT_BACKLOG 100

static void create_socket(void);
static void kill_server(void);
static void spawn_server(void);
static void parse_config_file(void);
static void open_file(void);
static void install_signal_handlers(void);

static const char server_command[MAX_COMMAND_LINE] = "node example/test.js";

static pid_t current_server;


int
main(int arc, char **argv)
{
    pid_t pid;
    int status;

    printf("node-launcherd started: %ld\n", (long) getpid());

    install_signal_handlers();
    parse_config_file();
    open_file();
    create_socket();
    spawn_server();

    for (;;) {
	pid = waitpid(-1, &status, 0);

	if (pid == -1) {
	    perror("error waiting for process");
	    abort();
	}

	if (WIFEXITED(status)) {
	    printf("process exitted. PID: %ld STATUS: %d\n", (long) pid, WEXITSTATUS(status));
	} else if (WIFSIGNALED(status)) {
	    printf("process signalled. PID: %ld STATUS: %d\n", (long) pid, WTERMSIG(status));
	} else {
	    fprintf(stderr, "error: Unexecpted status for PID: %ld\n", (long) pid);
	    abort();
	}

	if (pid == current_server) {
	    /* If the PID is an active PID, then we should respawn it. */
	    spawn_server();
	} else {
	    printf("Old process finally exitted. (%ld)\n", (long) pid);
	}
	/* Otherwise it is an old process and we can just let it exit */
    }

    return EXIT_SUCCESS;
}

static void
handler(int signum)
{
    printf("Got respawn signal\n");

    kill_server();
    spawn_server();
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
}

static void
parse_config_file(void)
{

}

static void
open_file(void)
{

}

static void
create_socket(void)
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
    sockaddr.sin_port = htons(DEFAULT_PORT);
    sockaddr.sin_addr.s_addr = DEFAULT_ADDR;

    r = bind(s, (struct sockaddr *) &sockaddr, sizeof sockaddr);

    if (r != 0) {
	perror("error binding socket");
	abort();
    }

    r = listen(s, DEFAULT_BACKLOG);

    if (r != 0) {
	perror("error listening on socket");
	abort();
    }

    printf("opened socket on fd: %d\n", s);
}

static void
kill_server(void)
{
    int r;

    r = kill(current_server, SIGUSR1);

    if (r != 0) {
	perror("couldn't kill existing server");
	abort();
    }
}

static void
spawn_server(void)
{
    pid_t pid;

    pid = fork();

    if (pid == 0) {
	/* Child process */
	(void) execl("/bin/sh", "/bin/sh", "-c", server_command, NULL);

	perror("execl");
	abort();
    } else {
	/* Parent process */
	printf("Spawned: %ld\n", (long) pid);

	current_server = pid;
    }
}
