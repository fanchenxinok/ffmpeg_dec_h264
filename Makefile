CROSS_COMPILER = /home/joe/worksource/J721E_EVM/ti-processor-sdk-linux-automotive-j7-evm-06_01_00_05/linux-devkit/sysroots/x86_64-arago-linux/usr/bin/aarch64-linux-gnu
CC = gcc
CFLAGS = -g -O0
CLIBS = -lavformat -lavcodec -lavutil -lSDL2 -pthread -lrt -lm

USR_INC = 
INCLUDE_DIRS = -I./

SOURCES = $(wildcard ./*.c)

TARGET = dec_h264
OBJECTS = $(patsubst %.c,%.o,$(SOURCES))

$(TARGET) : $(OBJECTS)
	$(CC) $^ -o $@ $(CLIBS)
	
$(OBJECTS) : %.o : %.c 
	$(CC) -c $(CFLAGS) $< -o $@ $(INCLUDE_DIRS)

.PHONY : clean
clean:
	rm -rf $(TARGET) $(OBJECTS)