CC := clang
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
          -fno-sanitize-recover=all -I.

SRCS := fuzz_main.c flash_sim.c generated_master_ring.c

fuzz: $(SRCS) ring_host.h flash_sim.h
	$(CC) $(CFLAGS) $(SRCS) -o $@

generated_master_ring.c: extract_master_ring.py ../../master/main/master.c ring_host.h
	python3 extract_master_ring.py

.PHONY: run clean
run: fuzz
	MallocNanoZone=0 ./fuzz

clean:
	rm -f fuzz generated_master_ring.c
	rm -rf fuzz.dSYM
