CC = gcc
CFLAGS = -Wall -Wextra -Werror -std=c11
TARGET = pingpong

all: $(TARGET)

$(TARGET): pingpong.c
	$(CC) $(CFLAGS) pingpong.c -o $(TARGET)

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
