# pax Makefile

MODULE_big = pax_am
OBJS = \
	pax_am.o

PGFILEDESC = "pax -- Table AM for Partition Attributes Across data organization model"

EXTENSION = pax_am
DATA = pax_am--1.0.sql

REGRESS = pax_am
ISOLATION = pax_mvcc
ISOLATION_OPTS = --load-extension=pax_am

PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)

SHARED_BUFFERS_TEST = tests/compare_shared_buffers.sh
COLUMNAR_BENCHMARK  = tests/benchmark_columnar.sh

.PHONY: shared-buffers-test columnar-benchmark
shared-buffers-test:
	$(SHARED_BUFFERS_TEST)

# Mesure les effets reels de la disposition columnaire face a heap.
# Hors installcheck : resultats dependants de la machine, aucun golden.
columnar-benchmark:
	$(COLUMNAR_BENCHMARK)

