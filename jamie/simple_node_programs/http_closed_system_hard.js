/*
  An HTTP 'closed system'.
  It defines a server and a client; the client opens an HTTP connection to the server.

  This simulates a more complex server side, in which the server has to do some 
    extra work to honor the client's request.
  There is also a background workload: timers, DNS queries, and FS operations.
*/

/* Modules. */
var http = require('http');
var dns = require('dns');
var fs = require('fs');

/* Globals. */
var port = 8000;
var n_clients = 10;
var client_count = 0;
var clients_handled = 0;
var runForever = (process.argv[2] && process.argv[2] == '--forever');

/* Helper functions. */

var mylog = function (str) {
  console.log('APP: ' + str);
};

/* Program code. */

/* Server listens. */
var server = http.createServer(function (request, response) {
  var client_id = client_count;
  client_count++;

  mylog('Server: handling client ' + client_id);

  fs.readdir("/lib", function respond(err, files){
    response.writeHead(200, {'Content-Type': 'text/plain'});
    response.write('Hello client ' + client_id + '!\n');
    response.write('In /tmp I found files: ' + files.toString() + '\n');
    response.end('Goodbye client ' + client_id + '!\n');
    mylog('Server: finished client ' + client_id);

    clients_handled += 1;
    mylog('clients_handled: ' + clients_handled);
    if (clients_handled == n_clients && !runForever)
    {
      mylog('Server closing'); 
      server.close(); 
    }
  });
}).listen(port, function (){ 
  mylog('Server listening'); 
});

/* Timers going off every .1 seconds for 1.5 seconds. */
var nums = new Array();
for (var i = 1; i <= 15; i++) {
  nums.push(i);
}
nums.forEach(function (num) {
  setTimeout(function(){ mylog("Timer: " + num*100/1000 + " second(s) has/ve passed"); }, num*100);
});

/* Async operations after .15 seconds
   Do this funnily enough enough that the clients have a chance to interweave. */

/* Some FS operations. */
setTimeout(function(){
  var files = ["/tmp/a", "/tmp/b", "/tmp/c", "/tmp/d"];
  files.forEach(function (file) {
    var fileLetter = file.charAt(file.length-1);
    var fileNum = fileLetter.charCodeAt() - "a".charCodeAt();
    var contents = fileLetter + fileLetter + fileLetter + fileLetter + fileLetter;
    fs.writeFile(file, contents, function() { 

       mylog("FS: Wrote " + contents + " to " + file); 
       var sleepLen = 100*fileNum;
       mylog("FS: sleepLen " + sleepLen);
       setTimeout(function(){
         fs.readFile(file, function(err, data) {
           mylog("FS: Read " + file + ": " + data); 
         });
       }, sleepLen);

    });
  });
}, 150);

/* DNS queries. */
setTimeout(function(){
  var sites = ['www.google.com', 'www.bbc.co.uk', 'www.gmail.com', 'www.cnn.com'];
  sites.forEach(function (site) {
    mylog("DNS: Looking up DNS for " + site);
    dns.lookup(site, function onLookup(err, addresses, family) {
      if (err) {
        mylog("DNS: lookup failed on " + site + ": " + err);
      }
      else {
        mylog("DNS: " + site + " -> " + addresses);
      }
    });
  });
}, 300);

/* Client speaks. */
var options = {
  protocol : 'http:', 
  host : 'localhost', 
  port : port,
  /* method : 'GET' */
  /* method : 'PUT' */
  method : 'POST'
};

var clientNums = new Array();
for (var i = 0; i < n_clients; i++) {
  clientNums.push(i);
}
clientNums.forEach(function (clientNum) {
  mylog('Client ' + clientNum + ': submitting request');
  http.request(options, function log_response (response) {
    mylog('Client ' + clientNum + ': I got response ' + response);
    response.on('data', function(chunk) { 
      mylog('Client ' + clientNum + ': Got ' + chunk.length + ' bytes of data: ' + chunk);
    });
    response.on('end', function() { 
      mylog('Client ' + clientNum + ': No more data is coming');
    });
  }).end();
});
