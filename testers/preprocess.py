#!/usr/bin/env python
import psycopg2
import sys
import os
import re
import socket
import json
import hashlib
from mldb import dbname, dbuser, dbpw

hostfile = "/srv/cal/src/apache/ml/testers/hosts"
whitelist = {} 

def read_whitelist():
    global hostfile
    global whitelist
    try:
        split = re.compile("\s*(\S+)\s*(.*)")
        comment = re.compile("^\s*#")
        isnonce = re.compile("(nonce)=(\"[^\"]*\"|'[^']*'|\S*)")
        wl = open(hostfile)
        for line in wl:
            lm = split.match(line)
            if lm == None: 
                continue
            bits = lm.groups()
            if comment.match(bits[0]): 
                continue

            yes = True
            if len(bits) > 1:
                m = isnonce.match(bits[1])
                if m != None:
                    authtype, pw = m.groups()
                    yes = hashlib.sha1(pw).hexdigest()

            whitelist[bits[0]] = yes
        wl.close()
        print "whitelist ",whitelist

    except Exception as e:
        print >>sys.stderr, str(e)
        print >>sys.stderr, "failed reading whitelist ",hostfile," at line ",line
        
read_whitelist()

noncepat = re.compile("nonce=(\w*):(\w*)")

try:
    if len(sys.argv) < 2: raise Exception("need a port number!")
    port = int(sys.argv[1]);
    if port < 1024: raise Exception("bad port number!")

    print >>sys.stderr, "mod_ml listening on ", port
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_address = ('',int(sys.argv[1]))
    sock.bind(server_address)
    sock.listen(1)

    while True: # this loop accepts a connection
        print >>sys.stderr, "waiting for connection"
        stream, client_address = sock.accept()

        try:
            if len(whitelist) == 0:
                pass
            elif client_address[0] in whitelist and whitelist[client_address[0]]:
                pass
            else:
                print >>sys.stderr, "rejected connection from ",client_address[0]," whitelist ",whitelist
                continue
        except Exception as e:
            print >>sys.stderr, str(e)
            print >>sys.stderr, "error checking whitelist for ",client_address[0]," whitelist ",whitelist
            continue

        try:
            newpid = os.fork()
            if newpid != 0:
                deadkid, kstatus = os.wait()
                print "pids were: parent ", os.getpid()," child ",deadkid," status ",kstatus
            else:
                # see http://www.python-course.eu/forking.php
                print >>sys.stderr, "child pid: ",os.getpid()," client: ",client_address
                message = ""
                while True: # this loop reads a message
                    try:
                        data = stream.recv(1024)
                        print >>sys.stderr, "got ",data
                        if data:
                           message = message + data
                           if "\n\n" in data:
                               stream.sendall("OK");
                        else:
                            break
                    except:
                        break
                # now update the db with the message
                try: 
                    print >>sys.stderr,"checking ",client_address[0]," against whitelist ",whitelist
                    if client_address[0] in whitelist:
                        print >>sys.stderr,"looking for password"
                        if whitelist[client_address[0]] == True:
                            pass
                        else:
                            sha1pw = whitelist[client_address[0]]
                            print >>sys.stderr,"found encoded pw ",sha1pw
                            m = noncepat.search(message)
                            if m != None:
                                nonce, salt = m.groups()
                                print >>sys.stderr, "got nonce ",nonce," and salt ",salt," from ",client_address
                                testnonce = hashlib.sha1(sha1pw+salt).hexdigest()
                                print >>sys.stderr,"nonce ",nonce," salt ",salt," testnonce ",testnonce
                                if testnonce != nonce:
                                    print >>sys.stderr, "nonce failed for ",client_address
                                    continue
                                else:
                                    print >>sys.stderr, "nonce ok for ",client_address
                            else:
                                print >>sys.stderr, "no nonce found in message, rejecting"
                                continue

                    print >>sys.stderr, "mod_ml connecting to database"
                    conn=psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)
                    cur=conn.cursor()
                    cur.execute("insert into log (ts, message) values (now(), %(message)s)", 
                            {'message':message.strip()});
                    conn.commit()
                    parsed = json.loads(message)
                    if parsed['hour'] == '': parsed['hour'] = 0
                    print >>sys.stderr, parsed
                    cur.execute(
                    """insert into botlog 
                       (hour,REMOTE_ADDR, status_line, UserAgent, epoch, REQUEST_URI,HTTP_HOST) 
                       values 
                       (%(hour)s, %(REMOTE_ADDR)s, %(status_line)s, %(User-Agent)s, %(epoch)s, 
                        %(REQUEST_URI)s, %(HTTP_HOST)s)""", 
                       parsed)
                    conn.commit()
                except Exception as e:
                    print str(e)
                    print "failed: message ",message.strip()," query ",cur.query
                finally:
                    cur.close()
                    conn.close()

                print >>sys.stderr, "exiting child ", os.getpid()
                os._exit(0)
                   
        except Exception as e:
             print str(e)
             print "failed reading socket"
             break
        finally:
             stream.close()

except Exception as e:
    print str(e)
    print "failed initializing"
    sys.exit(1)


