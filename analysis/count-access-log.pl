#!/usr/bin/perl
# read an access log from apache2 in combined format and count reqs per second
while (<>) {
	$rs{$1}++ if /:(\d\d:\d\d:\d\d) -0700/; 
}
foreach (sort keys %rs) { 
	$sum+= $rs{$_}; 
	$cnt++; 
	print "$_ => $rs{$_}\n"; 
	$min = $rs{$_} if !defined $min or $rs{$_} < $min; 
	$max = $rs{$_} if $rs{$_} > $max; 
}
printf "average %.2f min %d max %d\n", $sum/$cnt, $min, $max
