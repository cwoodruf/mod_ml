#!/bin/sh
# rebuild models using weka's CLI
# this script first creates a sample of data in the botstats table
# and then outputs it in weka's arff format
# a series of models are built (takes 10-20 min depending but some are quite quick)
# the MODELS link (used by botclassifier.sh) is redirected to the output directory
# on successful model build
basedir=/srv/cal/src/apache/ml/analysis
cd $basedir
today=`/bin/date +%Y%m%d_%H%M%S`
modelbase=$basedir/models
# cleanup
/usr/bin/find $modelbase -depth -mtime +2 -exec /bin/rm -fr {} \;

modeldir=$modelbase/$today
/bin/mkdir -p $modeldir
traindata=$modeldir/botstats-training-$today.arff

cp=".:./java/postgresql.jar:./java/weka-pg.jar"
# -Xss1g change stack size to 1GB
java="/usr/bin/time /usr/bin/java -Xss1g -cp $cp"

# first label all our data
# ./botstatsclass.py
# use psql/botstats.psql to build the db tables for the system
# then dump some data to make a model by running BotDump
# the default for the number of records sampled is 20000 - assuming there are 20000 that have not been sampled
# as uneven numbers of records by class are produced BotDump tries to remedy this by repeating records
echo "update botstats set sample=null where sample is not null" | /usr/bin/psql -dmod_ml
./botdump.sh botstats $traindata $@

echo building vowpal wabbit models and testing
cd vw
trainvw=`./weka2vw.pl $traindata`
for loss in hinge logistic
do
    echo training loss function $loss
    /usr/local/bin/vw -d $trainvw --loss_function=$loss --passes 20 --cache_file c -f $trainvw.$loss.model --quiet
    echo testing loss function $loss
    $basedir/vwtest1000quickly.sh $trainvw.$loss.model
done
cd ..

# now build some new weka models
# -c 1 is really important as it identifies the class to be used -o reduces the output
# echo weka.classifiers.trees.BFTree -S 1 -M 2 -N 5 -C 1.0 -P POSTPRUNED -c 1 -o -t $traindata -d $modeldir/BFTree.model &&\
# $java weka.classifiers.trees.BFTree -S 1 -M 2 -N 5 -C 1.0 -P POSTPRUNED -c 1 -o -t $traindata -d $modeldir/BFTree.model &&\
echo weka.classifiers.trees.J48 -C 0.35 -M 2 -c 1 -o -t $traindata -d $modeldir/J48.model &&\
$java weka.classifiers.trees.J48 -C 0.35 -M 2 -c 1 -o -t $traindata -d $modeldir/J48.model &&\
# echo weka.classifiers.trees.RandomForest -I 100 -K 0 -S 1 -c 1 -o -t $traindata -d $modeldir/RandomForest.model &&\
# $java weka.classifiers.trees.RandomForest -I 100 -K 0 -S 1 -c 1 -o -t $traindata -d $modeldir/RandomForest.model &&\
# echo weka.classifiers.trees.FT -I 15 -F 0 -M 15 -W 0.0 -c 1 -o -t $traindata -d $modeldir/FT.model &&\
# $java weka.classifiers.trees.FT -I 15 -F 0 -M 15 -W 0.0 -c 1 -o -t $traindata -d $modeldir/FT.model &&\
# echo weka.classifiers.trees.NBTree -c 1 -o -t $traindata -d $modeldir/NBTree.model &&\
# $java weka.classifiers.trees.NBTree -c 1 -o -t $traindata -d $modeldir/NBTree.model &&\
# echo weka.classifiers.trees.RandomTree -K 0 -M 1.0 -S 1 -c 1 -o -t $traindata -d $modeldir/RandomTree.model &&\
# $java weka.classifiers.trees.RandomTree -K 0 -M 1.0 -S 1 -c 1 -o -t $traindata -d $modeldir/RandomTree.model &&\
# echo weka.classifiers.trees.REPTree -M 2 -V 0.001 -N 3 -S 1 -L -1 -o -t $traindata -d $modeldir/REPTree.model &&\
# $java weka.classifiers.trees.REPTree -M 2 -V 0.001 -N 3 -S 1 -L -1 -o -t $traindata -d $modeldir/REPTree.model &&\
# echo weka.classifiers.trees.SimpleCart -S 1 -M 2.0 -N 5 -C 1.0 -c 1 -o -t $traindata -d $modeldir/SimpleCart.model &&\
# $java weka.classifiers.trees.SimpleCart -S 1 -M 2.0 -N 5 -C 1.0 -c 1 -o -t $traindata -d $modeldir/SimpleCart.model &&\
    (/bin/rm MODELS; /bin/ln -s $modeldir MODELS)

# some results with a 20000 count sample boosted to around 38000 using ./test1000.sh
# which tests 1000 items not included in the sample used to model building
# the weka figures are very misleading partly because of the boosted # bot examples (try w/o?)
# overall:      1000 bot YES 287 1000 bot NO 951
# RandomForest: 1000 bot YES 302 1000 bot NO 928
# BFTree:       1000 bot YES 454 1000 bot NO 895
# NBTree:       1000 bot YES 253 1000 bot NO 945
# SimpleCart:   1000 bot YES 389 1000 bot NO 905

echo done
