var express = require("express")
  , http = require("http")
  , https = require("https")

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

/* TODO: refactor this. */

function InsecureServer(name, fd) {
    this.type = "insecure"
    this.name = name
    this.fd = fd

    this.start = function(app, f) {
        var that = this;
        this.server = http.createServer(app)
        this.server.on("close", function() { return close(that) })
        this.server.on("error", function() { return error(that) })
        process.on("SIGUSR2", function() { return sigusr2(that) })
        return this.server.listen( { fd: this.fd }, f)
    }
}

function SecureServer(name, fd, key, cert) {
    this.type = "secure"
    this.name = name
    this.fd = fd
    this.key = key
    this.cert = cert

    this.start = function(app, f) {
        var that = this;
        this.server = https.createServer( { key: this.key, cert: this.cert }, app)
        this.server.on("close", function() { return close(that) })
        this.server.on("error", function() { return error(that) })
        process.on("SIGUSR2", function() { return sigusr2(that) } )
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

function createServers() {

    var niagra = {
        servers: [],
        environment: null
    }

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
            var fdname = parts[0], fdtype = parts[1], fd = parseInt(parts[2]), server
            
            switch (fdtype) {
            case "insecure": 
                server = new InsecureServer(fdname, fd)
                break
            case "secure":
                server = new SecureServer(fdname, fd, null, null)
                break
            }

            niagra.servers.push(server)
            niagra[server.name] = server
            break
        }
        }
    }

    if (niagra.environment && !process.env.NODE_ENV) {
        process.env.NODE_ENV = niagra.environment
    }

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
