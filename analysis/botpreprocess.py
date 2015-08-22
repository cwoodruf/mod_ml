#!/usr/bin/env python
import sys
import os
import os.path
import re
import socket
import hashlib
import time
import copy
import logging
import threading

logging.basicConfig(level=logging.DEBUG,
                            format='(%(threadName)-10s) %(message)s',
                                                )

# this script is an example preprocessor to be used by a mod_ml system
# it takes a string from mod_ml and sends if off to another module 
# to be processed.
# log and bottiming are examples of modules that do this type of processing 
# the processing appears to be resource intensive enough to require
# an initial buffer

import log          # handles initial of putting a row into a buffer table
import bottiming    # handles collecting timing stats from logged data

def read_whitelist(hostfile):
    """
    read_whitelist handles reading a whitelist of hosts that can communicate
    with this script. 
    
    the whitelist should consist of lines with 
    ip [optional nonce=pw]
    for additional security define a password for a host
    
    passwords need to be set in the mod_ml config as well for that host using a 
    MLFeatures auth {pw}
    directive.
    
    note that this script in theory will work fine without a whitelist
    but on the open internet it is possible that someone may probe the port
    and start playing with it
    """
    try:
        if len(hostfile) == 0:
            print "host file name missing"
            sys.exit(1)

        if not os.path.isfile(hostfile):
            print "hosts file ",hostfile," is not a file"
            sys.exit(1)

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
        return whitelist

    except Exception as e:
        print  str(e)
        print  "failed reading whitelist ",hostfile," at line ",line

# log messages
MAXTHREADS = 1
t = []

messages = [[] for x in range(MAXTHREADS)]
start = [time.time() for x in range(MAXTHREADS)]

def dolog(i):
    global messages
    global start

    logging.debug("starting dolog %d", i)
    while True:
        if len(messages[i]) > 0 or time.time() - start[i] > 5.0:
            if len(messages[i]) == 0: continue;
            start[i] = time.time()
            logging.debug("starting to process messages")
            # note the builtin data types are thread safe
            childmessages = copy.deepcopy(messages[i])
            messages[i] = []
            log.process(childmessages)
        time.sleep(1)

def doupdate():
    logging.debug("starting")
    while True:
        if bottiming.hasmsgs():
            newpid = os.fork()
            if newpid != 0:
                deadkid, kstatus = os.wait()
                logging.debug("pids were: parent %d child %d statu s%d" % (os.getpid(),deadkid,kstatus))
            else:
                # see http://www.python-course.eu/forking.php
                logging.debug("child pid: %d" % (os.getpid()))
                logging.debug("found messages, processing")
                bottiming.readmsgs(os.getpid())
                logging.debug("child %d finished processing" % (os.getpid()))
        time.sleep(1)

# start the program ...
whitelist = {} 
argc = len(sys.argv)

if argc < 2: 
    print "need a port number!"
    sys.exit(1)

if argc < 3:
    print "need a whitelist of host ips who can connect to this process"
    sys.exit(1)

print "starting ",sys.argv[0]
port = int(sys.argv[1]);
whitelist = read_whitelist(sys.argv[2])

noncepat = re.compile("nonce=(\w*):(\w*)")

# the script will loop forever until interrupted ...
try:
    if port < 1024: raise Exception("bad port number!")

    print  "mod_ml listening on ", port
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server_address = ('',int(sys.argv[1]))
    sock.bind(server_address)
    sock.listen(1)

    for i in range(MAXTHREADS):
        t.append(threading.Thread(
                name="process%d" % i,
                target=dolog, args=(i,)))
        t[i].start()
    ut = threading.Thread(name="updater",target=doupdate)
    ut.start()

    count = 0
    while True: # this loop accepts a connection
        print  "waiting for connection"

        stream, client_address = sock.accept()

        try:
            if len(whitelist) == 0:
                pass
            elif client_address[0] in whitelist and whitelist[client_address[0]]:
                pass
            else:
                print  "rejected connection from ",client_address[0] #," whitelist ",whitelist
                continue
        except Exception as e:
            print  str(e)
            print  "error checking whitelist for ",client_address[0] #," whitelist ",whitelist
            continue

        # now get the message
        try: 
            message = ""
            while True: # this loop reads a message
                try:
                    data = stream.recv(1024)
                    print  "got ",data
                    if data:
                       message = message + data
                       if "\n\n" in data:
                           stream.sendall("OK");
                    else:
                        break
                except:
                    break

            print "checking ",client_address[0] # ," against whitelist ",whitelist
            if client_address[0] in whitelist:
                if whitelist[client_address[0]] == True:
                    print "no pw needed"
                    pass
                else:
                    print "looking for password"
                    sha1pw = whitelist[client_address[0]]
                    print "found encoded pw ",sha1pw
                    m = noncepat.search(message)
                    if m != None:
                        nonce, salt = m.groups()
                        print  "got nonce ",nonce," and salt ",salt," from ",client_address
                        testnonce = hashlib.sha1(sha1pw+salt).hexdigest()
                        print "nonce ",nonce," salt ",salt," testnonce ",testnonce
                        if testnonce != nonce:
                            print  "nonce failed for ",client_address
                            continue
                        else:
                            print  "nonce ok for ",client_address
                    else:
                        print  "no nonce found in message, rejecting"
                        continue

        except Exception as e:
             print "failed reading socket: "
             print str(e)
             break
        finally:
             stream.close()

        messages[(count % MAXTHREADS)].append(message)
        count += 1
        if count > 1000000: count = 0

    ut.join()
    for i in range(MAXTHREADS):
        t[i].join()

except Exception as e:
    print str(e)
    print "failed initializing"
    sys.exit(1)


