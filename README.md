# node-launcherd

node-launcherd is there to launch, and relaunch, your node applications so that you can have *zero downtime* when doing upgrading your server.

## Zero Downtime

What exactly is *zero downtime*? Well, naively, when you upgrade your server you would need to shutdown any running server so that you can open the server port in the new version of the server. If you do this then you either have to wait until all the existing requests are fulfilled (which may take a long time if someone is transferring a large request or response), or you have to break the existing HTTP connection, which isn't going to look great either. A *zero downtime* approach means that the existing server can continue running until it has finished fulfilling all of the existing requests it is servicing while the new server will handle any new requests as they come in. Eventually the old server will have finished servicing all of the outstanding requests and then it can shut down cleanly, without causing any noticable problem for clients that might be connected to it.

## Design

node-launcherd is a small C program which enables *zero downtime* servers. When it starts it will bind to any configured server ports. It will then `fork` and `exec` the actual server, passing the open socket as a file descriptor.

When instructed (by a signal), node-launcherd will shutdown any existing servers it has spawned, and respawn new servers, passing them the existing open socket.

node-launcherd isn't magic and relies on the servers it is spawning to cooperate. Firstly, these servers need to handle receiving a socket as an open file-descriptor, rather than creating and binding and listening to the socket themselves.

Secondly, the server must play nice when node-launcherd instructs it to shutdown. When the server receives a shutdown signal from node-launcherd, it should stop accepting new connection on any sockets that node-launcherd passed to it.

The timing isn't critical, if it doesn't stop accepting new connections the old server will simply race with the new server to accept any new connections coming in. If an old-server is misbehaving it is always possible to manually destroy it with `kill`; this will affect any existing connections, but new connections on the new server will be unaffected.

## Other features

In addition to providing the zero downtime functionality, node-launcherd provides a few other useful things. node-launcherd, when run with root privileges, can obtain resources (such as privileged ports, and secret key files), and then drop privileges before launching the server itself.

node-launcherd is also able to spawn multiple copies of the server when necessary. This might be useful when running servers on a multi-core machine. With this approach each spawned process will intentionally race on `accept`. The underlying operating system kernel will pick the winner of the race. Your mileage may vary depending on your kernel as to how scalable this approach is.

node-launcherd also monitors the running process and is able to respawn a server process if it terminates unexpectedly. 

## Usage

node-launcherd is designed to run indefinitely. You system's daemon management tool (e.g: launchd, init, etc), should be used to manage node-launcherd's life-cycle.

    $ node-launcherd [ -d ] config

You normally want to run node-launcherd with root privileges.

If `-d` is passed, then node-launcherd will run in debug mode, and will not daemonize. In this case servers that have been started by
node-launcherd will be able to print to standard-output and standard-error.

The config file is a simple plain text format. There must be exactly one `command` line. `user` and `copies` are optional, with a maximum of one. There should be one or more `socket` lines. (Strictly speaking none are needed, however these somewhat defeats the purpose!). Zero or more `file` lines are allowed.

command: command-to-run
user: username
copies: n
socket: name [4|6] ip_addr port backlog
file: name /path/ flags

node-launcherd listens for SIGUSR1 signals. When it receives SIGUSR1 it will start the reload logic. This will restart all the servers with the zero downtime logic as described above. If node-launcherd receives
a SIGTERM it will gracefully attempt to shutdown servers.

## Server interface

Servers spawned by node-launcherd must be ready to follow the interface provided.

This is quite simple. On server launch, additional command line arguments will be provided in the form `--fd <name>,<fd>`. Each of these signifies an open file descriptor passed to the server which can then be used.

Each socket file descriptor is set as non-blocking, so in node.js can be directly passed to listenFD.

The server must register to handle SIGUSR1 signals. Once this signal is received the server should not `accept` any more connections on provided sockets.

## TODO

Not everything documented is currently actually implemented. The following is not implemented:

 * 'file' option
 * IPv6 sockets
 * dropping privileges
 * multiple server instances
