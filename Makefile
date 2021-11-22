LIBS = -lsqlite3
DEBUG = -g

all: fts-md

fts-md: *.c
	gcc $^ $(LIBS) -o $@

clean:
	rm -rf ftd-md
