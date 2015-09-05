This directory contains files for testing the mod_ml Apache module.
Many of the scripts are simple services used by the .htaccess files in
the modtest directory. For the tests to work these scripts must be 
copied to the location where a given .htaccess file expects them to be
(generally /usr/local/bin). For the unix socket test to work correctly
the sock.log file should be writeable by the preprocessor script.
