#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#ifndef _WIN32
#include <sys/select.h>
#define MM_API __attribute__((visibility("default")))
#else
#define MM_API __attribute__((dllexport))
#endif
#define BACKEND_NAME "core"
#include "midimonster.h"
#include "config.h"
#include "backend.h"
#include "plugin.h"

/* Core-internal structures */
typedef struct /*_event_collection*/ {
	size_t alloc;
	size_t n;
	channel** channel;
	channel_value* value;
} event_collection;

typedef struct /*_mm_channel_mapping*/ {
	channel* from;
	size_t destinations;
	channel** to;
} channel_mapping;

static size_t mappings = 0;
static channel_mapping* map = NULL;
static size_t fds = 0;
static managed_fd* fd = NULL;
static volatile sig_atomic_t fd_set_dirty = 1;
static uint64_t global_timestamp = 0;

static event_collection event_pool[2] = {
	{0},
	{0}
};
static event_collection* primary = event_pool;

volatile static sig_atomic_t shutdown_requested = 0;

static void signal_handler(int signum){
	shutdown_requested = 1;
}

MM_API uint64_t mm_timestamp(){
	return global_timestamp;
}

static void update_timestamp(){
	#ifdef _WIN32
	global_timestamp = GetTickCount();
	#else
	struct timespec current;
	if(clock_gettime(CLOCK_MONOTONIC_COARSE, &current)){
		fprintf(stderr, "Failed to update global timestamp, time-based processing for some backends may be impaired: %s\n", strerror(errno));
		return;
	}

	global_timestamp = current.tv_sec * 1000 + current.tv_nsec / 1000000;
	#endif
}

int mm_map_channel(channel* from, channel* to){
	size_t u, m;
	//find existing source mapping
	for(u = 0; u < mappings; u++){
		if(map[u].from == from){
			break;
		}
	}

	//create new entry
	if(u == mappings){
		map = realloc(map, (mappings + 1) * sizeof(channel_mapping));
		if(!map){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		memset(map + mappings, 0, sizeof(channel_mapping));
		mappings++;
		map[u].from = from;
	}

	//check whether the target is already mapped
	for(m = 0; m < map[u].destinations; m++){
		if(map[u].to[m] == to){
			return 0;
		}
	}

	map[u].to = realloc(map[u].to, (map[u].destinations + 1) * sizeof(channel*));
	if(!map[u].to){
		fprintf(stderr, "Failed to allocate memory\n");
		map[u].destinations = 0;
		return 1;
	}

	map[u].to[map[u].destinations] = to;
	map[u].destinations++;
	return 0;
}

static void map_free(){
	size_t u;
	for(u = 0; u < mappings; u++){
		free(map[u].to);
	}
	free(map);
	mappings = 0;
	map = NULL;
}

MM_API int mm_manage_fd(int new_fd, char* back, int manage, void* impl){
	backend* b = backend_match(back);
	size_t u;

	if(!b){
		fprintf(stderr, "Unknown backend %s registered for managed fd\n", back);
		return 1;
	}

	//find exact match
	for(u = 0; u < fds; u++){
		if(fd[u].fd == new_fd && fd[u].backend == b){
			fd[u].impl = impl;
			if(!manage){
				fd[u].fd = -1;
				fd[u].backend = NULL;
				fd[u].impl = NULL;
				fd_set_dirty = 1;
			}
			return 0;
		}
	}

	if(!manage){
		return 0;
	}

	//find free slot
	for(u = 0; u < fds; u++){
		if(fd[u].fd < 0){
			break;
		}
	}
	//if necessary expand
	if(u == fds){
		fd = realloc(fd, (fds + 1) * sizeof(managed_fd));
		if(!fd){
			fprintf(stderr, "Failed to allocate memory\n");
			return 1;
		}
		fds++;
	}

	//store new fd
	fd[u].fd = new_fd;
	fd[u].backend = b;
	fd[u].impl = impl;
	fd_set_dirty = 1;
	return 0;
}

static void fds_free(){
	size_t u;
	for(u = 0; u < fds; u++){
		//TODO free impl
		if(fd[u].fd >= 0){
			close(fd[u].fd);
			fd[u].fd = -1;
		}
	}
	free(fd);
	fds = 0;
	fd = NULL;
}

MM_API int mm_channel_event(channel* c, channel_value v){
	size_t u, p;

	//find mapped channels
	for(u = 0; u < mappings; u++){
		if(map[u].from == c){
			break;
		}
	}

	if(u == mappings){
		//target-only channel
		return 0;
	}

	//resize event structures to fit additional events
	if(primary->n + map[u].destinations >= primary->alloc){
		primary->channel = realloc(primary->channel, (primary->alloc + map[u].destinations) * sizeof(channel*));
		primary->value = realloc(primary->value, (primary->alloc + map[u].destinations) * sizeof(channel_value));

		if(!primary->channel || !primary->value){
			fprintf(stderr, "Failed to allocate memory\n");
			primary->alloc = 0;
			primary->n = 0;
			return 1;
		}

		primary->alloc += map[u].destinations;
	}

	//enqueue channel events
	//FIXME this might lead to one channel being mentioned multiple times in an apply call
	for(p = 0; p < map[u].destinations; p++){
		primary->channel[primary->n + p] = map[u].to[p];
		primary->value[primary->n + p] = v;
	}

	primary->n += map[u].destinations;
	return 0;
}

static void event_free(){
	size_t u;

	for(u = 0; u < sizeof(event_pool) / sizeof(event_collection); u++){
		free(event_pool[u].channel);
		free(event_pool[u].value);
		event_pool[u].alloc = 0;
	}
}

static void version(){
	printf("MIDIMonster %s\n", MIDIMONSTER_VERSION);
}

static int usage(char* fn){
	version();
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, "\t%s <configfile>\n", fn);
	return EXIT_FAILURE;
}

static fd_set fds_collect(int* max_fd){
	size_t u = 0;
	fd_set rv_fds;

	if(max_fd){
		*max_fd = -1;
	}

	DBGPF("Building selector set from %lu FDs registered to core\n", fds);
	FD_ZERO(&rv_fds);
	for(u = 0; u < fds; u++){
		if(fd[u].fd >= 0){
			FD_SET(fd[u].fd, &rv_fds);
			if(max_fd){
				*max_fd = max(*max_fd, fd[u].fd);
			}
		}
	}

	return rv_fds;
}

static int platform_initialize(){
	#ifdef _WIN32
	WSADATA wsa;
	WORD version = MAKEWORD(2, 2);
	if(WSAStartup(version, &wsa)){
		return 1;
	}
	#endif
	return 0;
}

static int platform_shutdown(){
	#ifdef _WIN32
	DWORD processes;
	if(GetConsoleProcessList(&processes, 1) == 1){
		fprintf(stderr, "\nMIDIMonster is the last process in this console, please press any key to exit\n");
		HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
		SetConsoleMode(input, 0);
		FlushConsoleInputBuffer(input);
		WaitForSingleObject(input, INFINITE);
	}
	#endif
	return 0;
}

static int args_parse(int argc, char** argv, char** cfg_file){
	size_t u;
	for(u = 1; u < argc; u++){
		if(!strcmp(argv[u], "-v") || !strcmp(argv[u], "--version")){
			version();
			return 1;
		}
		else if(!strcmp(argv[u], "-i")){
			if(!argv[u + 1]){
				fprintf(stderr, "Missing instance override specification\n");
				return 1;
			}
			if(config_add_override(override_instance, argv[u + 1])){
				return 1;
			}
			u++;
		}
		else if(!strcmp(argv[u], "-b")){
			if(!argv[u + 1]){
				fprintf(stderr, "Missing backend override specification\n");
				return 1;
			}
			if(config_add_override(override_backend, argv[u + 1])){
				return 1;
			}
			u++;
		}
		else{
			//if nothing else matches, it's probably the configuration file
			*cfg_file = argv[u];
		}
	}

	return 0;
}

static int core_process(size_t nfds, managed_fd* signaled_fds){
	event_collection* secondary = NULL;
	size_t u;

	//run backend processing, collect events
	DBGPF("%lu backend FDs signaled\n", nfds);
	if(backends_handle(nfds, signaled_fds)){
		return 1;
	}

	while(primary->n){
		//swap primary and secondary event collectors
		DBGPF("Swapping event collectors, %lu events in primary\n", primary->n);
		for(u = 0; u < sizeof(event_pool) / sizeof(event_collection); u++){
			if(primary != event_pool + u){
				secondary = primary;
				primary = event_pool + u;
				break;
			}
		}

		//push collected events to target backends
		if(secondary->n && backends_notify(secondary->n, secondary->channel, secondary->value)){
			fprintf(stderr, "Backends failed to handle output\n");
			return 1;
		}

		//reset the event count
		secondary->n = 0;
	}

	return 0;
}

static int core_loop(){
	fd_set all_fds, read_fds;
	managed_fd* signaled_fds = NULL;
	struct timeval tv;
	int error, maxfd = -1;
	size_t n, u;
	#ifdef _WIN32
	char* error_message = NULL;
	#else
	struct timespec ts;
	#endif

	FD_ZERO(&all_fds);

	//process events
	while(!shutdown_requested){
		//rebuild fd set if necessary
		if(fd_set_dirty){
			all_fds = fds_collect(&maxfd);
			signaled_fds = realloc(signaled_fds, fds * sizeof(managed_fd));
			if(!signaled_fds){
				fprintf(stderr, "Failed to allocate memory\n");
				return 1;
			}
			fd_set_dirty = 0;
		}

		//wait for & translate events
		read_fds = all_fds;
		tv = backend_timeout();

		//check whether there are any fds active, windows does not like select() without descriptors
		if(maxfd >= 0){
			error = select(maxfd + 1, &read_fds, NULL, NULL, &tv);
			if(error < 0){
				#ifndef _WIN32
				fprintf(stderr, "select failed: %s\n", strerror(errno));
				#else
				FormatMessage(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, 
						NULL, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPTSTR) &error_message, 0, NULL);
				fprintf(stderr, "select failed: %s\n", error_message);
				LocalFree(error_message);
				error_message = NULL;
				#endif
				free(signaled_fds);
				return 1;
			}
		}
		else{
			DBGPF("No descriptors, sleeping for %zu msec", tv.tv_sec * 1000 + tv.tv_usec / 1000);
			#ifdef _WIN32
			Sleep(tv.tv_sec * 1000 + tv.tv_usec / 1000);
			#else
			ts.tv_sec = tv.tv_sec;
			ts.tv_nsec = tv.tv_usec * 1000;
			error = nanosleep(&ts, NULL);
			#endif
		}

		//update this iteration's timestamp
		update_timestamp();

		//find all signaled fds
		n = 0;
		for(u = 0; u < fds; u++){
			if(fd[u].fd >= 0 && FD_ISSET(fd[u].fd, &read_fds)){
				signaled_fds[n] = fd[u];
				n++;
			}
		}

		//fetch and process events
		if(core_process(n, signaled_fds)){
			free(signaled_fds);
			return 1;
		}
	}

	free(signaled_fds);
	return 0;
}

int main(int argc, char** argv){
	int rv = EXIT_FAILURE;
	char* cfg_file = DEFAULT_CFG;

	//parse commandline arguments
	if(args_parse(argc, argv, &cfg_file)){
		return EXIT_FAILURE;
	}

	if(platform_initialize()){
		fprintf(stderr, "Failed to perform platform-specific initialization\n");
		return EXIT_FAILURE;
	}

	//initialize backends
	if(plugins_load(PLUGINS)){
		fprintf(stderr, "Failed to initialize a backend\n");
		goto bail;
	}

	//read config
	if(config_read(cfg_file)){
		fprintf(stderr, "Failed to read configuration file %s\n", cfg_file);
		backends_stop();
		channels_free();
		instances_free();
		map_free();
		fds_free();
		plugins_close();
		config_free();
		return (usage(argv[0]) | platform_shutdown());
	}

	//load an initial timestamp
	update_timestamp();

	//start backends
	if(backends_start()){
		goto bail;
	}

	signal(SIGINT, signal_handler);

	if(!fds){
		fprintf(stderr, "No descriptors registered for multiplexing\n");
	}

	//run the core loop
	if(!core_loop()){
		rv = EXIT_SUCCESS;
	}

bail:
	//free all data
	backends_stop();
	channels_free();
	instances_free();
	map_free();
	fds_free();
	event_free();
	plugins_close();
	config_free();
	platform_shutdown();

	return rv;
}
