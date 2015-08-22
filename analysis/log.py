# this does a simple insert of a message into mod_ml's db
import psycopg2
from mldb import dbname, dbuser, dbpw
import sys

def process(messages):
    try:
        print >>sys.stderr, "mod_ml connecting to database"
        conn=psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
        cur=conn.cursor()
        for message in messages:
            cur.execute("insert into log (ts, message) values (now(), %(message)s)", 
                    {'message':message.strip()});
            conn.commit()
    except Exception as e:
        print >>sys.stderr, str(e)
        print >>sys.stderr, "failed: message ",message.strip()," query ",cur.query
    finally:
        cur.close()
        conn.close()

