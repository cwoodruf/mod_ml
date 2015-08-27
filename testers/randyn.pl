#!/usr/bin/perl
# take input and output it in CAPS
# http://perl.plover.com/FAQs/Buffering.html
use FileHandle;
use sigtrap handler => \&ending, 'normal-signals';

open LOG, ">> /usr/local/apache2.4/testers/randyn.log" or die "can't open log: $!";
LOG->autoflush(1);
STDOUT->autoflush(1);
print LOG scalar(localtime)," $0 started\n";

while (<>) {
    chomp;
    $sent = (rand > .5 ? 'YES': 'NO');
    print "$sent\n";
    print LOG scalar(localtime)," got: $_ sent: $sent\n";
}

sub ending {
    print LOG scalar(localtime)," $0 finished\n";
    exit;
}
