// TODO:
//  - rename this to whichever name I decide to land on (don't forget to do a quick ':%s/init/whatever/g')
//  - support booting the system diskless (cf. /etc/rc.initdiskless)
//  - support booting within a jail (cf. nojail & nojailvnet keywords)

#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <grp.h>
#include <mqueue.h>

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

#define MQ_NAME "/init"
#define SERVICE_GROUP "service" // TODO find a more creative name

#define MAX_MESSAGES 10 // not yet sure what this signifies
#define MESSAGE_SIZE 256

#define INIT_ROOT "conf/init/"
#define MOD_DIR INIT_ROOT "mods/"

int main(int argc, char* argv[]) {
	// parse arguments
	// TODO should this be done with 'getopt' like the other aquaBSD programs instead?

	for (int i = 1; i < argc; i++) {
		char* option = argv[i];

		if (strcmp(option, "--verbose") == 0) {
			verbose = true;
		}

		else {
			FATAL_ERROR("Unknown option (%s)\n", option)
		}
	}

	// make sure we're root

	uid_t uid = getuid();

	if (uid) {
		FATAL_ERROR("Must be run as root user 😢 (UID = 0, current UID = %d)\n", uid)
	}

	// make sure the $SERVICE_GROUP group exists, and error if not

	int group_len = getgroups(0, NULL);

	gid_t* gids = malloc(group_len * sizeof *gids);
	getgroups(group_len, gids);

	gid_t service_gid = -1 /* i.e., no $SERVICE_GROUP group */;

	for (int i = 0; i < group_len; i++) {
		gid_t gid = gids[group_len];
		struct group* group = getgrgid(gid); // don't need to free this as per manpage

		if (strcmp(group->gr_name, SERVICE_GROUP) == 0) {
			service_gid = gid;
			break;
		}
	}

	if (service_gid < 0) {
		FATAL_ERROR("Couldn't find \"" SERVICE_GROUP "\" group\n");
	}

	// make sure a message queue named $MQ_NAME doesn't already exist to ensure only one instance of init is running at a time

	mode_t permissions = 0420; // owner ("root") can only read, group ($SERVICE_GROUP) can only write, and others can do neither

	if (mq_open(MQ_NAME, O_CREAT | O_EXCL, permissions, NULL /* defaults are probably fine */) < 0 && errno == EEXIST) {
		FATAL_ERROR("Only one instance of init may be running at a time 😢\n")
	}

	// create message queue

	mqd_t mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, permissions, NULL);

	if (mq < 0) {
		FATAL_ERROR("mq_open(\"" MQ_NAME "\"): %s", strerror(errno))
	}

	// set group ownership to the $SERVICE_GROUP group

	if (fchown(mq, uid /* most likely gonna be root */, service_gid) < 0) {
		FATAL_ERROR("fchown: %s\n", strerror(errno))
	}

	// setup message queue notification signal
	// thanks @qookie 😄

	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);

	sigprocmask(SIG_BLOCK, &set, NULL);

	// actually start launching processes

	LOG("aquaBSD init\n")

	// NOTES (cf. rc.d(8)):
	//  - autoboot=yes/rc_fast=yes for skipping checks and speeding stuff up
	//  - skip everything with the 'nostart' keyword if the $firstboot_sentinel file exists (and 'firstboot' if it's the first time the system is booting after installation - this is for services such as 'growfs' for resizing the root filesystem if it wasn't already done by the installer)
	//  - execute services until $early_late_divider is reached (what $early_late_divider is/means, not a fucking clue)
	//  - check the firstboot again (incase we've moved to a different fs - could this be what $early_late_divider is?)
	//  - delete $firstboot_sentinel (& $firstboot_sentinel"-reboot" if that exists, in which case reboot)

	// remove the message queue completely (this most likely indicated a shutdown/reboot, so it doesn't matter all that much what happens here)

	mq_close(mq); // not sure if this is completely necessary, doesn't matter
	mq_unlink(MQ_NAME);

	return EXIT_SUCCESS;
}