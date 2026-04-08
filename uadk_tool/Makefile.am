ACLOCAL_AMFLAGS = -I m4 -I./include
AUTOMAKE_OPTIONS = foreign subdir-objects
AM_CFLAGS=-Wall -Werror -fno-strict-aliasing -I$(top_srcdir) -I$(top_srcdir)/benchmark/include \
			-pthread
AM_CFLAGS += -fPIC -fPIE -pie -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
-O2 -ftrapv -Wl,-z,now -Wl,-s

#AUTOMAKE_OPTIONS = subdir-objects

bin_PROGRAMS=uadk_tool

# Only keep cipher-related test sources
uadk_tool_SOURCES+=benchmark/sec_soft_benchmark.c benchmark/sec_soft_benchmark.h

if WD_STATIC_DRV
AM_CFLAGS+=-Bstatic
uadk_tool_LDADD=$(libwd_la_OBJECTS) \
			$(libwd_crypto_la_OBJECTS) \
			../.libs/libhisi_sec.a \
			-ldl -lnuma
else
uadk_tool_LDADD=-L../.libs -l:libwd.so.2 -l:libwd_crypto.so.2 \
		-lnuma
endif

# For statistics
uadk_tool_LDADD+=-lm

if HAVE_ZLIB
uadk_tool_LDADD+=-lz
uadk_tool_CPPFLAGS=-DUSE_ZLIB
endif

if HAVE_CRYPTO
uadk_tool_SOURCES+=benchmark/sec_soft_benchmark.c benchmark/sec_soft_benchmark.h \
		   test/comp_main.c test/comp_main.h test/comp_lib.c test/comp_lib.h
uadk_tool_LDADD+=$(libcrypto_LIBS)
endif
