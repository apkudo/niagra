var http = require('http');
var pid = process.pid

var server = http.createServer(helloWorld)

function sigusr2() {
    console.log('[' + pid + ']', 'Got SIGUSR2, connection count ' + server.connections)
    /* Stop accepting connections on this server, we will eventually
       exit when there is no more work to do */
    server.close()

    server.on("request", function(req, res) {
        req.connection.end()
    })
}

function server_close() {
    console.log('[' + pid + ']', 'Closed all connections, exiting')
    process.exit()
}

function server_error() {
    console.log('[' + pid + ']', 'Got error, exiting')
    process.exit()
}

function helloWorld(req, res) {
    console.log('[' + pid + ']', req.url, req.socket.fd)
    res.writeHead(200, {'Content-Type': 'text/plain'})
    res.end('Hello World, I am ' + pid + '\n')
}


server.on("close", server_close)
server.on("error", server_error)
process.on("SIGUSR2", sigusr2)

server.listen({ fd: 3 })

console.log('[' + pid + ']', 'Started')
