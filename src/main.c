// TODO:
//  - rename this to whichever name I decide to land on (don't forget to do a quick ':%s/init/whatever/g')
//  - support booting the system diskless (cf. '/etc/rc.initdiskless')
//  - support booting within a jail (cf. 'service_t.disable_in_jail' & 'service_t.disable_in_vnet_jail')
//  - perhaps a hashmap system for resolving dependencies in a better time complexity (similar to what rcorder(8) does on NetBSD)
//  - record timing, which can I guess either be written to a log at some point or queried with some command

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <threads.h>
#include <unistd.h>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <grp.h>
#include <mqueue.h>
#include <libutil.h>

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

typedef enum {
	SERVICE_KIND_GENERIC,
	SERVICE_KIND_RESEARCH, // for research Unix-style runcom scripts
	SERVICE_KIND_AQUABSD,
} service_kind_t;

typedef struct {
	size_t provides_len;
	char** provides;
} service_research_t;

typedef struct {
} service_aquabsd_t;

typedef struct service_t service_t;

struct service_t {
	service_kind_t kind;

	char* name;
	char* path;

	size_t deps_len;

	char** dep_names;
	service_t** deps;

	bool check_circular_passed;

	// service flags (these are what NetBSD would call "keywords")

	bool on_start;
	bool on_stop;
	bool on_resume;

	bool first_boot;

	bool disable_in_jail;
	bool disable_in_vnet_jail;

	// actual service stuff

	thrd_t thread;
	pid_t pid;

	// kind-specific members

	union {
		service_research_t research;
		service_aquabsd_t aquabsd;
	};
};

static service_t* new_service(const char* name) {
	service_t* service = calloc(1, sizeof *service);

	service->kind = SERVICE_KIND_GENERIC;
	service->name = strdup(name);

	// set defaults for service flags
	// by default, a service is launched on start (on regular systems and any jails)

	service->on_start             = true;
	service->on_stop              = false;
	service->on_resume            = false;

	service->first_boot           = false;

	service->disable_in_jail      = false;
	service->disable_in_vnet_jail = false;

	return service;
}

static int fill_research_service(service_t* service) {
	// yeah, this function really isn't pretty, but I'd rather make is as compact as possible with ugly macros as I don't really want this to be the focus of this source file

	service->kind = SERVICE_KIND_RESEARCH;

	// read the service script, similar to what rcorder(8) does on NetBSD
	// a lot of this code is actually even stolen from NetBSD's 'sbin/rcorder/rcorder.c'

	FILE* fp = fopen(service->path, "r");

	if (!fp) {
		return -1;
	}

	char* buf;

	// now that's what I call a BIG GRAYSON
	// TODO little note, and probably something to fix in both FreeBSD & NetBSD, the 'REQUIRES', 'PROVIDES', & 'KEYWORDS' directives are useless (it's pretty obvious how when looking at the code)

	char* require = NULL;
	char* provide = NULL;
	char* before  = NULL;
	char* keyword = NULL;

	enum { BEFORE_PARSING, PARSING, PARSING_DONE } state;

	for (
		state = BEFORE_PARSING;
		state != PARSING_DONE && (buf = fparseln(fp, NULL, NULL, "\\\\", 0));
		free(buf)
	) {
		#define DIRECTIVE(lower, upper) \
			else if (strncmp("# " #upper ":", buf, sizeof(#upper) - 1) == 0) { \
				lower = strdup(buf + sizeof(#upper) + 3); \
			}

		if (0) {}

		DIRECTIVE(require, REQUIRE)
		DIRECTIVE(provide, PROVIDE)
		DIRECTIVE(before,  BEFORE )
		DIRECTIVE(keyword, KEYWORD)

		else {
			if (state == PARSING) {
				state = PARSING_DONE;
			}

			continue;
		}

		#undef DIRECTIVE

		state = PARSING;
	}

	// parse 'require' as service dependency names

	char* str;

	while ((str = strsep(&require, " \t\n"))) {
		if (!*str) {
			continue;
		}

		service->dep_names = realloc(service->dep_names, ++service->deps_len * sizeof *service->dep_names);
		service->dep_names[service->deps_len - 1] = strdup(str);
	}

	// parse 'provide' as, well, provide

	while ((str = strsep(&provide, " \t\n"))) {
		if (!*str) {
			continue;
		}

		service->research.provides = realloc(service->research.provides, ++service->research.provides_len * sizeof *service->research.provides);
		service->research.provides[service->research.provides_len - 1] = strdup(str);
	}

	// parse 'keyword' as service flags
	// default were already set when creating the service object

	while ((str = strsep(&keyword, " \t\n"))) {
		if (!*str) {
			continue;
		}

		#define KEYWORD(keyword, member, val) \
			else if (strcmp(str, (keyword)) == 0) { \
				service->member = (val); \
			}

		if (0) {}

		KEYWORD("nostart",    on_start,             false)
		KEYWORD("shutdown",   on_stop,              true )
		KEYWORD("resume",     on_resume,            true )

		KEYWORD("firstboot",  first_boot,           true )

		KEYWORD("nojail",     disable_in_jail,      true )
		KEYWORD("nojailvnet", disable_in_vnet_jail, true )

		else {
			WARN("Unknown research Unix-style service keyword '%s'\n", str)
		}

		#undef KEYWORD
	}

	// free everything

	fclose(fp);

	#define FREE(thing) \
		if ((thing)) { \
			free((thing)); \
		}

	FREE(require)
	FREE(provide)
	FREE(before )
	FREE(keyword)

	#undef FREE

	return 0;
}

static void del_service(service_t* service) {
	if (!service) {
		return;
	}

	#define FREE(thing) \
		if ((thing)) { \
			free((thing)); \
		}

	for (size_t i = 0; i < service->deps_len; i++) {
		FREE(service->dep_names[i])
	}

	if (service->deps) {
		FREE(service->deps)
	}

	if (service->kind == SERVICE_KIND_RESEARCH) {
		for (size_t i = 0; i < service->research.provides_len; i++) {
			FREE(service->research.provides[i])
		}

		if (service->research.provides) {
			FREE(service->research.provides)
		}
	}

	FREE(service->name)
	FREE(service->path)

	#undef FREE
	free(service);
}

static inline int __wait_for_process(pid_t pid) {
	// TODO this is really a function that seems like it should be native to aquaBSD

	int status = 0;
	while (waitpid(pid, &status, 0) > 0);

	if (WIFSIGNALED(status)) {
		return -1;
	}

	if (WIFEXITED(status)) {
		return WEXITSTATUS(status);
	}

	return 0;
}

static int service_thread(void* _service) {
	service_t* service = _service;

	// wait for dependencies to finish

	for (size_t i = 0; i < service->deps_len; i++) {
		service_t* dep = service->deps[i];

		int dep_rv = 0;
		thrd_join(dep->thread, &dep_rv);

		if (dep_rv) {
			return dep_rv;
		}
	}

	// create new process for service in question

	service->pid = fork();

	if (!service->pid) {
		//execlp(service->path, service->path /* TODO extra arguments? */);
		FATAL_ERROR("Failed to run %s service at '%s'\n", service->name, service->path)
	}

	// wait for process to finish up

	int rv = __wait_for_process(service->pid);

	if (rv < 0) {
		WARN("Something went wrong running the %s service at '%s'\n", service->name, service->path)
	}

	return rv;
}

static bool research_service_provides(service_t* service, const char* name) {
	if (service->kind != SERVICE_KIND_RESEARCH) {
		return false;
	}

	for (size_t i = 0; i < service->research.provides_len; i++) {
		char* provide = service->research.provides[i];

		if (strcmp(provide, name) == 0) {
			return true;
		}
	}

	return false;
}

static service_t* search_services(size_t services_len, service_t** services, const char* name) {
	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		if (strcmp(service->name, name) == 0) {
			return service;
		}

		if (research_service_provides(service, name)) {
			return service;
		}
	}

	// couldn't find service!

	return NULL;
}

static bool check_circular(service_t* service) {
	if (service->check_circular_passed) {
		return true;
	}

	service->check_circular_passed = true;

	for (size_t i = 0; i < service->deps_len; i++) {
		service_t* dep = service->deps[i];

		if (check_circular(dep)) {
			return true;
		}
	}

	service->check_circular_passed = false;

	return false;
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

	// actually start launching processes

	LOG("aquaBSD init\n")

	// NOTES (cf. rc.d(8)):
	//  - autoboot=yes/rc_fast=yes for skipping checks and speeding stuff up
	//  - skip everything with the 'nostart' keyword (and 'firstboot' if it's the first time the system is booting after installation - this is for services such as 'growfs' for resizing the root filesystem if it wasn't already done by the installer)
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

		char* path;
		asprintf(&path, "/etc/rc.d/%s", entry->d_name);

		struct stat sb;

		if (stat(path, &sb) < 0) {
			free(path);
			FATAL_ERROR("stat(\"%s\"): %s\n", path, strerror(errno))
		}

		mode_t permissions = sb.st_flags & 0777;

		if (permissions == 0555) {
			free(path);
			FATAL_ERROR("\"%s\" doesn't have the right permissions ('0%o', needs '0555')\n", entry->d_name, permissions)
		}

		// okay! add the service

		service_t* service = new_service(entry->d_name);
		service->path = path;

		services = realloc(services, ++services_len * sizeof *services);
		services[services_len - 1] = service;

		fill_research_service(service);
	}

	// resolve service dependencies (this is where we build the dependency graph)
	// yeah, this code has a pretty horrid time complexity  O(n^2)), can absolutely do better (O(n) ideally assuming no hashmap collisions)

	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		service->deps = malloc(service->deps_len * sizeof *service->deps);

		for (size_t j = 0; j < service->deps_len; j++) {
			char* name = service->dep_names[j];
			service->deps[j] = search_services(services_len, services, name);
		}
	}

	// check for circular dependencies

	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		if (check_circular(service)) {
			FATAL_ERROR("Found circular dependency")
		}
	}

	// launch each service we need on startup ('service_t.on_start == true')

	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		if (!service->on_start || service->first_boot) {
			continue;
		}

		thrd_create(&service->thread, service_thread, service);
	}

	// setup message queue notification signal
	// thanks @qookie 😄

	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);

	sigprocmask(SIG_BLOCK, &set, NULL);

	// block while waiting for messages on the message queue

	while (1) {
		siginfo_t info;
		sigwaitinfo(&set, &info);

		// received a message, run a bunch of sanity checks on it

		if (info.si_mqd != mq) {
			continue;
		}

		uid_t uid = info.si_uid;

		// read message data & process it

		char buf[256]; // TODO this will end up being some kind of command strutcure
		__attribute__((unused)) int priority; // we don't care about priority

	retry: {} // fight me, this is more readable than a loop

		ssize_t len = mq_receive(mq, buf, sizeof buf, &priority);

		if (errno == EAGAIN) {
			goto retry;
		}

		if (errno == ETIMEDOUT) {
			WARN("Receiving on the message queue timed out")
			continue;
		}

		if (len < 0) {
			WARN("mq_receive: %s", strerror(errno));
		}

		// TODO process command somehow
	}

	// launch each service we need on shutdown ('service_t.on_stop == true')

	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		if (!service->on_stop) {
			continue;
		}

		// TODO start threads and whatever
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