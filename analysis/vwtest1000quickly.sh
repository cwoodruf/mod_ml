#!/bin/sh
CURRENTMODEL=$1
vw=/srv/cal/src/vowpal_wabbit/vowpalwabbit/vw
vw=/usr/local/bin/vw
psql=/usr/bin/psql
conds="and mean is not null and sample is null order by random() limit 1000"
fields="class,'|','mean:'||mean,'var:'||var,'skew:'||skew,'kurtosis:'||kurtosis,'hmean:'||hmean,'hvar:'||hvar,'hskew:'||hskew,'hkurtosis:'||hkurtosis,'htmean:'||htmean,'htvar:'||htvar,'htskew:'||htskew,'htkurtosis:'||htkurtosis,'poverr:'||(pages/reqs),'uacount:'||(array_length(uas,1)),'errprop:'||(errs/reqs)"
echo "1000 random bots"
$psql -dmod_ml -t -A -F ' ' -c "select $fields from botstats where class in (-1) $conds" | \
    $vw -t -d /dev/stdin -p /dev/stdout -i $CURRENTMODEL --quiet | \
    perl -ne 'chomp; printf "%s\n", ($_ < 0.0 ? "human": "bot")' | \
    grep "human" | wc -l
echo "1000 random humans"
$psql -dmod_ml -t -A -F ' ' -c "select $fields from botstats where class in (1) $conds" | \
    $vw -t -d /dev/stdin -p /dev/stdout -i $CURRENTMODEL --quiet | \
    perl -ne 'chomp; printf "%s\n", ($_ < 0.0 ? "human": "bot")' | \
    grep "bot" | wc -l
