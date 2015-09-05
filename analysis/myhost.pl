#!/usr/bin/perl
# small IP server that takes some input and returns a message
# see http://xmodulo.com/how-to-write-simple-tcp-server-and-client-in-perl.html
use IO::Socket::INET;
use FileHandle;
use Getopt::Std;
use strict;
my %opt;
getopts('l:p:',\%opt);
my $log = $opt{l} || "/srv/cal/src/apache/ml_logs/myhost.log";
system "touch $log";
die "invalid log $log" unless -f $log;
my $port = $opt{p} || '48888';
die "invalid port $port" unless $port =~ /^\d{0,5}$/;

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

# cloudsmall1-8 in order from /etc/hosts
my @hosts = qw/cs1 cs2 cs3 cs4 cs5 cs6 cs7 cs8/;
my $hcount = scalar @hosts;

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

    # do simple scheduling based on mod of IP
    if ($data =~ /\d{1,3}\.\d{1,3}\.\d{1,3}\.(\d{1,3})/) {
        my $host = $hosts[$1 % $hcount]; 
        $data = "$host\n";
        $client_socket->send($data);
    }

    # notify client that response has been sent
    shutdown($client_socket, 1);
}

print LOG "closing\n";
$socket->close();

