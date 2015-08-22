#!/bin/sh
classifiers="data-and-models/botnormalize-bftree.model data-and-models/botnormalize-ftree.model data-and-models/botnormalize-j48.model data-and-models/botnormalize-lmt.model data-and-models/botnormalize-randforest.model"
# input='{"class":-1,  "a":8.52108115477692e-06, "b":6.72268750173818e-11, "c":0, "d":0, "e":0, "f":0, "g":0, "h":0, "i":0.000213964767135012, "j":7.74924308603671e-08, "k":0.409360181154389, "l":-0.245571822003189, "m":0, "n":1, "o":0}'
# javac -cp .:/usr/share/java/commons-lang-2.6.jar:./json-simple-1.1.1.jar:../../../weka-3-6-12/weka-pg.jar CLIClassifier.java
javac -cp .:./pgsql/postgresql.jar:/usr/share/java/commons-lang-2.6.jar:./json-simple-1.1.1.jar:../../../weka-3-6-12/weka-pg.jar BotClassifier.java
# java -cp .:/usr/share/java/commons-lang-2.6.jar:./json-simple-1.1.1.jar:../../../weka-3-6-12/weka-pg.jar CLIClassifier "$input"
java -cp .:./pgsql/postgresql.jar:/usr/share/java/commons-lang-2.6.jar:./json-simple-1.1.1.jar:../../../weka-3-6-12/weka-pg.jar BotClassifier $classifiers
