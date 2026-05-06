ACLOCAL_AMFLAGS = -I m4 -I./include
AUTOMAKE_OPTIONS = foreign subdir-objects
AM_CFLAGS=-Wall -Werror -fno-strict-aliasing -I$(top_srcdir)/include
AM_CFLAGS+=-fPIC -fPIE -pie -fstack-protector-strong -D_FORTIFY_SOURCE=2 \
	   -O2 -ftrapv -Wl,-z,relro,-z,now -Wl,-s
CLEANFILES =

if WITH_LOG_FILE
AM_CFLAGS+=-DWITH_LOG_FILE=\"$(with_log_file)\"
endif	# WITH_LOG_FILE

MAJOR = 2
MINOR = 10
REVISION = 0
UADK_VERSION = -version-number ${MAJOR}:${MINOR}:${REVISION}

DAY = 10
MONTH = Dec
YEAR = 2025
AM_CFLAGS+= -DUADK_VERSION_NUMBER="\"UADK version: ${MAJOR}.${MINOR}.${REVISION}\""
AM_CFLAGS+= -DUADK_RELEASED_TIME="\"Released ${MONTH} ${DAY}, ${YEAR}\""

pkginclude_HEADERS = include/wd.h include/wd_internal.h include/wd_cipher.h \
		  include/wd_aead.h include/uacce.h include/wd_alg_common.h \
		  include/wd_sched.h include/wd_alg.h

nobase_pkginclude_HEADERS=v1/wd.h v1/wd_cipher.h v1/wd_aead.h v1/uacce.h

lib_LTLIBRARIES=libwd.la libwd_crypto.la

uadk_driversdir=$(libdir)/uadk
uadk_drivers_LTLIBRARIES=libhisi_sec.la libisa_ce.la libisa_sve.la

libwd_la_SOURCES=wd.c wd_mempool.c wd_bmm.c wd_bmm.h wd.h wd_alg.c wd_alg.h	\
		lib/crypto/aes.c lib/crypto/sm4.c lib/crypto/galois.c

libwd_crypto_la_SOURCES=wd_cipher.c wd_cipher.h wd_cipher_drv.h \
			wd_aead.c wd_aead.h wd_aead_drv.h \
			wd_util.c wd_util.h \
			wd_sched.c wd_sched.h \
			wd.c wd.h

libhisi_sec_la_SOURCES=drv/hisi_sec.c drv/hisi_qm_udrv.c \
		lib/crypto/aes.c lib/crypto/sm4.c lib/crypto/galois.c \
		hisi_qm_udrv.h wd_cipher_drv.h wd_aead_drv.h aes.h sm4.h galois.h

if ARCH_ARM64
libisa_ce_la_SOURCES=arm_arch_ce.h drv/isa_ce_sm3.c drv/isa_ce_sm3_armv8.S isa_ce_sm3.h \
		drv/isa_ce_sm4.c drv/isa_ce_sm4_armv8.S drv/isa_ce_sm4.h wd_util.c wd_util.h

libisa_sve_la_SOURCES=drv/hash_mb/hash_mb.c wd_digest_drv.h drv/hash_mb/hash_mb.h \
		drv/hash_mb/sm3_sve_common.S drv/hash_mb/sm3_mb_asimd_x1.S \
		drv/hash_mb/sm3_mb_asimd_x4.S drv/hash_mb/sm3_mb_sve.S \
		drv/hash_mb/md5_sve_common.S drv/hash_mb/md5_mb_asimd_x1.S \
		drv/hash_mb/md5_mb_asimd_x4.S drv/hash_mb/md5_mb_sve.S
endif

if WD_STATIC_DRV
AM_CFLAGS += -DWD_STATIC_DRV -fPIC
AM_CFLAGS += -DWD_NO_LOG

libwd_la_LIBADD = $(libwd_la_OBJECTS) -ldl -lnuma

libwd_crypto_la_LIBADD = -lwd -ldl -lnuma -lm -lpthread
libwd_crypto_la_DEPENDENCIES = libwd.la

libhisi_sec_la_LIBADD = $(libwd_la_OBJECTS) $(libwd_crypto_la_OBJECTS)
libhisi_sec_la_DEPENDENCIES = libwd.la libwd_crypto.la

libisa_ce_la_LIBADD = $(libwd_la_OBJECTS) $(libwd_crypto_la_OBJECTS)
libisa_ce_la_DEPENDENCIES = libwd.la libwd_crypto.la

libisa_sve_la_LIBADD = $(libwd_la_OBJECTS) $(libwd_crypto_la_OBJECTS)
libisa_sve_la_DEPENDENCIES = libwd.la libwd_crypto.la

else
UADK_WD_SYMBOL= -Wl,--version-script,$(top_srcdir)/libwd.map
UADK_CRYPTO_SYMBOL= -Wl,--version-script,$(top_srcdir)/libwd_crypto.map
UADK_V1_SYMBOL= -Wl,--version-script,$(top_srcdir)/v1/libwd.map

libwd_la_LDFLAGS=$(UADK_VERSION) $(UADK_WD_SYMBOL) $(UADK_V1_SYMBOL)
libwd_la_LIBADD= -ldl -lnuma

libwd_crypto_la_LIBADD= -lwd -ldl -lnuma
libwd_crypto_la_LDFLAGS=$(UADK_VERSION) $(UADK_CRYPTO_SYMBOL) -lpthread
libwd_crypto_la_DEPENDENCIES= libwd.la

libhisi_sec_la_LIBADD= -lwd -lwd_crypto
libhisi_sec_la_LDFLAGS=$(UADK_VERSION)
libhisi_sec_la_DEPENDENCIES= libwd.la libwd_crypto.la

libisa_ce_la_LIBADD= -lwd -lwd_crypto
libisa_ce_la_LDFLAGS=$(UADK_VERSION)
libisa_ce_la_DEPENDENCIES= libwd.la libwd_crypto.la

libisa_sve_la_LIBADD= -lwd -lwd_crypto
libisa_sve_la_LDFLAGS=$(UADK_VERSION)
libisa_sve_la_DEPENDENCIES= libwd.la libwd_crypto.la

endif	# WD_STATIC_DRV

# Package configuration files
pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = lib/libwd_crypto.pc lib/libwd.pc
CLEANFILES += $(pkgconfig_DATA)

# Subdirectories - only include cipher-related tests and samples
SUBDIRS=. uadk_tool
