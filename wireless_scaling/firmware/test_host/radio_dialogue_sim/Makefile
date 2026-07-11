CC := clang
CFLAGS := -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined \
          -fno-sanitize-recover=all -I. -I../../common

radio_sweep: radio_sweep.c generated_radio.c radio_host.h
	$(CC) $(CFLAGS) radio_sweep.c generated_radio.c -o $@

generated_radio.c: extract_radio.py ../../node/main/espnow_tx.c ../../master/main/master.c
	python3 extract_radio.py

.PHONY: run clean
run: radio_sweep
	MallocNanoZone=0 ./radio_sweep
clean:
	rm -f radio_sweep generated_radio.c
	rm -rf radio_sweep.dSYM
