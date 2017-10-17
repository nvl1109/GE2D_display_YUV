TARGET = HelloGE2D

CC = gcc
CFLAGS = -O2
LDFLAGS =
SRCS = main.cpp convert.cpp
LIBS =

OBJS=$(SRCS:%.cpp=%.o)

.PHONY: all clean
all: $(TARGET)

clean:
	@rm -f *.o $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

.cpp.o:
	$(CC) $(CFLAGS) -c $< -o $@
