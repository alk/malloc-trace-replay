ACLOCAL_AMFLAGS = -I m4
AUTOMAKE_OPTIONS = subdir-objects

AM_CXXFLAGS = -pthread
AM_LDFLAGS = -pthread

CAPNP = @CAPNP@

noinst_PROGRAMS = replay test_dump

replay_SOURCES = replay.cc varint_codec.cc \
	actual_replay.h actual_replay.cc \
	replay.capnp.c++ replay.capnp.h replay.capnp
replay_LDADD =  @capnp_LIBS@
replay_CXXFLAGS = $(AM_CXXFLAGS) @capnp_CFLAGS@

noinst_LIBRARIES = libsimple_fiber.a librr_fiber.a

libsimple_fiber_a_SOURCES = simple-fiber.cc simple-fiber.h swapcontext_light.cc

librr_fiber_a_SOURCES = rr-fiber.cc rr-fiber.h

test_dump_SOURCES = test_dump.cc replay.capnp.c++ replay.capnp.h
test_dump_LDADD =  @capnp_LIBS@
test_dump_CXXFLAGS = $(AM_CXXFLAGS) @capnp_CFLAGS@

replay.capnp.c++ replay.capnp.h : replay.capnp
	$(CAPNP) compile -oc++ replay.capnp

noinst_PROGRAMS += fiber_test

fiber_test_SOURCES = fiber_test.cc
fiber_test_LDADD = libsimple_fiber.a

noinst_PROGRAMS += rr_fiber_test

rr_fiber_test_SOURCES = rr_fiber_test.cc
rr_fiber_test_LDADD = librr_fiber.a libsimple_fiber.a

# prevent gnu make from creating implicit rule for replay.capnp from
# replay.capnp.c++
replay.capnp: ;

@GENERATE_CHANGELOG_RULES@

# Windows wants write permission to .vcproj files and maybe even sln files.
dist-hook: dist-ChangeLog
