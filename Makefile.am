ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = subdir-objects

AM_CXXFLAGS = -pthread
AM_LDFLAGS =  -Wl,--no-as-needed -ldl -pthread

noinst_PROGRAMS = test_dump2

test_dump2_SOURCES = test_dump2.cc \
	fd-input-mapper.h fd-input-mapper.cc

noinst_PROGRAMS += replay2

replay2_SOURCES = replay2.cc \
	events-serializer.h events-serializer.cc \
	id_tree.h malloc_trace_encoder.h \
	varint_codec.cc varint_codec.h instruction.h \
	fd-input-mapper.h fd-input-mapper.cc

@GENERATE_CHANGELOG_RULES@

dist-hook: dist-ChangeLog
