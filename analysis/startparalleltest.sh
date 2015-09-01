#!/bin/sh
# meant to be run from a window 
# 206.12.16.221 is cloudbig
svchost=206.12.16.221
cs8=206.12.16.211
# first service is the vw classifier (see vw/startvw.sh)
# second save classification decisions to redis
# third service is the vw learner (see vw/startvw.sh)
# fourth service is the bot labelling service (botlabelonline.py)
./botlogger.py 39992 /etc/hosts $svchost:26542 $svchost $svchost:26541 $svchost:59999
