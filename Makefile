PREFIX = .
CC = cc
CFLAGS = -Wall -O0 -g -I$(PREFIX)/include `pkg-config --cflags arcan-shmif`
LDFLAGS = -L/usr/lib `pkg-config --libs arcan-shmif`

all: apdf
%.o: %.c doc.h
	$(CC) -c $(CFLAGS) $<
clean:
	-rm -f *.o fbpdf fbdjvu fbpdf2

apdf: apdf.o
	$(CC) -o $@ $^ $(LDFLAGS) -lmupdf -lmupdf-third -lmupdf-pkcs7 -lmupdf-threads -lm -lfreetype -lz -ljpeg -lharfbuzz -ljbig2dec -lopenjp2 -lgumbo
