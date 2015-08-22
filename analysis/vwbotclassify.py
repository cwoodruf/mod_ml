#!/usr/bin/env python
import psycopg2
import sys
import os
import re
import socket
import json
import hashlib
from mldb import dbname, dbuser, dbpw
import logging
import subprocess

vwclassify = "/srv/cal/src/apache/ml/analysis/vwbotclassify.sh"

if len(sys.argv) < 4:
    print "usage:",sys.argv[0],"{port} {hostsfile} {vowpal wabbit model file}"
    sys.exit(1)

port = int(sys.argv[1])
hostfile = sys.argv[2]
modelfile = sys.argv[3]
msgcount = 0
whitelist = {} 
query = """
select
    class||' '||
    '|'||' '||
    'mean:'||mean||' '||
    'var:'||var||' '||
    'skew:'||skew||' '||
    'kurtosis:'||kurtosis||' '||
    'hmean:'||hmean||' '||
    'hvar:'||hvar||' '||
    'hskew:'||hskew||' '||
    'hkurtosis:'||hkurtosis||' '||
    'htmean:'||htmean||' '||
    'htvar:'||htvar||' '||
    'htskew:'||htskew||' '||
    'htkurtosis:'||htkurtosis||' '||
    'poverr:'||(pages/reqs)||' '||
    'uacount:'||(array_length(uas, 1))||' '||
    'errprop:'||(errs/reqs)
from botstats
where ip=%(ip)s
"""

def reconnect(conn=None):
    if conn != None and not conn.closed:
        return conn
    logging.debug("mod_ml connecting to database")
    return psycopg2.connect(dbname=dbname, user=dbuser, password=dbpw)

conn = reconnect()
cur = conn.cursor()

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
        logging.debug("whitelist %s" % (whitelist))

    except Exception as e:
        logging.debug("failed reading whitelist %s at line %d: %s" % (hostfile,line, e))
        
read_whitelist()

noncepat = re.compile("nonce=(\w*):(\w*)")
ippat = re.compile("^\s*(\w[\w\.\-:]+\w/\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})")

try:
    if port < 1024: raise Exception("bad port number!")

    logging.debug("mod_ml listening on %d" % (port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_address = ('',int(sys.argv[1]))
    sock.bind(server_address)
    sock.listen(1)

    while True: # this loop accepts a connection
        msgcount += 1
        msgcount = msgcount % 200000000
        logging.debug("waiting for connection")
        stream, client_address = sock.accept()

        try:
            if len(whitelist) == 0:
                pass
            elif client_address[0] in whitelist and whitelist[client_address[0]]:
                pass
            else:
                logging.debug("rejected connection from %s" % (client_address[0]))
                continue
        except Exception as e:
            logging.error("error checking whitelist for %s: %s" % (client_address[0],e))
            continue

        try:
            message = ""
            while True: # this loop reads a message
                try:
                    data = stream.recv(1024)
                    logging.debug("got ",data)
                    if data:
                       message = message + data
                    else:
                        break
                except:
                    break
            # now update the db with the message
            try: 
                logging.debug("checking %s against whitelist" % (client_address[0]))
                if client_address[0] in whitelist:
                    logging.debug("looking for password")
                    if whitelist[client_address[0]] == True:
                        pass
                    else:
                        sha1pw = whitelist[client_address[0]]
                        logging.debug("found encoded pw %s" % (sha1pw))
                        m = noncepat.search(message)
                        if m != None:
                            nonce, salt = m.groups()
                            testnonce = hashlib.sha1(sha1pw+salt).hexdigest()
                            if testnonce != nonce:
                                logging.debug("nonce failed for %s" % client_address)
                                continue
                            else:
                                logging.debug("nonce ok for %s" % client_address)
                        else:
                            logging.debug("no nonce found in message, rejecting")
                            continue
                try:
                    parsed = json.loads(message)
                    if 'ip' in parsed:
                        ipstr = parsed['ip'].strip()
                except:
                    ipstr = message

                    i = ippat.search(ipstr)
                    if i == None:
                        logging.debug("bad ip %s" % ipstr) 
                        stream.sendall("BADIP")
                    else:
                        ip = i.group(1)
                        conn = reconnect(conn)
                        cur = conn.cursor()
                        cur.execute(query, { 'ip': ip })
                        if cur.rowcount > 0:
                            row = cur.fetchone()
                            prediction = subprocess.check_output([vwclassify, row[0], modelfile])
                            try:
                                p = float(prediction)
                                logging.debug("got prediction %.4f for %s" % (p,ip)) 
                                if p < 0:
                                    print msgcount,ip,"NO",p
                                    stream.sendall("NO")
                                else:
                                    print msgcount,ip,"YES",p
                                    stream.sendall("YES")
                            except:
                                logging.debug("invalid response for %s %s" % ip, prediction) 
                                print msgcount,ip,"invalid response",prediction
                                stream.sendall("BADRESPONSE")
                        else:
                            logging.debug("no data for %s" % ip) 
                            print msgcount,ip,"NOTFOUND"
                            stream.sendall("NOTFOUND")
                else:
                    logging.debug("no ip %s" % ipstr) 
                    stream.sendall("NOIP")
                
            except Exception as e:
                logging.error("failed: %s message %s query %s" % (e,message.strip(),cur.query))
            finally:
                cur.close()

        except Exception as e:
             logging.error("failed reading socket %s" % (e))
             break
        finally:
             stream.close()

except Exception as e:
    logging.error("failed initializing" % (str(e)))
    sys.exit(1)


