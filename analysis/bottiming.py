#!/usr/bin/env python
# save timing information on requests based on ip address
# this is intended to be used with the ippreprocess.py script
# it is the more naive approach that is too slow in practice for 
# high levels of traffic
# botlog.py uses functions from this module
# botlogger.py uses botlog.py and is able to keep up with more traffic
import sys
import json
import re
import os
import socket
import traceback
import logging

import redis
rs = None

import psycopg2
from mldb import dbname, dbuser, dbpw
autocommit = True

################################################################################
# functions used by botlog.py                                                  #
################################################################################
# given a list of numbers generate some stats on the distribution
def genstats(diffs):
    # this method is going to be slow but my kurtosis calculation formula isn't working
    if len(diffs) > 1:
        s = sum(diffs)+0.0
        n = len(diffs)+0.0
        m = s/n
        se = ce = qe = 0.0
        for d in diffs:
            se += (d - m)**2.0
            ce += (d - m)**3.0
            qe += (d - m)**4.0
        var = se/(n-1.0)
        # source for sn, kn1, kn2 sample size corrections 
        # http://www.ats.ucla.edu/stat/mult_pkg/faq/general/kurtosis.htm
        # and http://www.itl.nist.gov/div898/handbook/eda/section3/eda35b.htm
        if var != 0 and n > 1: 
            sn = ((n*(n-1.0))**(1/2))/(n-1.0)
            skew = sn*(ce/n)/(var*(var**(1.0/2.0)))
            if n > 3:
                kn1 = (n*(n+1.0))/((n-1.0)*(n-2.0)*(n-3.0))
                kn2 = ((n-1.0)**2.0)/((n-2.0)*(n-3.0))
                kurt = kn1*(qe/n)/(var**2.0) - 3.0*kn2
            else:
                kurt = 0.0
        else: 
            kurt = 0.0
            skew = 0.0
        return {'diffs':diffs, 'n': n, 'sum': s, 'mean': m, 'var': se, 'kurtosis': kurt, 'skew': skew}
    return None

# was the request an error?
# hackers will try all sorts of known buggy urls
def iserr(row):
    status_line = row['status_line']
    if status_line[0] == '4' or status_line[0] == '5':
        return True
    return False

# we only keep timing differences for the main html page requested
# this may not make sense for all websites
def ishtml(row):
    content_type = row['content_type']
    if content_type == 'text/html': 
        return True
    return False

# psycopg2 doesn't seem to have a way to return a dict from a select (?)
def row2dict(keys,row):
    return dict((keys[i], item) for i,item in enumerate(row))

def botstats2dict(row):
    keys = ['ip', 'n', 'sum', 'mean', 'var', 'skew', 'kurtosis', 
            'diffs', 'pages', 'reqs', 'hourdiffs', 
            'hn', 'hsum', 'hmean', 'hvar', 'hskew', 'hkurtosis', 
            'errs', 'hours', 
            'htmean', 'htsum', 'htvar', 'htskew', 'htkurtosis', 'htn',
            'uas','class','prediction','sample']
    return row2dict(keys,row)

# example row:
# 0 hour          0, 
# 1 REMOTE_ADDR   '101.142.15.16', 
# 2 status_line   '200', 
# 3 useragent     'Mozilla/5.0 (Linux; U; Android 2.3.6; ...
# 4 epoch         1391415125L, 
# 5 HTTP_HOST     '206.12.16.198:8898'
# 6 content_type  'text/html'
# 7 logid - only for botlog at the moment

def botlog2dict(row):
    keys = ['hour', 'REMOTE_ADDR', 'status_line', 'useragent', 
            'epoch', 'HTTP_HOST', 'content_type','logid']
    return row2dict(keys,row)

def botlatest2dict(row):
    keys = ['hour', 'REMOTE_ADDR', 'status_line', 'useragent', 
            'epoch', 'HTTP_HOST', 'content_type']
    return row2dict(keys,row)

# these get/set functions are used by botlog.py

# store this many items in our arrays
MAXDIFFS = 1000
# ignore anything over 2 days old - too extreme?
OLD = 2 * 86400 * 1000

def vw_string(data, stats):
    """
    make a string for vowpal wabbit -  minus the label
    """
    if stats is None or stats['reqs'] == 0: 
        data['poverr'] = 1
        data['uacount'] = 1
        data['errprop'] = 0
    else:
        data['poverr'] = 0.0+stats['pages']/stats['reqs']
        data['uacount'] = 0.0+len(stats['uas'])
        data['errprop'] = 0.0+stats['errs']/stats['reqs']

    features = "| mean:%(mean)s var:%(var)s skew:%(skew)s kurtosis:%(kurtosis)s hmean:%(hmean)s hvar:%(hvar)s hskew:%(hskew)s hkurtosis:%(hkurtosis)s htmean:%(htmean)s htvar:%(htvar)s htskew:%(htskew)s htkurtosis:%(htkurtosis)s poverr:%(poverr)s uacount:%(uacount)s errprop:%(errprop)s\n" % data
    return features

def get_label(ua, services):
    """
    use a service tied to useragentstring.com 
    to figure out a class for our user agent
    this is used for training new models
    """
    if 'ua' not in services or services['ua'] == None: 
        return None
    uasvc = services['ua']

    label = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(uasvc)
        s.sendall(ua)
        s.shutdown(socket.SHUT_WR)
        rawlabel = s.recv(1024)
        s.close()
        try:
            label = int(rawlabel.strip())
            logging.debug("got label %d for ua %s" % (label, ua))
        except:
            logging.debug("got label None for ua %s" % (ua))
    except:
        pass
    return label

def teach(label, features, services):
    """
    send data to vowpal wabbit online learning process
    needs the label from get_label 
    """
    global uas
    if 'vw_learn' not in services or services['vw_learn'] == None: 
        return None

    vw = services['vw_learn']

    if label == 0:
        features = "-1 "+features
    elif label == 1:
        features = "1 "+features
    else:
        return

    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect(vw)
        s.sendall(features)
        s.shutdown(socket.SHUT_WR)
        s.close()
        logging.debug("vw sent features %s" % (features))
    except:
        pass

def get_prediction(data,stats,services):
    """
    build a vw compatible string and get prediction 
    from vw server if it exists
    """
    global rs
    if 'vw' not in services or services['vw'] == None:
        return None, None
    vw = services['vw']
    features = vw_string(data, stats)

    if stats['class'] == None and 'uas' in stats and len(stats['uas']) > 0:
        stats['class'] = get_label(stats['uas'][0], services)

    # teach(stats['class'], features, services)

    pred = None
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        # "vw" should be a (host,port) tuple
        s.connect(vw)
        s.sendall(features)
        s.shutdown(socket.SHUT_WR)
        rawpred = s.recv(1024)
        s.close()
        rawpred = rawpred.strip()
        pred = float(rawpred)
        logging.debug("got prediction %f for %s" % (pred, features))

        if rs == None and 'redis' in services and services['redis'] != None:
            rs = redis.Redis(services['redis'])
            
        if rs != None:
            print "saving prediction to",data['ip']
            if pred < 0.0:
                rs.set(data['ip'],-1)
            else:
                rs.set(data['ip'], 1)

    except Exception as e:
        a, b, tb = sys.exc_info()
        traceback.print_tb(tb)
        print e
    return pred, stats['class']


def updatestats(ip,diffs,hourdiffs,hours,ucur,uconn,stats=None,services=None):
    """
    updates botstats fields
    """
    try:
        ucur.execute(
            """
            update botstats set
            diffs = %(diffs)s, 
            hourdiffs = %(hourdiffs)s,
            hours = %(hours)s
            where 
            ip = %(ip)s
            """,
            {'diffs':diffs,'hourdiffs':hourdiffs,'hours':hours,'ip':ip})
        if autocommit: uconn.commit()

        # need at least 3 hits for this ip to get two diffs
        if len(diffs) > 1 and len(hourdiffs) > 1:
            s = genstats(diffs)
            h = genstats(hourdiffs)
            ht = genstats(hours)

            if h == None or s == None or ht is None:
                raise Exception("missing data for ip "+ip)

            data = {'ip':ip,
                    'n':s['n'], 'sum':s['sum'], 'mean':s['mean'], 
                    'var':s['var'], 'skew':s['skew'], 'kurtosis':s['kurtosis'], 
                    'hn':h['n'], 'hsum':h['sum'], 'hmean':h['mean'], 
                    'hvar':h['var'], 'hskew':h['skew'], 'hkurtosis':h['kurtosis'], 
                    'htn':ht['n'], 'htsum':ht['sum'], 'htmean':ht['mean'], 
                    'htvar':ht['var'], 'htskew':ht['skew'], 'htkurtosis':ht['kurtosis']
                    }
            try:
                data['pred'], data['class'] = get_prediction(data,stats,services)
                ucur.execute(
                    """
                    update botstats set 
                        n=%(n)s, sum=%(sum)s, mean=%(mean)s, var=%(var)s, 
                        skew=%(skew)s, kurtosis=%(kurtosis)s, 
                        hn=%(hn)s, hsum=%(hsum)s, hmean=%(hmean)s, hvar=%(hvar)s, 
                        hskew=%(hskew)s, hkurtosis=%(hkurtosis)s, 
                        htn=%(htn)s, htsum=%(htsum)s, htmean=%(htmean)s, htvar=%(htvar)s, 
                        htskew=%(htskew)s, htkurtosis=%(htkurtosis)s,
                        prediction=%(pred)s, class=%(class)s
                    where ip=%(ip)s
                    """, data)
                if autocommit: uconn.commit()
            except Exception as e:
                uconn.rollback()
                print >>sys.stderr, "update error for ip ",ip
                print str(e)
    except Exception as e:
        uconn.rollback()
        print >>sys.stderr, "update distributions error for ip ",ip
        print str(e)

def updatestatcounts(ip,reqs,pages,errs,uas,ucur,uconn):
    """ update count fields for stats only """
    try:
        ucur.execute(
            """
            update botstats set
            pages=%(pages)s, reqs=%(reqs)s, errs=%(errs)s,
            uas = %(uas)s
            where ip=%(ip)s
            """,
            {'pages':pages,'reqs':reqs,'errs':errs,'uas':uas,'ip':ip})
        if autocommit: uconn.commit()
    except Exception as e:
        uconn.rollback()
        print >>sys.stderr, "update counts error for ip ",ip
        print str(e)

def initstats(scur,conn,ipd):
    """
    make a blank stats record for an ip
    """
    try:
        ipd['hours'] = [0 for x in range(24)]
        scur.execute( 
            """ 
            insert into botstats 
            (ip,diffs,hourdiffs,hours,reqs,pages,errs,uas) 
            values 
            (%(ip)s,'{}','{}',%(hours)s,0,0,0,'{}') 
            """, ipd)
        conn.commit()
        return True
    except Exception as e:
        conn.rollback()
        print >>sys.stderr, "initstats: ",str(e)
        return False

def getstats(scur, ipd):
    try:
        scur.execute(
            """
            select  
            ip, 
            n, sum, mean, var, skew, kurtosis, diffs, 
            pages, 
            reqs, 
            hourdiffs, hn, hsum, hmean, hvar, hskew, hkurtosis, 
            errs, 
            hours, htmean, htsum, htvar, htskew, htkurtosis, htn, 
            uas, class, prediction, sample
            from botstats where ip=%(ip)s
            """,ipd)
        if scur.rowcount:
            row = scur.fetchone()
            stats = botstats2dict(row)
            return stats
        else:
            return None
    except Exception as e:
        print >>sys.stderr, "getstats: ",str(e)
        return None

def flatten(row):
    """
    messages from mod_ml can have the capitalized apache envvars
    """
    if 'HTTP_HOST' in row:
        row['http_host'] = row['HTTP_HOST']

    if 'REMOTE_ADDR' in row:
        row['remote_addr'] = row['REMOTE_ADDR']
    return row

def getbotlatest(scur,ipd):
    try:
        scur.execute( 
            """ 
            select 
            hour,REMOTE_ADDR, status_line, useragent, epoch, HTTP_HOST, content_type 
            from botlatest 
            where 
            http_host=%(http_host)s 
            and remote_addr=%(remote_addr)s 
            """,ipd)
        if scur.rowcount > 0:
            return botlatest2dict(scur.fetchone())
        return None
    except:
        pass

def updatebotlatest(row, ucur, uconn):
    """ remember the last log entry we saw """
    try:
        row = flatten(row)
        ucur.execute(
            """
            update botlatest set 
                hour = %(hour)s, 
                status_line = %(status_line)s, 
                content_type = %(content_type)s,
                useragent = %(useragent)s, 
                epoch = %(epoch)s 
            where
                http_host = %(http_host)s and 
                remote_addr = %(remote_addr)s 
            """, row)
        if autocommit: uconn.commit()
    except Exception as e:
        uconn.rollback()
        print >>sys.stderr, "update botlatest error for ip ",row['remote_addr']
        print str(e)

def insertbotlatest(row, ucur, uconn):
    """
    create first botlatest entry for this ip
    botlatest in production would be cleared of old data periodically
    """
    try:
        row = flatten(row)
        ucur.execute(
            """
            insert into botlatest (
                hour, status_line, content_type, useragent,
                epoch, http_host, remote_addr
            )
            values (
                %(hour)s, %(status_line)s, %(content_type)s, %(useragent)s,
                %(epoch)s, %(http_host)s, %(remote_addr)s
            )
            """, row)
        if autocommit: uconn.commit()
    except Exception as e:
        uconn.rollback()
        print >>sys.stderr, "insert botlatest error for ip ",row['remote_addr']
        print str(e)

###############################################################################
# functions used by ippreprocess.py                                           #
###############################################################################
# see if we know about the user agent
# one feature is the number of different agents for an ip
# but we also want to keep track of uas as many are known to be for bots
# and we can use this information for training and testing classification
def checkua(ip, row, conn):
    cur = conn.cursor()
    ucur = conn.cursor()
    try:
        if 'useragent' not in row or row['useragent'] == None or row['useragent'] == '':
            raise Exception("Missing useragent for ip %s" % (ip))

        row['ip'] = ip
        cur.execute(
            """
            select * from botlabels where useragent = %(useragent)s
            """, row)
        rowcount = cur.rowcount

        if rowcount == 0:
            print >>sys.stderr,"saving ",row['useragent']," to botlabels"
            try:
                ucur.execute(
                    """
                    insert into botlabels (useragent) 
                    values (%(useragent)s)
                    """, row)
                conn.commit()
            except Exception as e:
                conn.rollback()
                print >>sys.stderr, str(e)


        cur.execute(
            """
            select * from botiplabels 
            where ip = %(ip)s 
            and useragent = %(useragent)s
            """, row) 
        rowcount = cur.rowcount

        if rowcount == 0:
            print >>sys.stderr,"saving ",row['useragent']," to botiplabels"
            try:
                ucur.execute(
                    """
                    insert into botiplabels 
                    (ip, HTTP_HOST, REMOTE_ADDR, useragent)
                    values 
                    (%(ip)s, %(HTTP_HOST)s, %(REMOTE_ADDR)s, %(useragent)s)
                    """, row)
                conn.commit()
            except Exception as e:
                conn.rollback()
                print >>sys.stderr,str(e)
            
    except Exception as e:
        print >>sys.stderr, "error saving useragent ",row['useragent']," for ip ",ip
        print >>sys.stderr, "message: ",str(e)
    finally:
        cur.close()
        ucur.close()

# when running this in production the processing is much too slow
# so instead log.py is used to save messages to a log table
# this function checks for log entries
def hasmsgs():
    conn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
    cur = conn.cursor()
    cur.execute("select count(*) from log where pid is null")
    count = cur.fetchone()
    cur.close()
    conn.close()
    if count[0] > 0: 
        return True
    return False

# if we have log entries read a bunch of them and process them
# the order of the messages must be cronological
# use pid to control which process accesses what 
# XXX: this probably won't work but its so slow its worth a try
def readmsgs(pid,howmany=1000):
    print "starting readmessages with pid ",pid
    conn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
    cur = conn.cursor()
    cur.execute(
        """
        update log set pid=%(pid)s where pid is null
        and logid <= 
        (select max(logid) from 
         (select logid from log 
          where pid is null 
          order by logid 
          limit %(howmany)s) lid)
        """, {'pid':pid,'howmany':howmany})
    conn.commit()
    cur.execute(
        """
        select logid, message 
        from log 
        where pid=%(pid)s 
        order by logid
        """,{'pid':pid})
    messages = []
    for msg in cur:
        messages.append(msg)
    logid = process(messages)
# temporarily turned this off to test 
# how the logged entries are processed
#    cur.execute(
#        """
#        delete from log 
#        where logid <= %(logid)s and pid=%(pid)s
#        """, {'logid':logid,'pid':pid})
#    conn.commit()
    cur.close()
    conn.close()

# not used by botlog.py but called from ippreprocess.py
def process(messages):
    """
    given a list of (logid, message) tuples 
    calculate time differences between pairs of 
    messages from the same remote_addr and
    calculate stats relating to the distributions
    of these time differences
    """

    global MAXDIFFS
    global OLD

    # for getting data on a host/ip pair
    conn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
    cur = conn.cursor()
    tcur = conn.cursor()

    # for updates - inserts
    uconn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
    ucur = uconn.cursor()


    logid = 0
    try:
        previp = ""

        for msg in messages:
            logid, message = msg
            row = json.loads(message)

            # this is a "harmless" administrative thing done by apache to manage child procs
            # mod_ml now filters these messages automatically
            if row['useragent'] == "Apache/2.4.7 (Ubuntu) PHP/5.5.9-1ubuntu4.11 (internal dummy connection)":
                print >>sys.stderr,"ignoring internal dummy connection"
                continue;
            
            # using host/ip as key as the ips were originally separately randomized
            # also have been using different cloudsmall hosts to represent different
            # types of input (e.g. cs7 handled all stuff from my robot scripts)
            ip = row['HTTP_HOST']+"/"+row['REMOTE_ADDR']

            if previp != ip:
                previp = ip
                # want to only do updates here as for a given host ips
                # show up in groups 
                # ideally run the preprocessor that calls this on each host
                

            checkua(ip=ip, row=row, conn=conn)

            pages = errs = 0
            reqs = 1

            # check the status_line for 4xx or 5xx messages
            if iserr(row):
                errs = 1

            # check content_type for text/html
            if ishtml(row):
                html = True
                pages = 1
            else:
                html = False

            # update our counts if they have been started
            # these counts would have to be cleared periodically
            # most likely the botlatest and botstats tables would be purged 
            # of old ip addresses from time to time
            cur.execute(
                """
                select pages,reqs,errs from botstats
                where ip=%(ip)s
                """, 
                {'ip':ip})

            if cur.rowcount > 0:
                ipcounts = cur.fetchone()
                pages += ipcounts[0]
                reqs += ipcounts[1]
                errs += ipcounts[2]
                updatestatcounts(reqs,pages,errs,ucur,uconn)

            else:
                ucur.execute(
                    """
                    insert into botstats (pages,reqs,errs,ip)
                    values (%(pages)s,%(reqs)s,%(errs)s,%(ip)s)
                    """,
                    {'pages':pages,'reqs':reqs,'errs':errs,'ip':ip})
                uconn.commit()

            if not html: 
                continue

            # see if we have a previous entry in the log
            # this is just for html pages
            # timing between html page requests is tracked in various ways
            # to see if its possible to detect robots - e.g. skew may be different
            cur.execute(
                    """
                    select 
                    hour,REMOTE_ADDR, status_line, useragent, epoch, HTTP_HOST, content_type
                    from botlatest 
                    where HTTP_HOST=%(HTTP_HOST)s 
                    and REMOTE_ADDR=%(REMOTE_ADDR)s
                    """, row)

            if cur.rowcount > 0:
                prev = botlatest2dict([v for v in cur.fetchone()])

                diff = int(row['epoch']) - int(prev['epoch'])

                # believe it or not this is a useful feature
                hourdiff = abs(int(row['hour']) - int(prev['hour']))

                # a negative diff means we are replaying an old log
                if diff < 0:
                    continue

                # ignore extreme outliers - in milliseconds 1200000 = 20 min
                # in a production setting this would correlate with when data gets removed
                if diff < OLD:

                    # get the lists we are using to calculate cumulative stats
                    cur.execute(
                        """
                        select diffs, hourdiffs, hours from botstats
                        where ip = %(ip)s
                        """,
                        { 'ip': ip })

                    if cur.rowcount == 0:
                        raise Exception("botstats insert failed for %s" % (ip))

                    diffrow = cur.fetchone()

                    # diffs and hourdiffs should be the same length
                    # so testing one will be adequate
                    if diffrow[0] == None:
                        diffs = []
                        hourdiffs = []
                        hours = [0 for h in range(24)]
                    else:
                        diffs = diffrow[0]
                        hourdiffs = diffrow[1]
                        hours = diffrow[2]

                    if len(diffs) >= MAXDIFFS:
                        del(diffs[0])

                    if len(hourdiffs) >= MAXDIFFS:
                        del(hourdiffs[0])

                    diffs.append(diff)
                    hourdiffs.append(hourdiff)
                    hours[int(row['hour'])] += 1
                    updatestats(diffs,hourdiffs,hours,ucur,uconn)

                updatebotlatest(row, ucur, uconn)

            else: # check for a previous botlatest entry failed, insert a new one

                insertbotlatest(row, ucur, uconn)

    except Exception as e:
        # from http://stackoverflow.com/questions/14519177/python-exception-handling-line-number
        exc_type, exc_obj, tb = sys.exc_info()
        print >>sys.stderr, "at line ",tb.tb_lineno," error: ",str(e)," message: ",message

    finally:
        ucur.close()
        cur.close()
        conn.close()
        uconn.close()
        return logid

if __name__ == '__main__':
    readmsgs(int(sys.argv[1]))
    # process([sys.argv[1]])
