#!/bin/sh
# epoch.pl converts apache time into epoch seconds

dir=/srv/cal/src/apache/ml/testers
port=39993
pidfile=$dir/epoch.pid
log=$dir/epoch.log
proc=epoch.pl
cd $dir

running=`/bin/ps x | grep perl | grep -v grep | grep $proc`

if [ $? -eq 0 ] && [ "$running" != "" ]
then
    if [ "$1" = "restart" ]
    then
        pid=`/bin/cat $pidfile`
        if [ "$pid" != "" ]
        then
            /bin/kill $pid
            /usr/bin/killall $proc > /dev/null 2>&1
        fi
        echo restarting $proc
    else
        echo $proc is running `/bin/date` ps output $running
        return
    fi
else
    echo $proc not running
fi

(while true; do ./$proc $port; done >> $log 2>&1) &
echo $! > $pidfile && echo $proc started with pid `/bin/cat $pidfile` on `/bin/date` ps output `/bin/ps x | grep perl | grep -v grep | grep $proc`

