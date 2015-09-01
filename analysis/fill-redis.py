#!/usr/bin/env python
import redis
import sys
import logging
import copy
import time

# logging.basicConfig(level=logging.ERROR)
# logging.basicConfig(level=logging.DEBUG)
logging.basicConfig(level=logging.INFO)

if len(sys.argv) < 3:
    print "usage:",sys.argv[0],"{redis server} {prediction files}"
    sys.exit(1)

rs = redis.Redis(sys.argv[1])

def read_predictions():        
    """
    read a series of prediction files
    these will have weighted predictions
    for ips
    idea is to add the predictions together
    if prediction < 0 then its human
    > 0 its a bot
    0 don't know
    *** multiple predictions for the same ip can exist in the same file ***
    """
    global rs

    args = copy.deepcopy(sys.argv)
    initialized = False
    seen = {}
    for i in range(2,len(args)):
        fname = args[i]
        logging.info("reading %s at %d" % (fname,time.time()))
        with open(fname,"r") as pfile:
            for line in pfile:
                p, hostip = line.split()
                host, ip = hostip.split('/')
                pred = int(p)
                try:
                    if ip in seen:
                        prev = int(rs.get(ip))
                    else:
                        prev = 0
                except:
                    prev = 0
                newpred = prev+pred
                logging.debug("saving prediction %d for %s" % (newpred,ip))
                rs.set(ip,newpred)
                rs.set(hostip,newpred)
                seen[ip] = True
        logging.info("fill redis %d" % time.time())

if __name__ == '__main__':
    read_predictions()
