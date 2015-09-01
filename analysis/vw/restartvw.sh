#!/bin/sh
# start two vw daemons: one to build a model and one to use it
# periodically restart the classifier
cport=$1
if [ "$cport" = "" ]
then
    cport=26542
fi
echo restarting classifier on $cport

vw=/usr/local/bin/vw
cpidfile=/tmp/vwclassifier.pid
if [ -f model ]
then
    if [ -f $pidfile ]
    then
        /usr/bin/kill `/bin/cat $cpidfile`
    fi
    # this process does the online classification - model is a symlink to latest model
    $vw -t -i model --daemon --port $cport
    echo $! > $cpidfile
else
    echo need to create a model before starting classifier
fi
