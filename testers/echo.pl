#!/usr/bin/perl
# take input and output it in CAPS
# http://perl.plover.com/FAQs/Buffering.html
use FileHandle;
use sigtrap handler => \&ending, 'normal-signals';

open LOG, ">> /usr/local/apache2.4/testers/echo.log" or die "can't open log: $!";
LOG->autoflush(1);
STDOUT->autoflush(1);
print LOG scalar(localtime)," $0 started\n";

while (<>) {
    chomp;
    print "$_\n";
}

sub ending {
    print LOG scalar(localtime)," $0 finished\n";
    exit;
}
