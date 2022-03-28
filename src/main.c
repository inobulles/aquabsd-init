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

typedef struct {
	char* name;

	size_t deps_len;
	service_t** deps;
} service_t;

static service_t* new_service(const char* name) {
	service_t* service = calloc(1, sizeof *service);
	service->name = strdup(name);

	return service;
}

static void fill_search_service(service_t* service) {
	// TODO read the service script, similar to what rcorder(8) does on NetBSD
}

static void del_service(service_t* service) {
	if (!service) {
		return;
	}

	if (service->name) {
		free(service->name);
	}

	free(service);
}

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
	//  - execute services until $early_late_divider is reached ('early_late_divider=FILESYSTEMS', ensure root & "critical" filesystems are mounted basically)
	//  - check the firstboot again (incase we've moved to a different fs)
	//  - delete $firstboot_sentinel (& $firstboot_sentinel"-reboot" if that exists, in which case reboot)

	size_t services_len = 0;
	service_t** services = NULL;

	// read all the legacy research Unix-style services in '/etc/rc.d'

	DIR* dp = opendir("/etc/rc.d");

	if (!dp) {
		FATAL_ERROR("opendir(\"/etc/rc.d\"): %s\n", strerror(errno))
	}

	struct dirent* entry;

	while ((entry = readdir(dp))) {
		if (entry->d_type != DT_REG) {
			continue; // anything other than a regular file is invalid
		}

		if (*entry->d_name == '.') {
			continue; // don't care about '.', '..', & other entries starting with a dot
		}

		struct stat sb;

		if (stat(entry->d_name, &sb) < 0) {
			FATAL_ERROR("stat(\"%s\"): %s\n", entry->d_name, strerror(errno))
		}

		mode_t permissions = sb.st_flags & 0777;

		if (permissions == 0555) {
			FATAL_ERROR("\"%s\" doesn't have the right permissions ('0%o', needs '0555')\n", entry->d_name, permissions)
		}

		// okay! add the service

		service_t* service = new_service(entry->d_name);

		services = realloc(services, ++services_len * sizeof *services);
		services[services_len - 1] = service;

		fill_research_service(service);
	}

	// free services

	for (size_t i = 0; i < services_len; i++) {
		del_service(services[i]);
	}

	if (services) {
		free(services);
	}

	// remove the message queue completely (this most likely indicated a shutdown/reboot, so it doesn't matter all that much what happens here)

	mq_close(mq); // not sure if this is completely necessary, doesn't matter
	mq_unlink(MQ_NAME);

	return EXIT_SUCCESS;
}