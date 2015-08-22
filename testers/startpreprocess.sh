#!/bin/sh
# preprocess.py adds data to the mod_ml database

dir=/srv/cal/src/apache/ml/testers
port=49994
pidfile=$dir/preprocess.pid
log=$dir/preprocess.log
proc=preprocess.py
cd $dir

running=`/bin/ps x | grep python | grep -v grep | grep $proc`

if [ $? -eq 0 ] && [ "$running" != "" ]
then
    if [ "$1" = "restart" ]
    then
        pid=`/bin/cat $pidfile`
        if [ "$pid" != "" ]
        then
            /bin/kill $pid
        fi
        echo restarting $proc
    else
        echo $proc is running `/bin/date` ps output $running
        return
    fi
else
    echo $proc not running
fi

(while /bin/true; do nice ./$proc $port ; done) >> $log 2>&1 &
echo $! > $pidfile && echo $proc started with pid `/bin/cat $pidfile` on `/bin/date`

