#!/bin/sh
cd /srv/cal/src/apache/ml/analysis
killall startbotpreprocess.sh 
killall python
./startbotpreprocess.sh

