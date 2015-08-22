#!/bin/sh
# for running from cron
. /home/cal/.bashrc
cd /srv/cal/src/apache/ml/analysis
./botstatsclass.py $@
