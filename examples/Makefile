# For multiple programs using a single source file each,
# we can just define 'progs' and create custom targets.
#PROGS	=	pkt-gen bridge testpcap libnetmap.so
#PROGS += pingd
#PROGS	+= testlock testcsum test_select kern_test
PROGS   =       pkt-gen bridge

CC = $(CROSS_COMPILE)gcc

CLEANFILES = $(PROGS) pcap.o nm_util.o *.o
NO_MAN=
CFLAGS = -O2 -pipe
CFLAGS += -Werror -Wall
CFLAGS += -I../sys
CFLAGS += -Wextra

include $(KSRC)/include/config/auto.conf
ifeq ($(CONFIG_CPU_BIG_ENDIAN), y)
CFLAGS += -big-endian
endif

LDFLAGS += -lpthread #-lpcap
LDFLAGS += -lrt	# on linux

all: $(PROGS)

testpcap: pcap.c libnetmap.so nm_util.o
	$(CC) $(CFLAGS) -DTEST -o testpcap pcap.c nm_util.o $(LDFLAGS)

kern_test: testmod/kern_test.c

nm_util.o pkt-gen.o bridge.o libnetmap.so pcap.o: nm_util.h

pkt-gen: pkt-gen.o nm_util.o
	$(CC) $(CFLAGS) -o pkt-gen pkt-gen.o nm_util.o $(LDFLAGS)

bridge: bridge.o nm_util.o

libnetmap.so:	pcap.c nm_util.c
	$(CC) $(CFLAGS) -fpic -c pcap.c
	$(CC) $(CFLAGS) -fpic -c nm_util.c
	$(CC) -shared -o ${.TARGET} pcap.o nm_util.o

clean:
	-@rm -rf $(CLEANFILES)
