#!/usr/bin/env python
# the differences between increasing diffs are too big
# because "latest" was not correctly reset in botupdstats.py
# the diffs and hourdiffs will follow a pattern of N, N+d1, N+d1+d2 ...
# this can be fixed by looking for diffs with this pattern and fixing diffs and hourdiffs

import psycopg2
from mldb import dbname, dbuser, dbpw
import bottiming
bottiming.autocommit = False
import sys
import re

select = """
        select ip, diffs, hourdiffs, hours
        from botstats
        where ip not like 'archive-%'
        and ip like '#host#%'
        and mean is not null and array_length(diffs,1) > 3
        order by ip, array_length(diffs,1) desc
        """
if len(sys.argv) < 2:
    print "need a host name to search for!"
    sys.exit(1)

print "searching for host ",sys.argv[1]
query = re.sub(r'#host#', sys.argv[1], select)
print query
try:
    rowcount = 0
    conn = psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
    cur = conn.cursor()
    ucur = conn.cursor()

    cur.execute(query)
    for row in cur:
        print row
        ip, diffs, hourdiffs, hours = row
        fixed = []
        hfixed = []
        prev = None
        hprev = None
        prevprev = None
        hprevprev = None
        for i in range(len(diffs)):
            diff = diffs[i]
            hdiff = hourdiffs[i]
            if i > 2:
                pdiff = diff - prevprev
                ppdiff = prev - prevprev
                # must be monotonically increasing
                if pdiff >= 0 and ppdiff >= 0 and pdiff >= ppdiff:
                    fixed[i-1] = ppdiff
                    fixed.append(diff - prev)
                    hfixed[i-1] = abs(hprev-hprevprev)
                    hfixed.append(abs(hdiff-hprev))
                else:
                    fixed.append(diff)
                    hfixed.append(hdiff)
            else:
                fixed.append(diff)
                hfixed.append(hdiff)
                    
            prevprev = prev
            prev = diff
            hprevprev = hprev
            hprev = hdiff
        print (ip, fixed, hfixed)
        bottiming.updatestats(ip,fixed,hfixed,hours,ucur,conn)
        rowcount += 1
        if rowcount > 100:
            rowcount = 0
            conn.commit()

except Exception as e:
    conn.rollback()
    print str(e)
