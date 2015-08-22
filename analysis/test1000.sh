#!/bin/bash
# find how right we are using data from a stats table with > 1000 bots classified as bots
# botstats is a view that combines botstats and botiplabels
# run the botclassifier.sh script in a separate window before running this script
if [ "$1" != "" ]
then
    port=$1
else
    port=39999
fi
if [ "$2" != "" ]
then
    table=$2
else
    table=botstats
fi
if [ "$3" != "" ]
then
    host=$2
else
    host=206.12.16.237
fi
if [ "$4" = "null" ]
then
    sample="sample is null"
elif [ "$4" = "not null" ]
then
    sample="sample is not null"
elif [ "$4" != "" ]
    sample="sample = $4"
else
    sample="sample is null"
fi
if [ "$5" != "" ]
then
    limit=$5
else
    limit=1000
fi

conds="and $sample and mean is not null and reqs > 0 order by random() limit $limit"
echo 1000 bot Random
# sample identifies which records were used to make the most recent model used for classification
query="select ip,class from $table where class in (-1,1) $conds "
echo $query
for ipclass in `psql -dmod_ml -tA -F ',' -c "$query" `; 
do 
    # echo $ipclass
    ip=`echo $ipclass | perl -pe 's/^\s*(\S*)\s*,.*/$1/'`
    class=`echo $ipclass | perl -pe 's/^.*,\s*(\S*).*/$1/'`
    predicted=`echo $ip | netcat $host $port`
    if [[ $predicted =~ YES ]] && [ "$class" -eq 1 ]
    then
       echo GOOD
    elif [[ $predicted =~ NO ]] && [ "$class" -eq -1 ]
    then
       echo GOOD
    else
       echo BAD
    fi 
done | grep GOOD | wc -l

echo 1000 bot YES
# sample identifies which records were used to make the most recent model used for classification
query="select ip from $table where class=1 $conds "
echo $query
for ip in `psql -dmod_ml -c "$query"`; 
do 
    echo $ip | netcat $host $port
    echo $ip 
done | grep YES | wc -l

echo 1000 bot NO
query="select ip from $table where class=-1 $conds "
echo $query
for ip in `psql -dmod_ml -c "$query"`; 
do 
    echo $ip | netcat $host $port
    echo $ip 
done | grep NO | wc -l

