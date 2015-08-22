#!/usr/bin/env python
# for a given user agent get the class of agent
# use this to update the class field in botstats
import psycopg2
import urllib
import json
import time
import sys
from mldb import dbname, dbuser, dbpw

verbose = False
if len(sys.argv) > 1 and sys.argv[1] == 'verbose':
    print "verbose output"
    verbose = True

def getlabel(ua, conn):
    """ 
    update botlabels with a ua it hasn't seen yet 
    in botlabels "label" is the designation from useragentstring
    but isbot is our designation of whether the ua is a bot
    since we are doing binary classification this can be 0|1
    in the botstats table this translates to -1|1 so SVM can work
    """
    global verbose
    ucur = conn.cursor()
    # from http://www.useragentstring.com/pages/api.php
    queryuri = "http://www.useragentstring.com/?uas="
    queryurifmt = "getJSON=all"
    testuri = "%s%s&%s" % (queryuri, urllib.quote_plus(ua), queryurifmt)
    if verbose: print testuri
    # time.sleep(0.5)
    response = urllib.urlopen(testuri)
    try:
        jsonstr = response.read()
        if verbose: print jsonstr
        rdata = json.loads(jsonstr)
        if verbose: print rdata
        if 'agent_type' in rdata:
            ucur.execute("update botlabels set label=%(label)s where useragent=%(ua)s",
                    {'label': rdata['agent_type'], 'ua': ua})
            conn.commit()
            return rdata['agent_type']

    except Exception as e:
        print str(e)
        print "failed reading json"

def updatelabel(ip, ua, label, isbot, conn):
    """ 
    given a label, ip and ua 
    find out if the ua is a bot from the isbot map
    update botstats with this information
    """
    global verbose
    if ua == None or label == None or len(ua) == 0 or len(label) == 0:
        if verbose: print "skipping empty ua/label for ",ip
        return

    ucur = conn.cursor()
    if verbose: print "ip ",ip," found label ",label," for ua ",ua
    if label in isbot:
        bot = isbot[label]
        if bot != None:
            cl = (2*bot) -1
            if verbose: print "found isbot ",bot," class ",cl," saving"
            try:
                ucur.execute(
                    """
                    update botstats 
                    set class=%(class)s,label=%(label)s 
                    where ip=%(ip)s
                    """,{'class':cl,'label':label,'ip':ip})
                conn.commit()
            except Exception as e:
                print "error ",str(e)

def setknown(conn):
    """
    get a list of known uas and their lable from useragentstring
    """
    global verbose
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
    global verbose
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
    look for unclassed stats entries 
    try and find whether they are a bot or not
    update the class entry with their class -1 = human, 1 = bot
    """
    global verbose
    known = setknown(conn)
    isbot = setisbot(conn)
    cur = conn.cursor()
    cur.execute(
        """
        select ip,uas 
        from botstats 
        where label is null
        and mean is not null 
        and array_length(uas,1) > 0
        and sample = 1420099203
        limit 2000
        """)
    for row in cur:
        if verbose: print row
        ip, uas = row
        # too many ips have enormous numbers of uas
        # assuming first one is representative
        ua = uas[0]
        if ua in known:
            label = known[ua]
            updatelabel(ip, ua, known[ua], isbot, conn)
        else:
            label = getlabel(ua, conn)
            known[ua] = label
            if label == None:
                print "error getting label for ",ua
                continue
            updatelabel(ip, ua, label, isbot, conn)


if __name__ == '__main__':
    conn = psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
    updateclasses(conn)

