#!/usr/bin/env python
import memcache
import sys
import re
import socket
import json
import hashlib
import logging
import threading
import time

logging.basicConfig(level=logging.ERROR)
# logging.basicConfig(level=logging.DEBUG)
# logging.basicConfig(level=logging.INFO)

if len(sys.argv) < 4:
    print "usage:",sys.argv[0],"{port} {hostsfile} {memcached server}"
    sys.exit(1)

port = int(sys.argv[1])
hostfile = sys.argv[2]
mc = memcache.Client([sys.argv[3]])
msgcount = 0
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
        logging.debug("whitelist %s" % (whitelist))

    except Exception as e:
        logging.debug("failed reading whitelist %s at line %d: %s" % (hostfile,line, e))

"""
def respond(stream, client_address):
"""
class respond(threading.Thread):
    def __init__(self, stream, client_address):
        threading.Thread.__init__(self)
        self.stream = stream
        self.client_address = client_address

    def run(self):
        stream = self.stream
        client_address = self.client_address
        global mc
        message = ""
        try:
            data = stream.recv(1024)
            if data:
               message = message + data
        except:
            pass
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
                            return
                        else:
                            logging.debug("nonce ok for %s" % client_address)
                    else:
                        logging.debug("no nonce found in message, rejecting")
                        return
            try:
                parsed = json.loads(message)
                if 'ip' in parsed:
                    ipstr = parsed['ip'].strip()
            except:
                ipstr = message

                logging.debug("got %s" % (ipstr)) 
                response = ""
                i = ippat.search(ipstr)
                if i == None:
                    response = "BADIP"
                else:
                    ip = i.group(1)
                    p = mc.get(ip)
                    if p:
                        logging.debug("got prediction %d for %s" % (p,ip)) 
                        if p < 0:
                            response = "NO"
                        else:
                            response = "YES"
                    else:
                        response = "MISSING"
                # response = response + "\n"
                stream.sendall(response)
                logging.debug(response)
        except Exception as e:
            print e
            logging.error("failed: message %s" % (message.strip()))

read_whitelist()

noncepat = re.compile("nonce=(\w*):(\w*)")
ippat = re.compile("^\s*\w[\w\.\-:]+\w[/\s]+(\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3})")

try:
    if port < 1024: raise Exception("bad port number!")

    logging.debug("mod_ml listening on %d" % (port))
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_address = ('',port)
    sock.bind(server_address)
    sock.listen(10)

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
            """
            fork just slightly slower than using threading.Thread
            newpid = os.fork()
            if newpid == 0:
                respond(stream, client_address)
                os._exit(0)
            else:
                kidpid, status = os.wait()
            """
            responder = respond(stream, client_address)
            responder.run()

        except Exception as e:
            logging.error("failed reading socket %s" % (e))
        finally:
            stream.close()

except Exception as e:
    print e
    logging.error("failed initializing" % (str(e)))
    sys.exit(1)

