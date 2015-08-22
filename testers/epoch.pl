#!/usr/bin/perl
# small IP server that takes some input and returns a message
# see http://xmodulo.com/how-to-write-simple-tcp-server-and-client-in-perl.html
use IO::Socket::INET;
use FileHandle;
use Getopt::Std;
use Time::Local;
use Digest::SHA qw/sha1_hex/;
use sigtrap qw/HUP/, handler => \&reload;
use Data::Dumper;
use strict;
my %opt;
getopts('w:l:p:',\%opt);

my $port = $opt{p} || '39993';
die "invalid port $port" unless $port =~ /^\d{0,5}$/;

my $log = $opt{l} || "/srv/cal/src/apache/ml/testers/epoch-$port.log";
if ($log eq '-') {
    open LOG, ">-" or die "can't open STDOUT: $!";
} else {
    system "touch $log";
    die "invalid log $log" unless -f $log;
    open LOG, ">> $log" or die "can't open $log for writing: $!";
}
LOG->autoflush(1);

my $whitelist = $opt{w} || "/srv/cal/src/apache/ml/testers/hosts";
my %whitelist;
&reload;

# auto-flush on socket
$| = 1;


# creating a listening socket
my $socket = new IO::Socket::INET (
    LocalHost => '0.0.0.0',
    LocalPort => $port,
    Proto => 'tcp',
    Listen => 5,
    Reuse => 1,
);
print LOG "cannot create socket $!\n" and exit 1 unless $socket;
print LOG "server waiting for client connection on port $port\n";
my %months = (
    Jan => 1,
    Feb => 2,
    Mar => 3,
    Apr => 4,
    May => 5,
    Jun => 6,
    Jul => 7,
    Aug => 8,
    Sep => 9,
    Oct => 10,
    Nov => 11,
    Dec => 12
);

my $client_socket;

while(1)
{
    # waiting for a new client connection
    $client_socket = $socket->accept();

    # get information about a newly connected client
    my $client_addr = $client_socket->peerhost();
    my $client_port = $client_socket->peerport();

    if (%whitelist) {
        print LOG "checking whitelist for $client_addr\n";
        unless ($whitelist{$client_addr}) {
            print LOG "rejecting connection from $client_addr:$client_port\n";
            shutdown($client_socket, 1);
            next;
        }
    } else {
        print LOG "no whitelist available, proceeding\n";
    }
    print LOG "connection from $client_addr:$client_port\n";

    # read up to 1024 characters from the connected client
    my $data = "";
    $client_socket->recv($data, 4096);
    print LOG "received data: $data\n";

    if (defined $whitelist{$client_addr}) {
        if ($whitelist{$client_addr} == 1) {
            print LOG "no pw needed for $client_addr\n";
        } elsif (defined $whitelist{$client_addr}{nonce}) {
            my $pw = $whitelist{$client_addr}{nonce};
            print LOG "checking nonce for $client_addr\n";
            my ($authtype, $nonce, $salt);
            if ($data =~ /(nonce)=(\w*):(\w*)/) {
                # this type of nonce takes an encoded password and reencodes it with the salt
                ($authtype, $nonce, $salt) = ($1,$2,$3);
                print LOG "found $authtype $nonce $salt\n";
            }
            if (sha1_hex($pw.$salt) ne $nonce) {
                print LOG "rejecting nonce from $client_addr\n";
                shutdown($client_socket, 1);
                next;
            }
        }
    }
    # write response data to the connected client
    # in this case its either the epoch seconds or the original string
    if (
        my ($day, $monstr, $year, $hour, $min, $sec) = 
            ($data =~ m#(\d+)/(\w+)/(\d\d\d\d):(\d+):(\d+):(\d+)#)
    ) {
        my $mon = $months{$monstr};
        my $yr = $year - 1900;
        my $m = $mon - 1;
        my $epoch = timelocal($sec, $min, $hour, $day, $m, $year);
        # $data = $epoch."\n\n";
        $data = $epoch;
        print LOG "sending data: $data\n";
    }
    $client_socket->send($data);

    # notify client that response has been sent
    shutdown($client_socket, 1);
}

print LOG "closing\n";
$socket->close();

sub reload {
    shutdown($client_socket, 1) if defined $client_socket;
    print LOG "try processing whitelist $whitelist\n";
    return unless -f $whitelist;
    print LOG "reloading hosts\n";
    open WL, $whitelist or die "can't open whitelist $whitelist: $!";
    while (<WL>) {
        chomp;
        my ($host,$data) = split /\s+/, $_;
        print LOG "found host $host data $data\n";
        if ($data =~ /(nonce)=(\S*|"[^"]*"|'[^']*')/) {
            my $authtype = $1;
            my $secret = $2;
            my $secretenc = sha1_hex($secret);
            print LOG "adding nonce secret $secretenc authtype $authtype\n";
            $secret =~ s/^['"]|['"]$//g;
            $whitelist{$host}{$authtype} = $secretenc;
        } else {
            $whitelist{$host} = 1;
        }
    }
    print LOG "whitelist:\n";
    print LOG Dumper(\%whitelist);
}

