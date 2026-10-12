CFLAGS += -O2 -Wall -Wextra $(shell pkg-config --cflags gtk+-3.0 gio-unix-2.0 libmpdclient)
LDLIBS += $(shell pkg-config --libs gtk+-3.0 gio-unix-2.0 libmpdclient) -pthread

ember: src/*.c src/*.h
	$(CC) $(CFLAGS) -o $@ $(filter %.c,$^) $(LDLIBS)

clean:
	rm -f ember
