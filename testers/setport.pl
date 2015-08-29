#!/usr/bin/perl
# does a very simple binary port mapping - returned port will either be N, N+1 or N+2
# depending on the IP address received - only looks at the first IPv4 address
use FileHandle;
use sigtrap handler => \&ending, 'normal-signals';

open LOG, ">> /usr/local/apache2.4/testers/setport.log" or die "can't open log: $!";
LOG->autoflush(1);
STDOUT->autoflush(1);
print LOG scalar(localtime)," $0 started\n";

while (<>) {
    chomp;
    my $port = 55555;
    if (/\d{1,3}\.\d{1,3}\.\d{1,3}\.(\d{1,3})/) {
        $port = $port + ($1 % 2) + 1;
    }
    print "$port\n\n";
    print LOG scalar(localtime)," got: $_ sent: $port\n";
}

sub ending {
    print LOG scalar(localtime)," $0 finished\n";
    exit;
}
