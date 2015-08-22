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
# only look for the population project stuff
cur.execute(
    """
    select * from botlog 
    where http_host = '206.12.16.219:8898' 
    order by remote_addr, epoch desc
    """) # limit 10000""") 
ipprev = {}
ipstats = {}
ipcounts = {}
ipdiffs = {}
iphourdiffs = {}
MAXDIFFS = 1000

def genstats(ipdiffs):
    # this method is going to be slow but my kurtosis calculation formula isn't working
    if ip in ipdiffs and len(ipdiffs[ip]) > 1:
        diffs = ipdiffs[ip]
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

# example row:
# 0 blid          5547567, 
# 1 hour          0, 
# 2 remote_addr   '101.142.15.16', 
# 3 status_line   '200', 
# 4 useragent     'Mozilla/5.0 (Linux; U; Android 2.3.6; ...
# 5 epoch         1391415125L, 
# 6 request_uri   '/', 
# 7 http_host     '206.12.16.198:8898'

# for these sites this works
# however in general it might be more difficult
# ishtml = re.compile('^/$|^/index.php')
# for population project html pages end in a number or number.html
ishtml = re.compile('.*/\d+(?:\.html|)$')

# ignore anything over 2 days old - may be too extreme?
OLD = 2 * 86400 * 1000

for row in cur:
    ip = row[7]+"/"+row[2]
    request_uri = row[6]
    if ip not in ipcounts:
        ipcounts[ip] = {}
        ipcounts[ip]['reqs'] = 1
        ipcounts[ip]['pages'] = 0
    else:
        ipcounts[ip]['reqs'] += 1

    if ishtml.match(request_uri):
        ipcounts[ip]['pages'] += 1
    else:
        continue

    if ip in ipprev:
        prev = ipprev[ip]
        # negative diffs mean a name collision
        diff = prev[5] - row[5]
        # but for hours they are possible
        hourdiff = abs(prev[1] - row[1])

        if ip not in ipdiffs:
            ipdiffs[ip] = []
            iphourdiffs[ip] = []

        # ignore extreme outliers - in milliseconds 1200000 = 20 min
        # in a production setting this would determine when data gets removed
        if diff >= 0 and diff < OLD:

            if len(ipdiffs[ip]) >= MAXDIFFS:
                del(ipdiffs[ip][0])
                del(iphourdiffs[ip][0])

            ipdiffs[ip].append(diff)
            iphourdiffs[ip].append(hourdiff)

            if ip in ipdiffs and len(ipdiffs[ip]) > 1:
                s = genstats(ipdiffs)
                h = genstats(iphourdiffs)
                if h == None or s == None:
                    raise Exception("missing data for ip "+ip)
                pages = ipcounts[ip]['pages']
                reqs = ipcounts[ip]['reqs']
                data = {'ip':ip,
                        'n':s['n'],'sum':s['sum'],'mean':s['mean'],
                        'var':s['var'],'skew':s['skew'],'kurtosis':s['kurtosis'],
                        'diffs':s['diffs'],'pages':pages,'reqs':reqs,'hourdiffs':h['diffs'],
                        'hn':h['n'],'hsum':h['sum'],'hmean':h['mean'],
                        'hvar':h['var'],'hskew':h['skew'],'hkurtosis':h['kurtosis']
                        }

                print data
                try:
                    ins = ucur.mogrify("""insert into botstats (
                                        ip,
                                        n,sum,mean,var,skew,kurtosis,
                                        diffs,pages,reqs,hourdiffs,
                                        hn,hsum,hmean,hvar,hskew,hkurtosis
                                        ) 
                                    values (
                                        %(ip)s,
                                        %(n)s,%(sum)s,%(mean)s,%(var)s,%(skew)s,%(kurtosis)s,
                                        %(diffs)s,%(pages)s,%(reqs)s,%(hourdiffs)s,
                                        %(hn)s,%(hsum)s,%(hmean)s,%(hvar)s,%(hskew)s,%(hkurtosis)s
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
                        ucur.execute("""update botstats set 
                                        n=%(n)s,sum=%(sum)s,mean=%(mean)s,var=%(var)s,skew=%(skew)s,kurtosis=%(kurtosis)s,
                                        diffs=%(diffs)s,pages=%(pages)s,reqs=%(reqs)s,hourdiffs=%(hourdiffs)s,
                                        hn=%(hn)s,hsum=%(hsum)s,hmean=%(hmean)s,hvar=%(hvar)s,hskew=%(hskew)s,hkurtosis=%(hkurtosis)s
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
                    tcur.execute("select * from botstats where ip=%(ip)s", {'ip':ip})
                    for trow in tcur:
                        print trow
                    print "finished check"

    ipprev[ip] = row

