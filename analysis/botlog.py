"""
 this module uses functions from the older botupdstats module 
 and is used by botlogger which is a preprocessor for mod_ml

 botlogger feeds this module messages via the process function below
 the process function simply puts messages from mod_ml into the botlog table
 then alerts the processing threads via the poke function
 the processing threads keep working as long as possible then
 wake up the cleanup thread which checks to see if there is left over
 stuff in botlog
 
 the basic idea is to have many threads each of which takes care of 
 the history for a single ip all at the same time
 they handle many ips in parallel
 note that the db considers an "ip" to be "http_host/remote_addr"
 this is specific to the script used to replay existing logs for testing
 messages are read oldest to newest 
 out of order messages are ignored

 as with botlogger this module can be adapted to do other tasks
 to change what happens to a specific message for a specific ip
 use a different function than readlog - in reality that should be 
 in its own module
"""
import logging
# note that if included from another script the logging.basicConfig may be ignored
# logging.basicConfig(level=logging.ERROR, 
logging.basicConfig(level=logging.DEBUG,
        format="%(levelname)s:(%(threadName)-10s) (%(funcName)-10s) %(message)s") 

import psycopg2
from mldb import dbname, dbuser, dbpw
import json
import sys
import time
import threading
import botupdstats as upd
import copy

# used for resetting the db connection if its died
lock = threading.Lock()
# used for cleaning up myips
iplock = threading.Lock()

# tell a thread to wake up and do something because poke has found work for it
condition = threading.Condition()

# tell cleanup to wake up and do something because a thread is idle
cleanupcond = threading.Condition()
# "data" that gets manipulated as part of cleanupcond processing
notbusy = True

# total processing threads to start 
# trying for about 10 ips per thread - see query in cleanup below
THREADCOUNT = 512
# next thread to assign an ip to
nextt = -1
# thred array
t = []
# who is looking after what IP
myips = {}
ipprocs = {}
# db connection for internal processing
conn = psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
# db connection for process func - fills botlog table
proconn = psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)

def mkip(parsed):
    ip = "%s/%s" % (parsed['HTTP_HOST'], parsed['REMOTE_ADDR'])
    return ip

def connect(prevconn=None):
    """ connect to db, either with a handle that had been defined before or make a new one """
    if prevconn != None:
        if prevconn.closed != 0:
            lock.acquire()
            logging.debug("reconnecting prevconn")
            prevconn = psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
            lock.release()
        return prevconn
    lock.acquire()
    logging.debug("connecting")
    conn = psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
    lock.release()
    return conn

def readmsgs():
    """
    thread process that wakes up and processes log entries
    handles the thread signalling part of the process
    """
    global myips
    global lock
    global condition
    global cleanupcond
    global notbusy
    global iplock
    global ipprocs

    logging.debug("starting processor")

    name = threading.current_thread().getName()
    MAXBACKOFF = 64
    conn = connect()

    while True:
        condition.acquire()
        backoff = 0.5
        mywork = 0
        while mywork == 0:
            mywork = 0
            logging.debug("trying to find my work")
            if name in myips:
                try:
                    conn = connect(conn)
                    cur = conn.cursor()
                    for ip, ipdata in myips[name].iteritems():
                        logging.debug("looking for ip %s" % ip)
                        cur.execute(
                            """
                            select count(*) from botlog 
                            where remote_addr=%(remote_addr)s and http_host=%(http_host)s
                            """, ipdata)
                        count = cur.fetchone()
                        mywork += count[0]
                        if mywork > 0: break

                except Exception as e:
                    logging.error(e)
                    conn.rollback()
                finally:
                    cur.close()
            if backoff <= MAXBACKOFF:
                backoff *= 2
            logging.debug("waiting backoff %d" % backoff)
            condition.wait(backoff)
            if mywork == 0:
                cleanupcond.acquire()
                notbusy = True
                cleanupcond.notify()
                cleanupcond.release()

        if name in myips:
            localips = copy.deepcopy(myips[name])
        else:
            localips = []
        condition.release()

        logcount = 1
        while logcount != 0:
            logcount = 0
            try:
                conn = connect(conn)
                for ip, whohas in localips.iteritems():
                    count = upd.processlog(whohas, 0, conn)
                    if count != None:
                        logcount += count
            except Exception as e:
                logging.error(e)
            
        iplock.acquire()
        if name in myips:
            logging.debug("carefully deleting my ips from global store")
            if name in myips:
                for ip, whohas in localips.iteritems():
                    if ip in myips[name]:
                        del(ipprocs[ip])
                        del(myips[name][ip])
                if len(myips[name]) == 0:
                    del(myips[name])
        iplock.release()

        cleanupcond.acquire()
        notbusy = True
        cleanupcond.notify()
        cleanupcond.release()

    logging.debug("exiting")


def startthreads():
    """
    start log processors
    """
    global t
    global conditions
    global THREADCOUNT
    try:
        for i in range(THREADCOUNT):
            t.append(threading.Thread(
                name="proc%d" % (i),
                target=readmsgs))
            t[i].start()
    except Exception as e:
        logging.error("startthreads error: %s" % (str(e)))

def set_ipd(ip,host,addr,thread=""):
    return {
        'ip':ip, 
        'http_host': host,
        'remote_addr': addr,
        'thread': thread
    }

def poke(newips):
    """
    scan through a list of ips we've seen 
    that we think need updating
    signal to the other threads to check them out
    """
    global nextt
    global THREADCOUNT
    global t
    global myips
    global ipprocs
    global condition
    global notbusy

    try:
        condition.acquire()
        logging.debug("adding some new ips")
        for ip, ipdata in newips.iteritems():
            if ip not in ipprocs:
                ipdata['thread'] = t[nextt].name
                nextt += 1
                if nextt >= THREADCOUNT:
                    nextt = 0
                if ipdata['thread'] not in myips:
                    myips[ipdata['thread']] = {}
                ipprocs[ip] = ipdata['thread']
                myips[ipdata['thread']][ip] = ipdata
                logging.debug("thread %s now associated with ip %s" % (ipdata['thread'], ip))

        notbusy = False
        condition.notify_all()
        condition.release()

    except Exception as e:
        exc_type, exc_obj, tb = sys.exc_info()
        logging.error("failed at %d: query %s exception %s" 
                % (tb.tb_lineno, cur.query, str(e)))

def process(messages):
    """ 
    reads a list of messages from a mod_ml preprocessor
    farms out work to threads
    also initiates threads if they haven't been started yet
    """
    global t
    global proconn
    try:

        if len(t) == 0:
            startthreads()

        logging.debug("botlog connecting to database")
        proconn = connect(proconn)
        cur = proconn.cursor()
        logging.debug("botlog connected to database")

        newips = {}
        for message in messages:
            try:
                parsed = json.loads(message)
            except:
                continue
            if parsed['hour'] == '': parsed['hour'] = 0
            logging.debug(parsed)
            try:
                cur.execute(
                """insert into botlog 
                   (hour,REMOTE_ADDR, status_line, useragent, epoch, HTTP_HOST, content_type) 
                   values 
                   (%(hour)s, %(REMOTE_ADDR)s, %(status_line)s, %(useragent)s, %(epoch)s, 
                    %(HTTP_HOST)s, %(content_type)s)""", 
                   parsed)
                proconn.commit()
            except:
                proconn.rollback()
            ip = mkip(parsed)
            newips[ip] = set_ipd(ip, parsed['HTTP_HOST'], parsed['REMOTE_ADDR'])

        logging.debug("got some ips %s" % newips)
        poke(newips)

    except Exception as e:
        proconn.rollback()
        exc_type, exc_obj, tb = sys.exc_info()
        logging.error("failed at %d: message %s query %s exception %s" 
                % (tb.tb_lineno, message.strip(), cur.query, str(e)))

def cleanup():
    """
    force a check of new stuff from any ip independent of botlogger
    some stuff may have been missed from last time
    """
    global cleanupcond
    global notbusy

    logging.debug("cleanup starting")
    conn = connect()

    while True:
        if len(t) == 0:
            startthreads()

        newips = {}
        try:
            logging.debug("cleanup checking botlog (connecting)")
            conn = connect(conn)
            logging.debug("cleanup checking botlog (select)")
            cur = conn.cursor()
            # 5000 seems to be the limit for doing the query quickly on cs8
            cur.execute("select distinct HTTP_HOST,REMOTE_ADDR from botlog limit 5120")
            logging.debug("cleanup checking botlog (scan)")
            for row in cur:
                parsed = {'HTTP_HOST':row[0],'REMOTE_ADDR':row[1]}
                ip = mkip(parsed)
                if ip not in newips:
                    newips[ip] = set_ipd(ip, parsed['HTTP_HOST'], parsed['REMOTE_ADDR'])

            if len(newips) > 0:
                logging.debug("cleanup checking botlog (poke)")
                poke(newips)

        except Exception as e:
            exc_type, exc_obj, tb = sys.exc_info()
            logging.error("cleanup failed at %d: query %s exception %s" 
                            % (tb.tb_lineno, cur.query, str(e)))

        logging.debug("cleanup waiting")
        cleanupcond.acquire()
        while notbusy == False:
            cleanupcond.wait(60)
        notbusy = False
        cleanupcond.release()

cleant = threading.Thread(name="cleanup", target=cleanup)
cleant.start()

