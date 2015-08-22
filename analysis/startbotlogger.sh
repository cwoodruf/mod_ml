#!/bin/sh
# the botlogger.py script  is a long running ip service 
# used by mod_ml enabled apache servers using the ml_bot.conf
# conf file that should be found in the main directory
# this is a test system that would need some tweaking to work 
# well in production under heavy load
cd /srv/cal/src/apache/ml/analysis
port=39992
log=../../ml_logs/botlogger-$port-`/bin/date +%Y%m%d`

echo starting botlogger.py on port $port log is $log
./botlogger.py $port /etc/hosts >> $log 2>&1 &
