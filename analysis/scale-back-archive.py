#!/usr/bin/env python
# the archived epochs are millisecond based not second based
# change the contents of botstats to reflect second based epoch time
import psycopg2
import bottiming as bt
from mldb import dbname, dbuser, dbpw

conn = psycopg2.connect(dbname=dbname,user=dbuser,password=dbpw)
cur = conn.cursor()
ucur = conn.cursor()
"""
 n          | integer                  | 
  sum        | bigint                   | 
   mean       | double precision         | 
    var        | double precision         | 
     skew       | double precision         | 
      kurtosis   | double precision         | 
       diffs      | integer[]                | 
"""

cur.execute(
    """
    select ip,diffs,hourdiffs,hours 
    from botstats
    where ip like 'archive-%'
    and mean is not null
    order by mean desc
    """)

for row in cur:
    ip, diffs, hourdiffs, hours = row
    scaleddiffs = [x/1000 for x in diffs]
    print ip," ",diffs," now ",scaleddiffs
    bt.updatestats(ip, scaleddiffs, hourdiffs, hours, ucur, conn)

