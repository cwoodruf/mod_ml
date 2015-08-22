#!/usr/bin/env python
# see if the t score is significant
import psycopg2
from mldb import dbname, dbuser, dbpw
from math import sqrt
import sys
import re
import logging

what = sys.argv[1]
print "checking t score for ",what
query1 = """
select 
    class,
    sum(%(what)s)/count(*) av,
    (sum(%(what)s*%(what)s)+sum(%(what)s)*sum(%(what)s)/count(*))/(count(*) -1) s2, 
    count(*) N,
    min(%(what)s),
    max(%(what)s)
from botstats 
where 
    mean is not null 
    and class in (-1,1)
group by class
"""
query2 = """
select 
    class,
    sum(%(what)s - %(mean)s),
    sum(%(what)s - %(mean)s)^3 as s3,
    sum(%(what)s - %(mean)s)^4 as s4
from botstats 
where 
    %(what)s is not null 
    and class in (%(class)s)
group by class
"""
query1 = re.sub(r'%\(what\)s', what, query1)
try:
    # print "trying to connect"
    conn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
    cur = conn.cursor()
    # print "executing query ",query1
    cur.execute(query1)
    stats = []
    for row in cur:
        print row
        cl, m, s2, N, min_, max_ = row
        stats.append({ 'class': cl, 'm': m, 's2': s2, 'N': N })
    t = (stats[0]['m'] - stats[1]['m'])/sqrt(stats[0]['s2']/stats[0]['N']+stats[1]['s2']/stats[1]['N'])
    print "t (s2s/N) score ",t
    t2 = (stats[0]['m'] - stats[1]['m'])/sqrt(((stats[0]['N']*stats[0]['s2'])+(stats[1]['N']*stats[1]['s2']))/(stats[0]['N']+stats[1]['N']))
    print "t (s2s/2) score ",t2
    for i in [0,1]:
        query = query2
        query = re.sub(r'%\(what\)s', what, query)
        query = re.sub(r'%\(mean\)s', str(stats[i]['m']), query)
        query = re.sub(r'%\(class\)s', str(stats[i]['class']), query)
        # print "executing query ",query
        cur.execute(query)
        row = cur.fetchone()
        N = stats[i]['N']
        s2 = stats[i]['s2']
        s3 = row[3]
        s4 = row[3]
        c1 = (N*(N+1.0))/((N-1.0)*(N-2.0)*(N-3.0))
        c2 = ((N-1.0)**2.0)/((N-2.0)*(N-3.0))
        kurtosis = c1*(s4/(s2**2))-3*c2
        print "kurtosis for ",row[0]," is ",kurtosis
        sN = ((N*(N-1.0))**(1/2))/(N-1.0)
        skew = sN*(s3/N)/(s2*(s2**(1.0/2.0)))
        print "skew for ",row[0]," is ",skew
        # jarque-bera test of normality
        # see https://en.wikipedia.org/wiki/Jarque%E2%80%93Bera_test
        jb = ((N-1.0)/6.0)*(skew**2.0+0.25*(kurtosis)**2)
        print "distribution has Jarque-Bera score of ",jb," on a sample of ",N


except Exception as e:
    exc_type, exc_obj, tb = sys.exc_info()
    logging.error("cleanup failed at %d: query %s exception %s"
                    % (tb.tb_lineno, cur.query, str(e)))



        
