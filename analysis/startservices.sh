#!/bin/sh
# start services needed for parallel processing test where everything is done at once online
# use this on the command line only
cd vw && ./startvw.sh 26541 26542 && cd ..
./botlabelonline.py 59999 &
echo check that memcached is also running
sudo service memcached status

