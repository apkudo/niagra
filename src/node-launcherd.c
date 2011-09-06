/* Copyright: Ben Leslie 2011: See LICENSE file. */

#include <sys/types.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>

#define MAX_COMMAND_LINE 1024

static void create_socket(void);
static void spawn_server(void);
static void parse_config_file(void);
static void open_file(void);
static void install_signal_handlers(void);

static char server_command[MAX_COMMAND_LINE] = "node example/test.js";

int
main(int arc, char **argv)
{
    pid_t pid;
    int status;

    printf("node-launcherd started\n");

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

	/* If the PID is an active PID, then we should respawn it. */
	spawn_server();

	/* Otherwise it is an old process and we can just let it exit */
    }

    return EXIT_SUCCESS;
}

static void
install_signal_handlers(void)
{

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
    /* create, bind and listen on the socket */
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
    }
}
