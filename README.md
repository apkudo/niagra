# niagra

niagra is there to launch, relaunch, and migrate your node applications so that you can have:
 * *multiple node instances* serving the same application (generally one per core).
 * *zero downtime* when upgrading.

## Zero Downtime

What exactly is *zero downtime*? Well, naively, when you upgrade your server you would need to shutdown any running server so that you can open the server port in the new version of the server. If you do this then you either have to wait until all the existing requests are fulfilled (which may take a long time if someone is transferring a large request or response), or you have to break the existing HTTP connection, which isn't going to look great either. A *zero downtime* approach means that the existing server can continue running until it has finished fulfilling all of the existing requests it is servicing while the new server will handle any new requests as they come in. Eventually the old server will have finished servicing all of the outstanding requests and then it can shut down cleanly, without causing any noticable problem for clients that might be connected to it.

## Design

*niagrad* is a small C program which enables *zero downtime* servers. When it starts it will bind to any configured server ports. It will then `fork` and `exec` the actual server, passing the open socket as a file descriptor.

When instructed (by a signal), niagrad will shutdown any existing servers it has spawned, and respawn new servers, passing them the existing open socket. This is known as a migration.

niagrad isn't magic and relies on the servers it is spawning to cooperate. Firstly, these servers need to handle receiving a socket as an open file-descriptor, rather than creating and binding and listening to the socket themselves.

Secondly, the server must play nice when niagrad instructs it to shutdown. When the server receives a shutdown signal from niagrad, it should stop accepting new connection on any sockets that niagra passed to it.

The timing isn't critical, if it doesn't stop accepting new connections the old server will simply race with the new server to accept any new connections coming in. If an old server is misbehaving it will be placed on a backlog list; if it does not gracefully die within 3 migration requests, it will be terminated. It is also always possible to manually destroy it with `kill`; this will affect any existing connections, but new connections on the new server will be unaffected.

A management utility, *niagra*, is used to interact with and manage niagrad.

## Other features

In addition to providing the zero downtime functionality, niagra provides a few other useful things. niagra, when run with root privileges, can obtain resources (such as privileged ports, and secret key files), and then drop privileges before launching the server itself.

niagra is also able to spawn multiple copies of the server when necessary. This might be useful when running servers on a multi-core machine. With this approach each spawned process will intentionally race on `accept`. The underlying operating system kernel will pick the winner of the race. Your mileage may vary depending on your kernel as to how scalable this approach is.

niagra also monitors the running process and is able to respawn a server process if it terminates unexpectedly.


## niagra Usage

*niagra* is a utility to manage niagrad instances. Usage is as follows:

    $ niagra [ command ] [ options ]

Commands:
 * *start [-d] config_file [log_file]*: Start niagra instance with config file and optional log file.
 * *list* | *ls*:  List running niagra instances.
 * *count*: Count of running niagra instances.
 * *migrate [pid]* | *mg [pid]*: Migrate a niagra instance. Zero-downtime restart of all nodes.
 * *restart [pid]*: Restart a niagra instance. Possible-downtime restart of all nodes.
 * *terminate [pid]*: Terminate a niagra instance. Full-downtime kill of all nodes.
 * *state [pid]* | *st [pid]*: Output state of existing niagra instance.

Options:
 * *-d*: Debug mode. niagra instance will not be daemonized.
 * *pid*: pid of niagra instance. Command applies to all instances if not provided.


## niagrad Usage

niagra is designed to run indefinitely. You system's daemon management tool (e.g: launchd, init, etc), should be used to manage niagra's life-cycle.

    $ niagrad [ -d ] config [ logfile ]

You normally want to run niagrad with root privileges.

If `-d` is passed, then niagrad will run in debug mode, and will not daemonize. In this case servers that have been started by niagra will be able to print to standard-output and standard-error.

The config file is a simple plain text format. There must be exactly one `command` line. `user` and `copies` are optional, with a maximum of one. There should be one or more `socket` lines. (Strictly speaking none are needed, however this somewhat defeats the purpose!). Zero or more `file` lines are allowed.

    command: command-to-run
    user: username
    copies: n
    socket: name [4|6] ip_addr port backlog
    file: name /path/ flags

All relative paths are relative to the location of the config file.

niagrad implements the following signal interface:

 * SIGUSR1: migrate all nodes (zero-downtime restart). This results in a SIGUSR2 to node instances, as described in the below server interface.
 * SIGINT: restart all nodes (possible-downtime restart)
 * SIGTERM: terminate all nodes (complete downtime)
 * SIGUSR2: write niagra state to file /tmp/niagra-{niagrad-pid}-{requester-pid}.state

## Server interface

Servers spawned by niagra must be ready to follow the interface provided.

This is quite simple. On server launch, additional command line arguments will be provided in the form `--fd <name>,<fd>`. Each of these signifies an open file descriptor passed to the server which can then be used.

Each socket file descriptor is set as non-blocking, so in node.js can be directly passed to listen({ fd: *fd* }).

The server must register to handle SIGUSR2 signals. Once this signal is received the server should not `accept` any more connections on provided sockets.

## TODO

Not everything documented is currently actually implemented. The following is not implemented:

 * 'file' option
 * IPv6 sockets
 * dropping privileges
 * express plugin
 * alerts via email
 * defaulting copies to the number cores
 * add version information to binary