#!/usr/bin/perl
# idea is to replay http requests to a test server based on log input
# timing is at least somewhat preserved - maybe proportionally shortened
# when using this script to handle a massive volume of logs use the 
# IP of the test server to avoid DNS throttling
# alternatively putting the domain/ip mapping /etc/hosts could work
use strict;
use Getopt::Std;
use Time::Local;
use Time::HiRes qw/usleep/;
use List::Util qw/shuffle/;
use Data::Dumper;
use DBI;

my %opt;
getopts('a:u:m:o',\%opt);
my $awake = ($opt{a} ? 1 : 0);
my $asleep = $opt{a} || 100; # set time to sleep in milliseconds

my $testuri = $opt{u};  # use the IP in high throughput situations */
die "need a test uri $testuri!" unless $testuri =~ m/\w/;
# allow for round robin with multiple uris
my @uris = split ",", $testuri;
my $whichuri = 0;
my $uricount = scalar @uris;

# hide ip addresses
my $obfuscate = $opt{o};

# will mulitply wait interval by this
my $multiplier = $opt{m} || 0.5;

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

=filter out
example curl command

 curl -X GET --header 'REMOTE_ADDR: 1.2.3.4' --header 'status_line: 404 Not Found' --header 'epoch: [17/Jul/2015:12:01:38 -0400]' --header 'hour: [17/Jul/2015:12:02:37 -0400]' http://cloudsmall8.cs.surrey.sfu.ca:8898/wow

from 

64.237.45.114 - - [17/Jul/2015:00:00:45 -0400] "GET /auto/asacpnews.rss?phpsessid=9bd08b121d40f121003b0a564113a4ba HTTP/1.0" 200 4213 "-" "RSSingBot (http://www.rssing.com)"

=cut

my $sleep = 0;
my $prevmicros;
my $div = 1_000_000;
my $processed = 0;

# see http://zetcode.com/db/sqliteperltutorial/connect/
# use a database to hold fake ip addresses so we can have multiple processes doing testing
my $dbh = DBI->connect("dbi:SQLite:dbname=replay.sqlite3","","");

$dbh->do(
        "create table if not exists fakeips ".
        "(realip varchar(128) primary key, fakeip varchar(128))"
) or die $dbh->errstr;
my $get = $dbh->prepare("select fakeip from fakeips where realip=?");
my $ins = $dbh->prepare("insert or ignore into fakeips (realip,fakeip) values (?,?)");

while (<>) {
	print;
	chomp;
	next unless my ($ip, $dt, $link, $status, $ref, $ua) = 
		($_=~m#^(\S*).*?(\[[^\]]*\])\s*"\S+\s*(\S.*\S?)\s+[^"]*"\s*(\d+)\s+\S+\s+"([^"]*)"\s*"([^"]*)"#);
	print "\nstarting processing $ip|$dt|$link|$status|$ref|$ua\n";

	$get->execute($ip) or die $get->errstr;
	my $fakeip;
	if ($get->rows) {
		my $row = $get->fetch;
		$fakeip = $row->[0];
	} else {
		if ($obfuscate) {
			$fakeip = join ".", 
				(shuffle(map { $_ % 256 } 
					(map { join '', shuffle(split //, $_) } split /\./, $ip)));
		} else {
			$fakeip = $ip;
		}
		$ins->execute($ip,$fakeip) or warn $ins->errstr;
	}
	$get->finish;
	print "ip $ip now $fakeip\n";

	my $micros;
	if (
		my ($day, $monstr, $year, $hour, $min, $sec) = 
		    ($dt =~ m#(\d+)/(\w+)/(\d\d\d\d):(\d+):(\d+):(\d+)#)
	) {
		my $mon = $months{$monstr};
		my $yr = $year - 1900;
		my $m = $mon - 1;
		$micros = timelocal($sec, $min, $hour, $day, $m, $year) * $div;
	}
	next unless defined $micros;	

	$sleep = $multiplier * ($micros - $prevmicros) if defined $prevmicros;
	print "got epoch $micros from $dt (sleep $sleep) ",scalar(localtime($micros/$div)),"\n";
	$prevmicros = $micros;

	my $ct = 'text/html' if $link =~ m#(?:^/(?:\?|$)|^/\w+\.php|^/\w+\.html?)#;
    my $uri = $uris[$whichuri];
    $whichuri = (++$whichuri) % $uricount;
	my $curlcmd = "curl ".
		"--header 'REMOTE_ADDR: $fakeip' ".
		"--header 'status_line: $status' ".
		"--header 'epoch: $dt' ".
		"--header 'hour: $dt' ".
		"--header 'content_type: $ct' ".
		"--user-agent '$ua' ". 
		"'$uri$link' &";

	$processed++;
	print "$processed: $curlcmd\n\n";

	system $curlcmd || warn "error running $curlcmd: $!";

	next if $sleep < 0;
	if ($awake) {
		usleep $asleep*1000 if $asleep > 0;
	} else {
		usleep $sleep;
	}
}

