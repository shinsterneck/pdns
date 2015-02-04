AM_CPPFLAGS = @THREADFLAGS@ @BOOST_CPPFLAGS@
EXTRA_DIST= OBJECTFILES OBJECTLIBS
pkglib_LTLIBRARIES = libgeosqlbackend.la
libgeosqlbackend_la_SOURCES =geosqlbackend.cpp geosqlbackend.h
libgeosqlbackend_la_LDFLAGS =-module -avoid-version
libgeosqlbackend_la_LIBADD = $(OPENDBX_LIBS) @BOOST_REGEX_LDFLAGS@
