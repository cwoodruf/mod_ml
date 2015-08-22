#!/bin/sh
# make all botstats table data in to training and test sets for SVM processing in vowpal wabbit
cd /srv/cal/src/apache/ml/analysis
echo running query on mod_ml.botstats
psql -dmod_ml -f./psql/vowpal_wabbit.psql 
grep -P '^-1|^1' botstats.vw | \
    shuf | \
    perl -ne 'if (rand() < 0.9) { print; } else { print STDERR; }' > botstats-train.vw 2>botstats-test-unsorted.vw 
sort botstats-test-unsorted.vw | uniq > botstats-test.vw
echo training with svm
echo saving model to botstats-svm.vw
vw -d botstats-train.vw -f botstats-svm.vw --loss_function=$@
if [ -f botstats-svm.vw ]
then
    echo saving test predictions to botstats-predictions.txt
    vw -d botstats-test.vw -t -i botstats-svm.vw -p botstats-predictions.txt
    perl -ne '
    BEGIN { 
        $ls = $/; 
        undef $/; 
        open TEST, "botstats-test.vw" or die $!; 
        %actuals=map { @l=split /\s/; ($l[2],$l[0]) } split /\n/, <TEST>; 
        $/ = $ls 
    }
    ($pred,$label) = split /\s+/; 
    $found = $actuals{"$label|"};
    chomp $found; 
    $found =~ s/(^-?1) .*/$1/; 
    $err = ((($found < 0 and $pred < 0) or ($found > 0 and $pred > 0)) ? 0: 1);
    $errs++ if $err;
    $lines++;
    printf "%-40s actual %2d predicted %2.4f error %s\n", $label, $found, $pred, ($err?"yes":"no");
    END { printf STDERR "errors %5d predictions %5d %2.2f%% right\n", $errs, $lines, (100.0*($lines-$errs)/$lines); }
    ' botstats-predictions.txt > botstats-comparison.txt
fi
