.PHONY: all
all: text_transcript

SQLITE3_PATH = ..

sqlite3.o:
	gcc -O3 -c $(SQLITE3_PATH)/sqlite3.c -o sqlite3.o

text_transcript: sqlite3.o
	gcc -O3 -c -I$(SQLITE3_PATH) text_processing.c -o text_processing.o
	gcc text_processing.o sqlite3.o -o text_transcript

.PHONY: clean
clean:
	-rm text_processing.o text_transcript transcript.txt

.PHONY: cleanall
cleanall: clean
	-rm *.o