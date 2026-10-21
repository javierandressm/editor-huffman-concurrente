CC = gcc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_FILE_OFFSET_BITS=64
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g
DEPFLAGS = -MMD -MP

TARGET = editor
SRC_DIR = src
TEST_DIR = tests
OBJ_DIR = obj
SRCS = $(SRC_DIR)/editor_main.c $(SRC_DIR)/editor.c \
       $(SRC_DIR)/lista.c $(SRC_DIR)/archivo.c $(SRC_DIR)/compresor.c
OBJS = $(SRCS:$(SRC_DIR)/%.c=$(OBJ_DIR)/%.o)
DEPS = $(OBJS:.o=.d)
CFLAGS += -pthread

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

run: all
	./$(TARGET)

test: all
	sh $(TEST_DIR)/pruebas.sh

clean:
	rm -rf $(OBJ_DIR) $(TARGET)

-include $(DEPS)

.PHONY: all run test clean
