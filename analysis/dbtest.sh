#!/bin/sh
CLASSPATH=.:./pgsql/postgresql.jar:/srv/cal/src/weka-3-6-12/weka-pg.jar
javac  -cp $CLASSPATH MLClassifier.java
java  -cp $CLASSPATH MLClassifier $@
