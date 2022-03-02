#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define LOG_SIGNATURE "[init]"

#define LOG_REGULAR "\033[0m"
#define LOG_RED     "\033[0;31m"
#define LOG_GREEN   "\033[0;32m"
#define LOG_YELLOW  "\033[0;33m"

static bool verbose = false;

#define LOG(...) \
	if (verbose) { \
		printf(LOG_SIGNATURE LOG_REGULAR " " __VA_ARGS__); \
	}

#define FATAL_ERROR(...) \
	fprintf(stderr, LOG_SIGNATURE LOG_RED " ERROR "__VA_ARGS__); \
	fprintf(stderr, LOG_REGULAR); \
	\
	exit(EXIT_FAILURE);

#define WARN(...) \
	fprintf(stderr, LOG_SIGNATURE LOG_YELLOW " WARNING "__VA_ARGS__); \
	fprintf(stderr, LOG_REGULAR);

#define INIT_ROOT "conf/init/"
#define MOD_DIR INIT_ROOT "mods/"

int main(int argc, char* argv[]) {
	// parse arguments

	for (int i = 1; i < argc; i++) {
		char* option = argv[i];
		
		if (strcmp(option, "--verbose") == 0) {
			verbose = true;
		}

		else {
			FATAL_ERROR("Unknown option (%s)\n", option)
		}
	}

	// TODO make sure another aquaBSD init process has not already been started

	setproctitle("aquaBSD init");

	// make sure we're root

	if (geteuid()) {
		FATAL_ERROR("Must be run as root user 😢\n")
	}

	LOG("aquaBSD init\n")

	return EXIT_SUCCESS;
}