CC := gcc
CFLAGS := -std=c11 -Wall -Wextra -O0 -g -I../..

OBJS := runner.o test_bus.o cJSON.o ../../cpu.o ../../cpu_ops.o

runner: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

../../%.o: ../../%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f *.o ../../cpu.o ../../cpu_ops.o runner