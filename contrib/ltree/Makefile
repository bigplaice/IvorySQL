# contrib/ltree/Makefile

MODULE_big = ltree
OBJS = \
	$(WIN32RES) \
	_ltree_gist.o \
	_ltree_op.o \
	crc32.o \
	lquery_op.o \
	ltree_gist.o \
	ltree_io.o \
	ltree_op.o \
	ltxtquery_io.o \
	ltxtquery_op.o

EXTENSION = ltree
DATA = ltree--1.2--1.3.sql ltree--1.1--1.2.sql ltree--1.1.sql \
	ltree--1.0--1.1.sql
PGFILEDESC = "ltree - hierarchical label data type"

HEADERS = ltree.h

REGRESS = ltree
ORA_REGRESS = ivy_ltree

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/ltree
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
