# contrib/wal2json/Makefile

MODULE_big = wal2json
OBJS = \
	$(WIN32RES) \
	wal2json.o
PGFILEDESC = "passwordcheck - strengthen user password checks"

# uncomment the following two lines to enable cracklib support
# PG_CPPFLAGS = -DUSE_CRACKLIB '-DCRACKLIB_DICTPATH="/usr/lib/cracklib_dict"'
# SHLIB_LINK = -lcrack

REGRESS = cmdline insert1 update1 update2 update3 update4 delete1 delete2 \
                  delete3 delete4 savepoint specialvalue toast bytea message typmod \
                  filtertable selecttable include_timestamp include_lsn include_xids \
                  include_domain_data_type truncate type_oid actions position default \
                  pk rename_column numeric_data_types_as_string 

ORA_REGRESS = ivy_cmdline ivy_insert1 ivy_update1 ivy_update2 ivy_update3 ivy_update4 \
                  ivy_delete1 ivy_delete2 ivy_delete3 ivy_delete4 ivy_savepoint \
                  ivy_specialvalue ivy_toast ivy_bytea ivy_message ivy_typmod \
                  ivy_filtertable ivy_selecttable ivy_include_timestamp ivy_include_lsn \
                  ivy_include_xids ivy_include_domain_data_type ivy_truncate ivy_type_oid \
                  ivy_actions ivy_position ivy_default ivy_pk ivy_rename_column \
                  ivy_numeric_data_types_as_string 

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/wal2json/logical.conf
ORACLE_REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/wal2json/logical.conf

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/wal2json
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
