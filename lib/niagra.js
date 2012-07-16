var express = require("express")
  , http = require("http")
  , https = require("https")
  , fs = require("fs")
  , buffer = require("buffer")

exports = module.exports = createServers

var pid = process.pid

function sigusr2(s) {
    console.log('[' + pid + ', ' + s.name + ']', 'Got SIGUSR2, closing, current connection count '
                + s.server.connections)
    s.server.close()
    s.server.on("request", function(req, res) {
        req.connection.end()
    })
}

function close(s) {
    console.log('[' + pid + ', ' + s.name + ']', 'Closed all connections, exiting')
    process.exit()
}

function error(s) {
    console.log('[' + pid + ', ' + s.name + ']', 'Got error, exiting')
    process.exit()
}

function Server(type, name, fd) {
    this.type = type
    this.name = name
    this.fd = fd

    this.start = function(app, f) {
        var that = this;
        console.log('[' + pid + ', ' + this.name + ']', 'Starting')
        if (this.type === "secure") {
            if (!this.key || !this.cert) {
                throw new Error('secure sockets specified by no key and cert file available')
            }
            this.server = https.createServer( { key: this.key, cert: this.cert }, app)
        } else {
            this.server = http.createServer(app)
        }
        this.server.on("close", function() { return close(that) })
        this.server.on("error", function() { return error(that) })
        process.on("SIGUSR2", function() { return sigusr2(that) })
        return this.server.listen( { fd: this.fd }, f)
    }
}

function start(servers, test, app, f) {
    servers.forEach(function(server) {
        if (!test || test(server)) {
            server.start(app, f)
        }
    })
}

function parseArguments(niagra) {

    for (var i = 0; i < process.argv.length - 1; i++) {

        switch (process.argv[i]) {

        case "--env": {
            niagra.environment = process.argv[++i]
            break
        }

        case "--fd": {
            i++
            var parts = process.argv[i].split(',')
            if (parts.length != 3) {
                throw new Error('malformed --fd argument passed \'' + process.argv[i] + '\'')
            }
            var fdname = parts[0], fdtype = parts[1], fd = parseInt(parts[2])
            var server = new Server(fdtype, fdname, fd)
            if (fdtype === "secure") niagra.hasSecure = true
            niagra.servers.push(server)
            niagra[server.name] = server
            break
        }

        case "--file": {
            i++
            var parts = process.argv[i].split(',')
            if (parts.length != 2) {
                throw new Error('malformed --file argument passed \'' + process.argv[i] + '\'')
            }
            var file = {
                key: parts[0],
                fd: parseInt(parts[1])
            }
            niagra.files[file.key] = file
        }
        }
    }
}

function setEnvironment(niagra) {
    if (niagra.environment && !process.env.NODE_ENV) {
        process.env.NODE_ENV = niagra.environment
    }
}

function readSecureFiles(niagra) {
    if (niagra.hasSecure) {
        if (!niagra.files.key || !niagra.files.cert) {
            throw new Error('secure sockets specified by no key and cert file provided')
        }

        var bufferSize = 2048

        niagra.secureKey = new Buffer(bufferSize)
        fs.readSync(niagra.files.key.fd, niagra.secureKey, 0, bufferSize, 0)
        fs.closeSync(niagra.files.key.fd)

        niagra.secureCert = new Buffer(bufferSize)
        fs.readSync(niagra.files.cert.fd, niagra.secureCert, 0, bufferSize, 0)
        fs.closeSync(niagra.files.cert.fd)

        niagra.servers.forEach(function(server) {
            if (server.type == "secure") {
                server.key = niagra.secureKey
                server.cert = niagra.secureCert
            }
        })
    }
}

function createServers() {

    var niagra = {
        servers: [],
        files: {},
        environment: null,
        hasSecure: false,
        secureKey: null,
        secureCert: null,
    }

    parseArguments(niagra)

    setEnvironment(niagra)

    readSecureFiles(niagra)

    niagra.secure = {
        start: function(app, f) {
            start(niagra.servers, function(server) { return server.type == "secure" }, app, f)
        }
    }

    niagra.insecure = {
        start: function(app, f) {
            start(niagra.servers, function(server) { return server.type == "insecure" }, app, f)
        }
    }

    niagra.start = function(app, f) {
        start(niagra.servers, false, app, f)
    }

    return niagra
}
