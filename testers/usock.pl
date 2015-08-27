#!/usr/bin/perl
# make a unix socket test server
# from http://www.perlmonks.org/?node_id=742842
use FileHandle;
use Getopt::Std;
use Socket;
use strict;
umask 0000;
$| = 1; # this was needed for their example to work

my %opt;
getopts("l:s:",\%opt);

my $log = $opt{l} || "/usr/local/apache2.4/testers/usock.log";
system "touch $log";
die "bad log file $log" unless -f $log;
open LOG, ">> $log" or die "can't open $log: $!";
LOG->autoflush(1);

my $socket_path = $opt{s} || "/tmp/usock.sock";
print LOG scalar(localtime)," open socket $socket_path\n";

my $sock_addr = sockaddr_un($socket_path);
socket(my $server, PF_UNIX,SOCK_STREAM,0) || die "socket: $!";
unlink($socket_path);
bind($server,$sock_addr) || die "bind: $!"; 
listen($server,SOMAXCONN) || die "listen: $!";

while (1) {
    accept(my $client,$server); 
    $_= <$client>;
    chomp;
    # print LOG scalar(localtime)," $_\n";
    print $client "SOCK ",scalar(localtime)," $_\n";
    # shutdown($client, 1);
}

print LOG scalar(localtime)," close socket $socket_path\n";
unlink($socket_path);

