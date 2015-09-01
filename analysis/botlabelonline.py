#!/usr/bin/env python
# for a given user agent get the class of agent
# use this to update the class field in botstats
import psycopg2
import urllib
import json
import time
import sys
import os
import socket
import traceback
from mldb import dbname, dbuser, dbpw

if len(sys.argv) < 2:
    print >>sys.stderr, "need a port!"
    sys.exit(1)

port = int(sys.argv[1])
if port < 1024:
    print >>sys.stderr, "port should be > 1024"
    sys.exit(1)

def getlabel(ua, conn):
    """ 
    update botlabels with a ua it hasn't seen yet 
    in botlabels "label" is the designation from useragentstring
    but isbot is our designation of whether the ua is a bot
    since we are doing binary classification this can be 0|1
    in the botstats table this translates to -1|1 so SVM can work
    """
    ucur = conn.cursor()
    # from http://www.useragentstring.com/pages/api.php
    queryuri = "http://www.useragentstring.com/?uas="
    queryurifmt = "getJSON=all"
    testuri = "%s%s&%s" % (queryuri, urllib.quote_plus(ua), queryurifmt)
    # time.sleep(0.5)
    response = urllib.urlopen(testuri)
    try:
        jsonstr = response.read()
        rdata = json.loads(jsonstr)
        if 'agent_type' in rdata:
            ucur.execute("update botlabels set label=%(label)s where useragent=%(ua)s",
                    {'label': rdata['agent_type'], 'ua': ua})
            conn.commit()
            return rdata['agent_type']

    except Exception as e:
        print str(e)
        print "failed reading json"
        return None

def setknown(conn):
    """
    get a list of known uas and their lable from useragentstring
    """
    cur = conn.cursor()
    cur.execute("select useragent,label from botlabels where label is not null")
    known = {}
    for row in cur:
        ua,label = row
        if ua in known: continue
        known[ua] = label
    return known

def setisbot(conn):
    """
    fill the isbot dict with mapping of label => isbot 
    """
    cur = conn.cursor()
    cur.execute("select label,isbot from isbot")
    isbot = {}
    for row in cur:
        label,bot = row
        if label in isbot: continue
        isbot[label] = bot
    return isbot

def updateclasses(conn):
    """
    wait on a port and receive uas 
    try and find whether they are a bot or not
    return the class entry with their class -1 = human, 1 = bot
    remember what we have seen
    """
    global port
    known = setknown(conn)
    isbot = setisbot(conn)
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    print "binding to port",port
    s.bind(('',port))
    s.listen(8)
    while True:
        try:
            stream, server = s.accept()
            childpid = os.fork()
            if childpid != 0:
                pid, status = os.wait()
            else:
                try:
                    ua = stream.recv(2048).strip()
                    if ua in known:
                        label = known[ua]
                    else:
                        label = getlabel(ua, conn)
                        known[ua] = label
                    stream.sendall("%s\n" % isbot[label])
                except:
                    pass
                finally:
                    os._exit(0)

        except Exception as e:
            pass
        finally:
            stream.close()

if __name__ == '__main__':
    conn = psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
    updateclasses(conn)

