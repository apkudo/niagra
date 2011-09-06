var http = require('http');
var pid = process.pid

function helloWorld(req, res) {
    console.log('[' + pid + ']', req.url, req.socket.fd)
    res.writeHead(200, {'Content-Type': 'text/plain'});
    res.end('Hello World I am ' + pid + '\n');
}

var server = http.createServer(helloWorld)
server.listen(1337, "127.0.0.1");

console.log('[' + pid + ']', 'Started')
