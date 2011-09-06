/* Copyright: Ben Leslie 2011: See LICENSE file. */
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>

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
    printf("node-launcherd started\n");

    install_signal_handlers();
    parse_config_file();
    open_file();
    create_socket();
    spawn_server();

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
    /* fork, exec */
    execl("/bin/sh", "/bin/sh", "-c", server_command, NULL);
}
