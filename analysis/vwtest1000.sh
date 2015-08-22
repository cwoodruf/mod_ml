#!/bin/bash
# find how right we are using data from a stats table with > 1000 bots classified as bots
# botstats is a view that combines botstats and botiplabels
# run the botclassifier.sh script in a separate window before running this script
if [ "$1" != "" ]
then
    table=$1
else
    table=botstats
fi

function judge {
    # echo $ipclass
    ip=`echo $ipclass | perl -pe 's/^\s*(\S*)\s*,.*/$1/'`
    class=`echo $ipclass | perl -pe 's/^.*,\s*(\S*).*/$1/'`
    predicted=`./vwbotclassify.sh $ip | perl -ne 'chomp; printf("%d", 10*$_)'`
    # echo $predicted
    if [ $predicted -gt 0 ] && [ "$class" -eq 1 ]
    then
       echo GOOD
    elif [ $predicted -lt 0 ] && [ "$class" -eq -1 ]
    then
       echo GOOD
    else
       echo BAD
    fi 
}

echo 1000 bot Random
# sample identifies which records were used to make the most recent model used for classification
query="select ip,class from $table where class in (-1,1) and sample is null order by random() limit 1000"
echo $query
for ipclass in `psql -dmod_ml -tA -F ',' -c "$query" `; 
do 
    judge
done | grep GOOD | wc -l

echo 1000 bot YES
# sample identifies which records were used to make the most recent model used for classification
query="select ip from $table where class=1 and sample is null order by random() limit 1000"
echo $query
for ip in `psql -dmod_ml -c "$query"`; 
do 
    judge
done | grep GOOD | wc -l

echo 1000 bot NO
query="select ip from $table where class=-1 and sample is null order by random() limit 1000"
echo $query
for ip in `psql -dmod_ml -c "$query"`; 
do 
    judge
done | grep GOOD | wc -l

