import psycopg2
import logging
import bottiming as bt
import sys
import threading

"""
 this module works with botlog to update stats in 
 a database of web requests
 the structure of the db is: 
 
 botlog - source table of request information

 botlatest - remembers last seen log entry for an ip

 botstats - collected stats for that ip 
            this includes 4 arrays:
                diffs - difference in epoch request time from prev entry
                hourdiffs - same but by hour of the day
                hours - distribution of requests by hour of day
                uas - list of last 10 user agents seen
            (the diffs are only for text/html pages)

            each array also holds various statistics:
                n (hn, ...) - count of items in array
                sum (hsum, ...) - total of items
                mean
                var - sample variance 
                skew - sample symmetricality
                kurtosis - flatness of sample

            in addition to these stats there are:
                reqs - total number of requests seen
                pages - how many were text/html
                errs - how many were 4xx or 5xx errors
                class - 1 = bot, -1 = human (based on 1st user agent)
                label - type of user agent for 1st user agent in array
                sample - this is only filled if the item was recently used
                         to build a learning model
"""
# latestlock = threading.Lock()
# statslock = threading.Lock()

def processlog(ipd,lastlogid,conn,vw=True):
    """ 
    read log entries for a specific ip and do stats 
    this should be considered a critical section
    needs to be called from readmsgs
    """
    global statslock
    try:
        cur = conn.cursor()
        lcur = conn.cursor()
        scur = conn.cursor()

        if lastlogid != None:
            ipd['lastlogid'] = lastlogid
        else:
            ipd['lastlogid'] = 0
        try:
            cur.execute(
                """
                select 
                hour,REMOTE_ADDR, status_line, useragent, epoch, HTTP_HOST, content_type,logid
                from botlog 
                where http_host=%(http_host)s 
                    and remote_addr=%(remote_addr)s 
                    and logid > %(lastlogid)s
                    order by logid
                """,ipd)
            if cur.rowcount == 0:
                if verbose:
                    logging.debug("found nothing to process for %s %s" % (str(ipd),cur.query))
                return 0
        except:
            return 0

        # get our stats row or make one
        isrc = True
        # statslock.acquire()
        stats = bt.getstats(scur,ipd)
        if stats == None:
            logging.debug("no stats yet")
            isrc = bt.initstats(scur, conn, ipd)
            stats = bt.getstats(scur, ipd)
        else:
            logging.debug("found stats")
        # statslock.release()
        if not isrc:
            raise Exception("error initializing stats for %s" % (ipd['ip']))
        if stats == None:
            raise Exception("Stats should not be empty here!")

        # go through the list of entries for us and update diffs
        update = False
        diff = -1
        log = {}
        logcount = 0

        # keep a unique list of user agents
        if 'uas' in stats and stats['uas'] != None:
            uas = set(stats['uas'])
        else:
            uas = set([])

        latest = bt.getbotlatest(lcur, ipd)

        for loglist in cur:
            log = bt.botlog2dict(loglist)
            logging.debug("next entry ... %s" % log)

            if len(uas) >= 10:
                tmpuas = list(uas)
                del(tmpuas[0])
                uas = set(tmpuas)
            uas.add(log['useragent'])

            stats['reqs'] += 1
            if bt.iserr(log):
                stats['errs'] += 1
            if bt.ishtml(log):
                stats['pages'] += 1
            else:
                continue

            if latest == None:
                latest = log
                bt.insertbotlatest(latest, lcur, conn)

            diff = log['epoch'] - latest['epoch']
            if diff < 0: continue

            latest = log

            if len(stats['diffs']) > bt.MAXDIFFS:
                del(stats['diffs'][0])
            stats['diffs'].append(diff)

            hourdiff = abs(log['hour'] - latest['hour'])
            if len(stats['hourdiffs']) > bt.MAXDIFFS: 
                del(stats['hourdiffs'][0])
            stats['hourdiffs'].append(hourdiff)

            stats['hours'][log['hour']] += 1
            logcount += 1

        stats['uas'] = list(uas);
        logging.debug("saving stats")
        logging.debug(stats)
        bt.updatestatcounts(
            ipd['ip'],
            stats['reqs'],
            stats['pages'],
            stats['errs'],
            stats['uas'],
            scur, conn)

        if len(stats['diffs']) > 0 and logcount > 0:
            bt.updatestats(
                ipd['ip'],
                stats['diffs'],
                stats['hourdiffs'],
                stats['hours'],
                scur, conn,
                stats,vw)

        # delete what we have seen
        # note that more entries may have been added
        # while we were processing
        logging.debug("last entry %s" % log)
        bt.updatebotlatest(log, lcur, conn)
        logging.debug(
            "deleting log entries for %(http_host)s %(remote_addr)s before %(logid)d" 
            % log)
        try:
            cur.execute(
                """
                delete from botlog 
                where 
                remote_addr=%(remote_addr)s 
                and http_host=%(http_host)s
                and logid <= %(logid)s
                """,log);
            conn.commit()
            logging.debug("deleted %d" % (cur.rowcount))
        except Exception as e:
            conn.rollback()
            logging.error("Failed to delete: %s" % e)

        return logcount

    except Exception as e:
        conn.rollback()
        # from http://stackoverflow.com/questions/14519177/python-exception-handling-line-number
        exc_type, exc_obj, tb = sys.exc_info()
        logging.error("processlog failed at %d: %s" % (tb.tb_lineno, str(e)))

