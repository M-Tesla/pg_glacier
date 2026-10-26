# One shared library per PostgreSQL major. This Makefile targets PostgreSQL 17.
# pg_glacier is not part of `zig build`.

-include Makefile.local

GLACIER_INCLUDE ?= ../Glacier/include
GLACIER_LIBDIR ?= ../Glacier/zig-out/lib
PG_CONFIG ?= pg_config

ifeq ($(shell command -v $(PG_CONFIG) >/dev/null 2>&1 && echo yes),yes)
PG_CPPFLAGS += -I$(GLACIER_INCLUDE)
SHLIB_LINK += -L$(GLACIER_LIBDIR) -lglacier -Wl,-rpath,'$(abspath $(GLACIER_LIBDIR))'
EXTENSION = pg_glacier
DATA = pg_glacier--0.6.2.sql pg_glacier--0.6.3.sql pg_glacier--0.6.2--0.6.3.sql
MODULE_big = pg_glacier
OBJS = pg_glacier.o
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
ifeq ($(strip $(PG_SERVER_INCLUDE)),)
$(error Set PG_SERVER_INCLUDE to the PostgreSQL 17 server headers, or put pg_config on PATH)
endif
CC ?= cc
CFLAGS ?= -O2 -fPIC -Wall -Wno-unused-parameter
CPPFLAGS += -I$(PG_SERVER_INCLUDE) -I$(GLACIER_INCLUDE)

pg_glacier.so: pg_glacier.c $(GLACIER_INCLUDE)/glacier.h
	$(CC) $(CFLAGS) $(CPPFLAGS) -shared -o $@ pg_glacier.c \
		-L$(GLACIER_LIBDIR) -lglacier \
		-Wl,-rpath,'$(abspath $(GLACIER_LIBDIR))'

clean:
	rm -f pg_glacier.so pg_glacier.o
endif
