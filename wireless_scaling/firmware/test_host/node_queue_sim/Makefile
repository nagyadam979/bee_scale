# node_queue_sim — host-oldali power-loss fuzz tesztpad a node_buffer.c-hez.
# make && ./fuzz  (vagy: make run)

CC      := clang
CFLAGS  := -std=c11 -g -O1 -Wall -Wextra \
           -fsanitize=address,undefined -fno-sanitize-recover=all \
           -I. -Imocks -I../../common -I../../node/main

SRCS := fuzz_main.c nvs_sim.c ../../node/main/node_buffer.c
HDRS := sim_shared.h \
        mocks/esp_err.h mocks/nvs.h mocks/nvs_flash.h mocks/esp_log.h \
        mocks/driver/gpio.h mocks/esp_adc/adc_oneshot.h

fuzz: $(SRCS) $(HDRS) Makefile
	$(CC) $(CFLAGS) $(SRCS) -o $@

.PHONY: run clean
run: fuzz
	MallocNanoZone=0 ./fuzz

clean:
	rm -f fuzz
	rm -rf fuzz.dSYM
