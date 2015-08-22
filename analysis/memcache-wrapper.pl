#!/usr/bin/perl
# simple process wrapper to the memcached script
# avoids having to do ip from mod_ml
use Getopt::Std;
use FileHandle;
use strict;
STDOUT->autoflush(1);
my %opt;
getopts("h:p:",\%opt);
my $host = $opt{h} || 'cloudsmall8.cs.surrey.sfu.ca';
my $port = $opt{p} || 39999;
while (<>) {
    chomp;
    my $response = `echo $_ | /bin/netcat $host $port`;
    $response =~ s/^\s*|\s*$//g;
    print "$response\n";
}
