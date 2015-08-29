#!/usr/bin/perl
# small IP server that takes some input and returns a message
# see http://xmodulo.com/how-to-write-simple-tcp-server-and-client-in-perl.html
use IO::Socket::INET;
use FileHandle;
use Getopt::Std;
use strict;
my %opt;
getopts('l:p:',\%opt);
my $port = $opt{p} || '7777';
die "invalid port $port" unless $port =~ /^\d{0,5}$/;
my $log = $opt{l} || "/usr/local/apache2.4/testers/evenport-$port.log";
system "touch $log";
die "invalid log $log" unless -f $log;

# auto-flush on socket
$| = 1;

open LOG, ">> $log" or die "can't open $log for writing: $!";
LOG->autoflush(1);

# creating a listening socket
my $socket = new IO::Socket::INET (
    LocalHost => '0.0.0.0',
    LocalPort => $port,
    Proto => 'tcp',
    Listen => 5,
    Reuse => 1
);
print LOG "cannot create socket $!\n" and exit 1 unless $socket;
print LOG "server waiting for client connection on port $port\n";

while(1)
{
    # waiting for a new client connection
    my $client_socket = $socket->accept();

    # get information about a newly connected client
    my $client_address = $client_socket->peerhost();
    my $client_port = $client_socket->peerport();
    print LOG "connection from $client_address:$client_port\n";

    # read up to 1024 characters from the connected client
    my $data = "";
    $client_socket->recv($data, 4096);
    print LOG "received data: $data\n";

    # write response data to the connected client
    my $iseven = ($port % 2 ? "ODD": "EVEN");
    $data = "$iseven\n";
    $client_socket->send($data);

    # notify client that response has been sent
    shutdown($client_socket, 1);
}

print LOG "closing\n";
$socket->close();

