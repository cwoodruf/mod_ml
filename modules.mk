mod_ml.la: mod_ml.slo
	$(SH_LINK) -rpath $(libexecdir) -module -avoid-version  mod_ml.lo
DISTCLEAN_TARGETS = modules.mk
shared =  mod_ml.la
