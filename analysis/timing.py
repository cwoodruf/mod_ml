#!/usr/bin/env python
# get timing information on requests based on ip address
import psycopg2
import re
from mldb import dbname, dbuser, dbpw

conn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
cur = conn.cursor()
tcur = conn.cursor()
# for updates - inserts
uconn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
ucur = uconn.cursor()
cur.execute("select * from botlog_archive order by http_host, remote_addr, epoch") # limit 1000") 
ipprev = {}
ipstats = {}
ipcounts = {}
ipdiffs = {}
iphourdiffs = {}
iphours = {}
MAXDIFFS = 1000

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

def iserr(row):
    status_line = row['status_line']
    if status_line[0] == '4' or status_line[0] == '5':
        return True
    return False

def ishtml(row):
    # for these logs this works
    # however in general use content_type = text/html for streaming data
    # ishtml = re.compile('^/$|^/index.php')
    content_type = row['content_type']
    if content_type == 'text/html': 
        return True
    return False

# example row:
# 0 blid          5547567, 
# 1 hour          0, 
# 2 remote_addr   '101.142.15.16', 
# 3 status_line   '200', 
# 4 useragent     'Mozilla/5.0 (Linux; U; Android 2.3.6; ...
# 5 epoch         1391415125L, 
# 6 request_uri   '/', 
# 7 http_host     '206.12.16.198:8898'
# 8 content_type  'text/html'

def row2dict(row):
    keys = ['blid', 'hour', 'remote_addr', 'status_line', 'useragent', 'epoch', 'request_uri', 'http_host', 'content_type']
    return dict((keys[i], item) for i,item in enumerate(row))

# ignore anything over 2 days old - too extreme?
OLD = 2 * 86400 * 1000

for rowlist in cur:
    row = row2dict(rowlist)

    ip = row['http_host']+"/"+row['remote_addr']

    if ip not in ipcounts:
        ipcounts[ip] = {}
        ipcounts[ip]['reqs'] = 1
        ipcounts[ip]['pages'] = 0
        ipcounts[ip]['errs'] = 0
    else:
        ipcounts[ip]['reqs'] += 1

    if iserr(row):
        ipcounts[ip]['errs'] += 1

    if ishtml(row):
        ipcounts[ip]['pages'] += 1
    else:
        continue

    if ip in ipprev:
        prev = ipprev[ip]
        diff = row['epoch'] - prev['epoch']

        # believe it or not this is a useful feature
        hourdiff = abs(row['hour'] - prev['hour'])

        if ip not in ipdiffs:
            ipdiffs[ip] = []
            iphours[ip] = [0 for i in range(24)]
            iphourdiffs[ip] = []

        # ignore extreme outliers - in milliseconds 1200000 = 20 min
        # in a production setting this would determine when data gets removed
        if diff >= 0 and diff < OLD:

            if len(ipdiffs[ip]) >= MAXDIFFS:
                del(ipdiffs[ip][0])
                del(iphourdiffs[ip][0])

            ipdiffs[ip].append(diff)
            iphourdiffs[ip].append(hourdiff)
            iphours[ip][row['hour']] += 1

            if ip in ipdiffs and len(ipdiffs[ip]) > 1:
                s = genstats(ipdiffs[ip])
                h = genstats(iphourdiffs[ip])
                ht = genstats(iphours[ip])

                if h == None or s == None or ht is None:
                    raise Exception("missing data for ip "+ip)

                pages = ipcounts[ip]['pages']
                reqs = ipcounts[ip]['reqs']
                errs = ipcounts[ip]['errs']

                data = {'ip':ip,
                        'n':s['n'],'sum':s['sum'],'mean':s['mean'],
                        'var':s['var'],'skew':s['skew'],'kurtosis':s['kurtosis'],
                        'diffs':s['diffs'],'pages':pages,'reqs':reqs,'hourdiffs':h['diffs'],
                        'hn':h['n'],'hsum':h['sum'],'hmean':h['mean'],
                        'hvar':h['var'],'hskew':h['skew'],'hkurtosis':h['kurtosis'],
                        'errs': errs, 
                        'hours':ht['diffs'], # these aren't actual diffs
                        'htn':ht['n'],'htsum':ht['sum'],'htmean':ht['mean'],
                        'htvar':ht['var'],'htskew':ht['skew'],'htkurtosis':ht['kurtosis']
                        }

                print data
                try:
                    ins = ucur.mogrify("""insert into botstats_archive (
                                        ip,
                                        n,sum,mean,var,skew,kurtosis,
                                        diffs,pages,reqs,hourdiffs,
                                        hn,hsum,hmean,hvar,hskew,hkurtosis,
                                        errs,
                                        hours,
                                        htn,htsum,htmean,htvar,htskew,htkurtosis
                                        ) 
                                    values (
                                        %(ip)s,
                                        %(n)s,%(sum)s,%(mean)s,%(var)s,%(skew)s,%(kurtosis)s,
                                        %(diffs)s,%(pages)s,%(reqs)s,%(hourdiffs)s,
                                        %(hn)s,%(hsum)s,%(hmean)s,%(hvar)s,%(hskew)s,%(hkurtosis)s,
                                        %(errs)s,
                                        %(hours)s,
                                        %(htn)s,%(htsum)s,%(htmean)s,%(htvar)s,%(htskew)s,%(htkurtosis)s
                                        )
                                        """, data)
                    # print ins
                    ucur.execute(ins)
                    print "committing data for ip ",ip
                    uconn.commit()
                    print ucur.statusmessage
                except Exception as e:
                    print "error inserting or may already have data for ip ",ip
                    print str(e)
                    uconn.rollback()
                    try:
                        ucur.execute("""update botstats_archive set 
                                        n=%(n)s,sum=%(sum)s,mean=%(mean)s,var=%(var)s,
                                        skew=%(skew)s,kurtosis=%(kurtosis)s,
                                        diffs=%(diffs)s,pages=%(pages)s,reqs=%(reqs)s,hourdiffs=%(hourdiffs)s,
                                        hn=%(hn)s,hsum=%(hsum)s,hmean=%(hmean)s,hvar=%(hvar)s,
                                        hskew=%(hskew)s,hkurtosis=%(hkurtosis)s,
                                        errs=%(errs)s,
                                        hours=%(hours)s,
                                        htn=%(htn)s,htsum=%(htsum)s,htmean=%(htmean)s,htvar=%(htvar)s,
                                        htskew=%(htskew)s,htkurtosis=%(htkurtosis)s
                                        where ip=%(ip)s""", data)
                        print "committing data for ip ",ip
                        uconn.commit()
                        # print ucur.query
                        print ucur.statusmessage
                    except Exception as e:
                        print "update error for ip ",ip
                        print str(e)

                finally:
                    print "checking commit for ip ",ip
                    tcur.execute("select * from botstats_archive where ip=%(ip)s", {'ip':ip})
                    for trow in tcur:
                        print trow
                    print "finished check"

    ipprev[ip] = row

