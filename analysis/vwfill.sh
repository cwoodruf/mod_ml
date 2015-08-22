#!/bin/sh
CURRENTMODEL=$1
if [ "$CURRENTMODEL" = "" ]
then
    echo "usage: $0 {vw model file}"
    exit 1
fi
vw=/usr/local/bin/vw
psql=/usr/bin/psql
conds="and mean is not null and reqs > 0"
fields="class,1,ip||'|','mean:'||mean,'var:'||var,'skew:'||skew,'kurtosis:'||kurtosis,'hmean:'||hmean,'hvar:'||hvar,'hskew:'||hskew,'hkurtosis:'||hkurtosis,'htmean:'||htmean,'htvar:'||htvar,'htskew:'||htskew,'htkurtosis:'||htkurtosis,'poverr:'||(pages/reqs),'uacount:'||(array_length(uas,1)),'errprop:'||(errs/reqs)"
$psql -dmod_ml -t -A -F ' ' -c "select $fields from botstats where 1=1 $conds" | \
    $vw -t -d /dev/stdin -p /dev/stdout -i $CURRENTMODEL --quiet | \
    perl -ne '($pred,$ip) = split " "; $pred = ($pred < 0 ? -1: 1); print "$pred $ip\n"'
