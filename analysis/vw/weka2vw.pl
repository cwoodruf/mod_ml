#!/usr/bin/perl
# change a weka arff format file to vw format
# grab classes from the {} in the metadata at the top
use Data::Dumper;
$fieldcount = 0;
while (<>) {
    # print STDERR;
    chomp;
    if (!defined $outfile) {
        ($outfile = $ARGV) =~ s#.*/(.*)#$1#;
        $outfile .= '.vw';
        open OUT, "> $outfile" or die "can't write to $outfile: $!";
        print "$outfile\n";
    }
    if (/\@data/) {
        $data = 1;
        next;
    } elsif (/\@relation/) {
        $relation = 1;
        next;
    } else {
        if ($data) {
            $relation = 0;
            @fields = split /,/, $_;
            warn "invalid row $_" unless scalar @fields == $fieldcount;
            print OUT "$fields[$classpos] | ";
            for ($i=0; $i<$fieldcount; $i++) {
                next if $i == $classpos;
                print OUT "$meta[$i]{name}:$fields[$i] ";
            }
            print OUT "\n";
        } elsif ($relation) {
            if (/\@attribute\s+(\w+)\s+(.*)/) {
                ($name, $type) = ($1,$2);
                $meta[$fieldcount] = {name=>$name,type=>$type};
                # only considers one class per row
                if ($type =~ /{(.*?)}/) {
                    $classpos = $fieldcount;
                    $classes = $1;
                    @classes = split /,/, $classes;
                    $type = 'class';
                    $meta[$fieldcount]{opts} = {classpos=>$classpos, classes=>[@classes]};
                } else {
                    $opts = undef;
                }
                $fieldcount++;
            }
        }
    }
}
__END__
@relation botstats

@attribute class {-1,1}
@attribute mean numeric
@attribute var numeric
@attribute skew numeric
@attribute kurtosis numeric
@attribute hmean numeric
@attribute hvar numeric
@attribute hskew numeric
@attribute hkurtosis numeric
@attribute htmean numeric
@attribute htvar numeric
@attribute htskew numeric
@attribute htkurtosis numeric
@attribute poverr numeric
@attribute uacount numeric
@attribute errprop numeric

@data

    }
}
