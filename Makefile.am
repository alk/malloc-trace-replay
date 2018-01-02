ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = subdir-objects

AM_CXXFLAGS = -pthread
AM_LDFLAGS =  -Wl,--no-as-needed -ldl -pthread

CAPNP = @CAPNP@
LD_RUN_PATH = @LD_RUN_PATH@
export LD_RUN_PATH

noinst_PROGRAMS = test_dump2

test_dump2_SOURCES = test_dump2.cc replay2.capnp.c++ replay2.capnp.h
test_dump2_LDADD =  @capnp_LIBS@
test_dump2_CXXFLAGS = $(AM_CXXFLAGS) @capnp_CFLAGS@

replay2.capnp.c++ replay2.capnp.h : replay2.capnp
	$(CAPNP) compile -oc++ replay2.capnp

noinst_PROGRAMS += replay2

replay2_SOURCES = replay2.cc \
	events-serializer.h events-serializer.cc \
	id_tree.h malloc_trace_encoder.h \
	varint_codec.cc varint_codec.h instruction.h
nodist_replay2_SOURCES = replay2.capnp.c++ replay2.capnp.h
replay2_LDADD =  @capnp_LIBS@
replay2_CXXFLAGS = $(AM_CXXFLAGS) @capnp_CFLAGS@

dist_noinst_DATA = replay2.capnp

MOSTLYCLEANFILES = replay2.capnp.c++ replay2.capnp.h
BUILT_SOURCES = replay2.capnp.h

# prevent gnu make from creating implicit rule for replay.capnp from
# replay2.capnp.c++
replay2.capnp: ;

@GENERATE_CHANGELOG_RULES@

# Windows wants write permission to .vcproj files and maybe even sln files.
dist-hook: dist-ChangeLog
