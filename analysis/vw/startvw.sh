#!/bin/sh
# start two vw daemons: one to build a model and one to use it
# periodically restart the classifier
lport=$1
if [ "$lport" = "" ]
then
    lport=26541
fi

cport=$2
if [ "$cport" = "" ]
then
    cport=26542
fi
echo starting classifier on $cport and learner on $lport

vw=/usr/local/bin/vw
cpidfile=/tmp/vwclassifier.pid
lpidfile=/tmp/vwlearner.pid
# quiet=--quiet
quiet=
if [ -f model ]
then
    # this process does the online classification - model is a symlink to latest model
    /bin/sh -c "echo \$\$ > $cpidfile && $vw -t -i model --daemon --port $cport $quiet"
    # this process does model building - this expects some form of class label for the input (-1,1)
    # the hinge loss function seemed to work better than logistic but there are a vast number of possibilities
    model=botmodel-`/bin/date +%Y%m%d_%H%M%S`.vw
    # /bin/rm model
    /bin/ln -s $model model
    /bin/sh -c "echo \$\$ > $lpidfile && $vw --loss_function=hinge --no_stdin --save_resume -f $model --save_per_pass $quiet --daemon --port $lport"
else
    echo need to create a model before starting classifier
fi
