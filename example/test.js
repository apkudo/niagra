var http = require('http');
var pid = process.pid

function sigusr1() {
    console.log('[' + pid + ']', 'Got SIGUSR1')
    /* Stop accepting connection on this server, we will eventually
       exit when there is no more work to do */
    server.watcher.stop()
}

function helloWorld(req, res) {
    console.log('[' + pid + ']', req.url, req.socket.fd)
    res.writeHead(200, {'Content-Type': 'text/plain'});
    res.end('Hello World I am ' + pid + '\n');
}

var server = http.createServer(helloWorld)

process.on('SIGUSR1', sigusr1)

server.listenFD(3)

console.log('[' + pid + ']', 'Started')
