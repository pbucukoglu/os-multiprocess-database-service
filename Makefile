CC = gcc
CFLAGS = -Wall -Wextra -std=c99
TARGET = database_service
SOURCES = src/database_service.c

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET)

.PHONY: clean
