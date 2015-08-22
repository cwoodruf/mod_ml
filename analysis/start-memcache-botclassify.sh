#!/bin/sh
cd /srv/cal/src/apache/ml/analysis
(while /bin/true
do
./memcache-botclassify.py 39999 /etc/hosts 127.0.0.1:11211 predictions.txt >> ../../ml_logs/memcache-botclassify-39999.log 2>&1
done) &
