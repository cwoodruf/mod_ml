#!/bin/sh
data=$1
model=$2
vw=/usr/local/bin/vw
echo $data | $vw -t -d /dev/stdin -p /dev/stdout -i $2 --quiet
