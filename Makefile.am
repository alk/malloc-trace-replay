ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = subdir-objects

CAPNP = @CAPNP@

noinst_PROGRAMS = replay test_dump

replay_SOURCES = replay.cc varint_codec.cc \
	actual_replay.h actual_replay.cc \
	replay.capnp.c++ replay.capnp.h replay.capnp
replay_LDADD =  @capnp_LIBS@
replay_CXXFLAGS = -pthread @capnp_CFLAGS@
replay_LDFLAGS = -pthread

test_dump_SOURCES = test_dump.cc replay.capnp.c++ replay.capnp.h
test_dump_LDADD =  @capnp_LIBS@
test_dump_CXXFLAGS = -pthread @capnp_CFLAGS@
test_dump_LDFLAGS = -pthread

replay.capnp.c++ replay.capnp.h : replay.capnp
	$(CAPNP) compile -oc++ replay.capnp

noinst_PROGRAMS += fiber_test

fiber_test_SOURCES = simple-fiber.cc simple-fiber.h \
	swapcontext_light.cc fiber_test.cc
fiber_test_CXXFLAGS = -pthread
fiber_test_LDFLAGS = -pthread

# prevent gnu make from creating implicit rule for replay.capnp from
# replay.capnp.c++
replay.capnp: ;

@GENERATE_CHANGELOG_RULES@

# Windows wants write permission to .vcproj files and maybe even sln files.
dist-hook: dist-ChangeLog
