EXTRA_DIST = OBJECTFILES OBJECTLIBS

pkglib_LTLIBRARIES = libenumbackend.la

libenumbackend_la_SOURCES = enumbackend.cc enumbackend.hh
libenumbackend_la_LDFLAGS = -module -avoid-version
libenumbackend_la_LIBADD = $(LDAP_LIBS)
