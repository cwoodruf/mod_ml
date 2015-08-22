#!/bin/sh
# used by botremodel.sh as part of the model regeneration process
# BotDump expects a file name to output to and optionally a sample size 
# to get a full parameter list run BotDump with no parameters
cd /srv/cal/src/apache/ml/analysis
cp=.:./java/postgresql.jar:./java/weka-pg.jar 
/usr/bin/javac -cp $cp BotDump.java
/usr/bin/java -cp $cp BotDump $@
