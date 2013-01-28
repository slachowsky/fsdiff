CFLAGS=-g -O0
#LDLIBS=-lbz2
all: bsdiff bspatch fsdiff fspatch

fsdiff fspatch: LDLIBS=-ltar

clean:
	rm bsdiff bspatch fsdiff fspatch
