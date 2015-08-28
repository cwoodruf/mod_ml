##
##  Makefile -- Build procedure for sample ml Apache module
##  Autogenerated via ``apxs -n ml -g''.
##

builddir=.
# top_srcdir=/usr/share/apache2
# top_builddir=/usr/share/apache2
# include /usr/share/apache2/build/special.mk
top_srcdir=/usr/local/apache2.4
top_builddir=/usr/local/apache2.4
include /usr/local/apache2.4/build/special.mk

#   the used tools
APXS=/usr/local/apache2.4/bin/apxs
APACHECTL=/usr/local/apache2.4/bin/apachectl

#   additional defines, includes and libraries
#DEFS=-Dmy_define=my_value
#INCLUDES=-Imy/include/dir
#LIBS=-Lmy/lib/dir -lmylib
SRC=mod_rewrite_funcs.c mod_ml.c
sources=mod_rewrite_funcs.c mod_ml.c

#   the default target
all: both

# compile the shared object file
both: $(SRC) Makefile
	sudo $(APXS) -o mod_ml.so -i -c $(SRC)
	# sudo $(APXS) -D ML_DEBUG=1 -o mod_ml.so -i -c $(SRC)
	# sudo $(APXS) -D ML_DEBUG=0 -o mod_ml.so -i -c $(SRC)
	sudo /usr/local/apache2.4/bin/apachectl restart

#   install the shared object file into Apache 
# install: install-modules-yes
install: both

#   cleanup
clean:
	-rm -f mod_ml.o mod_ml.lo mod_ml.slo mod_ml.la mod_rewrite_funcs.o mod_rewrite_funcs.lo mod_rewrite_funcs.slo mod_rewrite_funcs.la 

#   simple test
test: reload
	lynx -mime_header http://localhost/ml

#   install and activate shared object by reloading Apache to
#   force a reload of the shared object file
reload: install restart

#   the general Apache start/restart/stop
#   procedures
start:
	$(APACHECTL) start
restart:
	$(APACHECTL) restart
stop:
	$(APACHECTL) stop
