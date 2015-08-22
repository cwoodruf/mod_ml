

The mod_ml Apache module

mod_ml is an Apache module designed to work with machine learning
systems. An example system developed around doing bot detection
the hard way can be found in the analysis directory. The ml_bot.conf
file is the mod_ml configuration file needed to interface with
this bot detection system.

For more on mod_ml syntax see the users guide in the docs directory.

Note about the bot detection reference design:

The idea behind the bot detection system is: can we detect bots
without lots of logging? In reality you would want to use the
User-Agent header to detect most bots and than fork over to
this system if you cannot figure out whether they are a bot or not.
However, mod_ml makes it relatively easy to use the bot/human
classification to modify web requests or even system behaviour.

The bot detection system requires a postgres database and uses
weka. See analysis/psql and the shell scripts for more.
The botstats.psql file in analysis/psql creates the relevant tables.
You should be able to adapt it to work with other tables that
have similar structure. The system has been tested under fairly
heavy load but is not production ready. For example, the best
models for detecting bots have not been determined.
