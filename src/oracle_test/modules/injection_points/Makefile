# src/oracle_test/modules/injection_points/Makefile

MODULES = injection_points

EXTENSION = injection_points
DATA = injection_points--1.0.sql
PGFILEDESC = "injection_points - facility for injection points"

REGRESS = injection_points
REGRESS_OPTS = --dlpath=$(top_builddir)/src/oracle_test/regress

ISOLATION = inplace

# The injection points are cluster-wide, so disable installcheck
NO_INSTALLCHECK = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/oracle_test/modules/injection_points
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global

# XXX: This test is conditional on enable_injection_points in the
# parent Makefile, so we should never get here in the first place if
# injection points are not enabled. But the buildfarm 'misc-check'
# step doesn't pay attention to the if-condition in the parent
# Makefile. To work around that, disable running the test here too.
ifeq ($(enable_injection_points),yes)
include $(top_srcdir)/contrib/contrib-global.mk
else
check:
	@echo "injection points are disabled in this build"
endif

endif
