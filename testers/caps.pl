#!/usr/bin/perl
# take input and output it in CAPS
# http://perl.plover.com/FAQs/Buffering.html
use FileHandle;
use sigtrap handler => \&ending, 'normal-signals';

open LOG, ">> /etc/apache2/bin/caps.log" or die "can't open log: $!";
LOG->autoflush(1);
STDOUT->autoflush(1);
print LOG scalar(localtime)," $0 started\n";

while (<>) {
    chomp;
    print "\U$_\n";
    print LOG scalar(localtime)," got: $_ sent: \U$_\n";
}

sub ending {
    print LOG scalar(localtime)," $0 finished\n";
    exit;
}
