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
#include <time.h>
#include <unistd.h>

#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <grp.h>
#include <mqueue.h>
#include <libutil.h>

#include <umber.h>
#define UMBER_COMPONENT "MOTHER"

#define FATAL_ERROR(...) \
	LOG_FATAL(__VA_ARGS__); \
	exit(EXIT_FAILURE);

#define MQ_NAME "/init"
#define SERVICE_GROUP "service" // TODO find a more creative name

#define MAX_MESSAGES 10 // not yet sure what this signifies
#define MESSAGE_SIZE 256

#define INIT_ROOT "conf/init/"
#define MOD_DIR INIT_ROOT "mods/"

typedef enum {
	SERVICE_KIND_GENERIC,
	SERVICE_KIND_RESEARCH, // for research UNIX-style runcom scripts
	SERVICE_KIND_AQUABSD,
} service_kind_t;

typedef struct {
	size_t provides_len;
	char** provides;
} service_research_t;

typedef int (*aquabsd_start_func_t) (void);

typedef struct {
	void* lib;
	aquabsd_start_func_t start;
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
	// unfortunately, the C11 standard doesn't seem to support condition values/mutices, which basically makes it useless
	// TODO the above comment *seems* to be false? Documentation is lacking, but I might aswell switch back to C11 threads if I can

	bool thread_created;
	pthread_t thread;
	pthread_mutex_t mutex;
	pid_t pid;

	// timing stuff

	long double start_time;
	long double total_time;

	// kind-specific members

	union {
		service_research_t research;
		service_aquabsd_t aquabsd;
	};
};

static inline long double __get_time(void) {
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	return (long double) now.tv_sec + 1.e-9 * (long double) now.tv_nsec;
}

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
			LOG_WARN("Unknown research UNIX-style service keyword '%s'", str)
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

	LOG_VERBOSE("Filled research UNIX-style service %s", service->name)

	return 0;
}

typedef size_t (*get_deps_len_func_t)  (void);
typedef char** (*get_dep_names_func_t) (void);

static int fill_aquabsd_service(service_t* service) {
	service->kind = SERVICE_KIND_AQUABSD;

	// we're using 'RTLD_NOW' here instead of 'RTLD_LAZY' as would normally be preferred
	// since we only have a small number of functions that we know we'll eventually use, it's better to resolve all external symbols straight away

	service->aquabsd.lib = dlopen(service->path, RTLD_NOW);

	if (!service->aquabsd.lib) {
		LOG_WARN("dlopen: failed to load %s: %s", service->path, dlerror())
		return -1;
	}

	dlerror(); // clear last error

	// get start function, duh

	service->aquabsd.start = dlsym(service->aquabsd.lib, "start");

	if (!service->aquabsd.start) {
		LOG_WARN("aquaBSD services must have start symbol")

		dlclose(service->aquabsd.lib);
		return -1;
	}

	// get dependencies

	get_deps_len_func_t  get_deps_len  = dlsym(service->aquabsd.lib, "get_deps_len" );
	get_dep_names_func_t get_dep_names = dlsym(service->aquabsd.lib, "get_dep_names");

	if (!get_deps_len || !get_dep_names) {
		LOG_WARN("aquaBSD services must have get_deps_len & get_dep_names symbols")

		dlclose(service->aquabsd.lib);
		return -1;
	}

	service->deps_len  = get_deps_len ();
	service->dep_names = get_dep_names();

	// get service flags

	#define FLAG(flag) \
		if (dlsym(service->aquabsd.lib, #flag)) { \
			service->flag = true; \
		}

	FLAG(on_start            )
	FLAG(on_stop             )
	FLAG(on_resume           )

	FLAG(first_boot          )

	FLAG(disable_in_jail     )
	FLAG(disable_in_vnet_jail)

	LOG_VERBOSE("Filled aquaBSD service %s", service->name)

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

	else if (service->kind == SERVICE_KIND_AQUABSD) {
		dlclose(service->aquabsd.lib);
	}

	FREE(service->name)
	FREE(service->path)

	#undef FREE
	free(service);
}

static inline int __wait_for_process(pid_t pid) {
	// TODO this is really a function that seems like it should be native to aquaBSD
	//      also, using a spinlock is probably not the best of ideas either

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

static void* service_thread(void* _service) {
	service_t* service = _service;

	// TODO what if a dependant process somehow manages to lock the mutex before us?

	pthread_mutex_lock(&service->mutex);
	LOG_VERBOSE("%s waiting for dependencies to complete", service->name)

	// wait for dependencies to complete

	for (size_t i = 0; i < service->deps_len; i++) {
		service_t* dep = service->deps[i];

		if (!dep) {
			continue;
		}

		// wait for mutex to be unlocked by attempting to lock it and then instantly unlocking it

		if (!dep->mutex) {
			continue;
		}

		pthread_mutex_lock(&dep->mutex);
		pthread_mutex_unlock(&dep->mutex);
	}

	// record start time

	LOG_INFO("Starting %s", service->name)
	service->start_time = __get_time();

	// create new process for service in question

	service->pid = fork();

	if (!service->pid) {
		if (service->kind == SERVICE_KIND_RESEARCH) {
			// we don't care about freeing this

			char* call;
			asprintf(&call, ". /etc/rc.subr && run_rc_script %s faststart", service->path);

			execlp("sh", "sh", "-c", call, NULL);
		}

		else if (service->kind == SERVICE_KIND_AQUABSD) {
			_exit(service->aquabsd.start());
		}

		else {
			// TODO
		}

		_exit(EXIT_FAILURE);
	}

	// wait for process to finish up

	int rv = __wait_for_process(service->pid);

	if (rv) {
		LOG_WARN("Something went wrong running the %s service at '%s'", service->name, service->path)
	}

	pthread_mutex_unlock(&service->mutex);
	LOG_SUCCESS("Completed %s", service->name)

	// compute total time service took

	long double now = __get_time();
	service->total_time = now - service->start_time;

	return (void*) (uint64_t) rv;
}

static void start_on_start_services(size_t services_len, service_t** services) {
	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		if (!service) {
			continue;
		}

		if (!service->on_start || service->first_boot) {
			continue;
		}

		if (service->thread_created) {
			continue;
		}

		service->thread_created = true;

		// go as far down the tree as we can before actually starting services
		// this is so that we can join the threads of dependencies in 'service_thread' while waiting for them

		start_on_start_services(service->deps_len, service->deps);

		pthread_mutex_init(&service->mutex, NULL);
		pthread_create(&service->thread, NULL, service_thread, service);

		LOG_VERBOSE("Thread created for %s: %p", service->name, service->thread)
	}
}

static void join_services(size_t services_len, service_t** services) {
	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		if (!service) {
			continue;
		}

		if (!service->thread_created) {
			continue;
		}


		__attribute__((unused)) void* rv = NULL;
		pthread_join(service->thread, &rv);
	}
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
	// returns true if circular dependencies found
	// returns false otherwise

	if (!service) {
		return false;
	}

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
		LOG_ERROR("Unknown option (%s)", option)
	}

	// make sure we're root

	uid_t uid = getuid();

	if (uid) {
		FATAL_ERROR("Must be run as root user 😢 (UID = 0, current UID = %d)", uid)
	}

	// make sure the $SERVICE_GROUP group exists, and error if not

	struct group* service_group = getgrnam(SERVICE_GROUP);

	if (!service_group) {
		FATAL_ERROR("Couldn't find \"" SERVICE_GROUP "\" group");
	}

	gid_t service_gid = service_group->gr_gid;
	endgrent();

	// make sure a message queue named $MQ_NAME doesn't already exist to ensure only one instance of init is running at a time

	mode_t permissions = 0420; // owner ("root") can only read, group ($SERVICE_GROUP) can only write, and others can do neither

	if (mq_open(MQ_NAME, O_CREAT | O_EXCL, permissions, NULL /* defaults are probably fine */) < 0 && errno == EEXIST) {
		FATAL_ERROR("Only one instance of init may be running at a time 😢")
	}

	// create message queue

	mqd_t mq = mq_open(MQ_NAME, O_CREAT | O_RDWR, permissions, NULL);

	if (mq < 0) {
		FATAL_ERROR("mq_open(\"" MQ_NAME "\"): %s", strerror(errno))
	}

	// set group ownership to the $SERVICE_GROUP group

	// if (fchown(mq, uid /* most likely gonna be root */, service_gid) < 0) {
	// 	FATAL_ERROR("fchown: %s\n", strerror(errno))
	// }

	// actually start launching processes

	LOG_INFO("MOTHER\n")

	// NOTES (cf. rc.d(8)):
	//  - autoboot=yes/rc_fast=yes for skipping checks and speeding stuff up
	//  - skip everything with the 'nostart' keyword (and 'firstboot' if it's the first time the system is booting after installation - this is for services such as 'growfs' for resizing the root filesystem if it wasn't already done by the installer)
	//  - execute services until $early_late_divider is reached ('early_late_divider=FILESYSTEMS', ensure root & "critical" filesystems are mounted basically)
	//  - check the firstboot again (incase we've moved to a different fs)
	//  - delete $firstboot_sentinel (& $firstboot_sentinel"-reboot" if that exists, in which case reboot)

	size_t services_len = 0;
	service_t** services = NULL;

	// read all the aquaBSD services in '/etc/init/services'

	DIR* dp = opendir("/etc/init/services");

	if (!dp) {
		FATAL_ERROR("opendir(\"/etc/init/services\"): %s", strerror(errno))
	}

	struct dirent* ent;

	while ((ent = readdir(dp))) {
		if (ent->d_type != DT_REG) {
			continue; // anything other than a regular file is invalid
		}

		if (*ent->d_name == '.') {
			continue; // don't care about '.', '..', & other entries starting with a dot
		}

		char* path;
		asprintf(&path, "/etc/init/services/%s", ent->d_name);

		// okay! add the service

		service_t* service = new_service(ent->d_name);
		service->path = path;

		if (fill_aquabsd_service(service) < 0) {
			continue;
		}

		services = realloc(services, ++services_len * sizeof *services);
		services[services_len - 1] = service;
	}

	closedir(dp);

	// read all the legacy research UNIX-style services in '/etc/rc.d'

	/* DIR* */ dp = opendir("/etc/rc.d");

	if (!dp) {
		FATAL_ERROR("opendir(\"/etc/rc.d\"): %s", strerror(errno))
	}

	while ((ent = readdir(dp))) {
		if (ent->d_type != DT_REG) {
			continue; // anything other than a regular file is invalid
		}

		if (*ent->d_name == '.') {
			continue; // don't care about '.', '..', & other entries starting with a dot
		}

		char* path;
		asprintf(&path, "/etc/rc.d/%s", ent->d_name);

		struct stat sb;

		if (stat(path, &sb) < 0) {
			free(path);
			FATAL_ERROR("stat(\"%s\"): %s", path, strerror(errno))
		}

		mode_t permissions = sb.st_flags & 0777;

		if (permissions == 0555) {
			free(path);
			FATAL_ERROR("\"%s\" doesn't have the right permissions ('0%o', needs '0555')", ent->d_name, permissions)
		}

		// okay! add the service

		service_t* service = new_service(ent->d_name);
		service->path = path;

		if (fill_research_service(service) < 0) {
			continue;
		}

		services = realloc(services, ++services_len * sizeof *services);
		services[services_len - 1] = service;
	}

	closedir(dp);

	// resolve service dependencies (this is where we build the dependency graph)
	// yeah, this code has a pretty horrid time complexity  O(n^2), can absolutely do better (O(n) ideally assuming no hashmap collisions)

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

	long double start_time = __get_time();
	start_on_start_services(services_len, services);

	// join all services to exit out of init

	join_services(services_len, services);

	// print out timing information and exit

	long double now = __get_time();
	LOG_INFO("Took %Lf seconds", now - start_time)

	char* longest_name = "unknown";
	long double longest_time = 0.0;

	for (size_t i = 0; i < services_len; i++) {
		service_t* service = services[i];

		if (!service) {
			continue;
		}

		if (service->total_time < longest_time) {
			continue;
		}

		longest_name = service->name;
		longest_time = service->total_time;
	}

	LOG_INFO("Longest service to complete was %s, at %Lf seconds", longest_name, longest_time)

	exit(0);

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
		// TODO getting some weird warning saying I can't compare info.si_mqd (int) and mq (mqd_t)

		if (info.si_mqd != mq) {
			continue;
		}

		uid_t uid = info.si_uid;

		// read message data & process it

		char buf[256]; // TODO this will end up being some kind of command structure
		__attribute__((unused)) unsigned priority; // we don't care about priority

	retry: {} // fight me, this is more readable than a loop

		ssize_t len = mq_receive(mq, buf, sizeof buf, &priority);

		if (errno == EAGAIN) {
			goto retry;
		}

		if (errno == ETIMEDOUT) {
			LOG_WARN("Receiving on the message queue timed out")
			continue;
		}

		if (len < 0) {
			LOG_WARN("mq_receive: %s", strerror(errno))
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
