This directory contains a reference design for web robot detection that uses
the mod_ml Apache module as its main data source. The ml_bot.conf file in
the main mod_ml directory contains directives used to test this reference
design. 

Testing for this reference design involved replaying apache combined format
access logs. An 8 node cluster did all preprocessing. Classifier predictions
were stored in an in-memory redis database as well as a postgres database.
Vowpal wabbit (vw) was used as the classifier/model building tool for this reference
design. Redis and vw were run in daemon mode on a separate server. 

The core files used for the experiment were:
* ml_bot.conf, ml.load, mod_ml.so - all loaded on the web servers in the test web server cluster
* botlogger.py, botlog.py, botupdstats.py and bottiming.py run on 8 servers on port 39992 for preprocessing
* psql/botstats.psql - to create tables for botlogger.py etc.
* botlabelonline.py - run on port 49994 to translate User Agent strings into bot(1)/human(-1) labels for supervised learning
* psql/botlabels.psql - to create tables used by botlabelonline.py
* vw/startvw.sh - to start the vw classifier process on port 26542 and model builder on port 26541
* epoch.pl (in the testers directory) converts Apache date format to epoch seconds
* myhost.pl run on port 48888 converts a REMOTE_ADDR IP to a preprocessor host IP to parallelize preprocessing
* optionally redis-botclassify.py run on port 39999 on the same host that bottiming.py writes predictions to 

What gets run where:
* replay.pl gets runo on a separate host with a set of Apache access logs in combined format
* apache is run on one or more hosts with the mod_ml module and configuration
* botlogger.py etc is run on 8 hosts in a cluster with non-replicated postgresql instances (these should support > 513 connections)
* epoch.pl, myhost.pl, botlabelonline.py and redis-botclassify.py are run on a separate host from botlogger, apache and replay.pl
* for the experiment apache and botlogger were run on the same hosts and replay.pl epoch.pl, myhost.pl etc were run on a single separate host

