# pax Makefile

MODULE_big = pax_am
OBJS = \
	pax_am.o

PGFILEDESC = "pax -- Table AM for Partition Attributes Across data organization model"

EXTENSION = pax_am
DATA = pax_am--1.0.sql

REGRESS = pax_am

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

