# makefile
CFLAGS = -O2 -g
LFLAGS  = -lpthread
CC	= gcc 
OBJ = pgiosim.o 

.SUFFIXES: .c

.c.o:
	$(CC) -c $(CFLAGS) $<


all: pgiosim

pgiosim:	$(OBJ)	
	$(CC) $(CFLAGS) -o pgiosim $(OBJ) $(LFLAGS)

clean:
	rm -f *.o *.bak *~ *% #*
	rm -f libhtmltemplate.a htexample


# DO NOT DELETE THIS LINE -- make depend depends on it.
