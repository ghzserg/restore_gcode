#CC = arm-linux-gnueabihf-gcc-12
#CC = arm-linux-gnueabi-gcc-12
CFLAGS = -Wall -s -O3

SRC_FILES = $(wildcard *.c)

TARGET = restore_gcode

$(TARGET): $(SRC_FILES)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

.PHONY: clean

clean:
	rm -f $(TARGET)
