#!/usr/bin/env python
# for a given user agent get the class of agent
import psycopg2
import urllib
import json
import time

# from http://www.useragentstring.com/pages/api.php
queryuri = "http://www.useragentstring.com/?uas="
queryurifmt = "getJSON=all"
# for doing queries
conn = psycopg2.connect(dbname="mod_ml", user="cal", password="cmpt416summer2015")
cur = conn.cursor()
# for doing updates
ucur = conn.cursor()
cur.execute("select useragent from botlabels where label is null")
for row in cur:
    print row
    ua = row[0]
    if ua == '' or ua == '-': continue
    testuri = "%s%s&%s" % (queryuri, urllib.quote_plus(ua), queryurifmt)
    print testuri
    response = urllib.urlopen(testuri)
    try:
        jsonstr = response.read()
        print jsonstr
        rdata = json.loads(jsonstr)
        print rdata
        if 'agent_type' in rdata:
            ucur.execute("update botlabels set label=%(label)s where useragent=%(ua)s",
                    {'label': rdata['agent_type'], 'ua': ua})
            conn.commit()
    except Exception as e:
        print str(e)
        print "failed reading json"

    time.sleep(1)

