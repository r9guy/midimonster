#ifndef MIDIMONSTER_HEADER
#define MIDIMONSTER_HEADER
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

/* Core version unless set by the build process */
#ifndef MIDIMONSTER_VERSION
	#define MIDIMONSTER_VERSION "v0.7-dist"
#endif

/* Set backend name if unset */
#ifndef BACKEND_NAME
	#define BACKEND_NAME "unspec"
#endif

/* API call attributes and visibilities */
#ifndef MM_API
	#ifdef _WIN32
		#define MM_API __attribute__((dllimport))
	#else
		#define MM_API
	#endif
#endif

/* Some build systems may apply the -fvisibility=hidden parameter from the core build to the backends, so mark the init function visible */
#ifndef MM_PLUGIN_API
	#ifdef _WIN32
		#define MM_PLUGIN_API __attribute__((dllexport))
	#else
		#define MM_PLUGIN_API __attribute__((visibility ("default")))
	#endif
#endif

/* Straight-forward min / max macros */
#define max(a,b) (((a) > (b)) ? (a) : (b))
#define min(a,b) (((a) < (b)) ? (a) : (b))

/* Clamp a value to a range */
#define clamp(val,max,min) (((val) > (max)) ? (max) : (((val) < (min)) ? (min) : (val)))

/* Log function prototype - do not use directly. Use the LOG/LOGPF/DBGPF macros below instead */
MM_API int log_printf(int level, char* module, char* fmt, ...);

/* Debug messages only compile in when DEBUG is set */
#ifdef DEBUG
	#define DBGPF(format, ...) log_printf(1, (BACKEND_NAME), format "\n", __VA_ARGS__)
#else
	#define DBGPF(format, ...)
#endif

/* Log messages should be routed through these macros to ensure interoperability with different core implementations */
#define LOGPF(format, ...) log_printf(0, (BACKEND_NAME), format "\n", __VA_ARGS__)
#define LOG(message) log_printf(0, (BACKEND_NAME), message "\n")

/* Stop compilation if the build system reports an error */
#ifdef BUILD_ERROR
	#error The build system reported an error, compilation stopped. Refer to the invocation for this compilation unit for more information.
#endif

/* Pull in additional defines for non-linux platforms */
#include "portability.h"

/* Default configuration file name to read when no other is specified */
#ifndef DEFAULT_CFG
	#define DEFAULT_CFG "monster.cfg"
#endif

/* Default backend plugin location */
#ifndef PLUGINS
	#ifndef _WIN32
		#define PLUGINS "./backends/"
	#else
		#define PLUGINS "backends\\"
	#endif
#endif

/* Forward declare some of the structs so we can use them in each other */
struct _channel_value;
struct _backend_channel;
struct _backend_instance;
struct _managed_fd;

/*
 * Backend module callback defines
 *
 * The lifecycle of a backend module is as follows
 * 	* int init()
 * 		The only function that should be exported by the shared object.
 * 		Called when the shared object is attached. Should register
 * 		a backend structure containing callable entry points with the core.
 * 		Returning anything other than zero causes midimonster to fail the
 * 		startup checks.
 * 	* mmbackend_configure
 * 		Parse backend-global configuration options from the user-supplied
 * 		configuration file. Returning a non-zero value fails config parsing.
 * 	* mmbackend_instance
 * 		Allocate the backend-specific data parts of the supplied instance
 * 		structure. Returning non-zero signals an error condition and
 * 		terminates the program.
 * 	* mmbackend_configure_instance
 * 		Parse instance configuration from the user-supplied configuration
 * 		file. Returning a non-zero value fails config parsing.
 * 	* mmbackend_channel
 * 		Parse a channel-spec to be mapped to/from. The `flags` parameter supplies
 * 		additional information to the parser, such as whether the channel is being
 * 		queried for use as input (to the MIDIMonster core) and/or output
 * 		(from the MIDIMonster core) channel (on a per-query basis).
 * 		Returning NULL signals an out-of-memory condition and terminates the program.
 * 	* mmbackend_start
 * 		Called after all instances have been created and all mappings
 * 		have been set up. Only backends for which instances have been configured
 * 		receive the start call. May be used to connect to backing hardware
 * 		or to update runtime-specific data in the various data structures.
 * 		Returning a non-zero value signals an error starting the backend
 * 		and stops further progress.
 * 	* Normal processing loop starts here
 * 		* mmbackend_process_fd
 * 			Handle data from signaled fds registered via mm_manage_fd.
 * 			Push generated events to the core with mm_channel_event.
 * 			All registered fds that are ready to read are pushed at once.
 * 			Backends that have not registered any fds are still called with
 * 			nfds set to 0 in order to support polling backends.
 * 			Returning a non-zero value signals an error and gracefully terminates
 * 			the program.
 *		* mmbackend_handle_event
 *			An event resulted in a channel for an instance being set.
 *			Called once per changed instance with all updated channels for that
 *			specific instance.
 *			Returning a non-zero value terminates the program.
 *		* (optional) mmbackend_interval
 *			Return the maximum sleep interval for this backend in milliseconds.
 *			If not implemented, a maximum interval of one second is used.
 *			Returning 0 signals that the backend does not have a minimum
 *			interval.
 *	* mmbackend_shutdown
 *		Clean up all allocations, finalize all hardware connections. All registered
 *		backends receive the shutdown call, regardless of whether they have been
 *		started previously.
 *		Return value is currently ignored.
 */
typedef int (*mmbackend_handle_event)(struct _backend_instance* inst, size_t channels, struct _backend_channel** c, struct _channel_value* v);
typedef int (*mmbackend_create_instance)(struct _backend_instance* inst);
typedef struct _backend_channel* (*mmbackend_parse_channel)(struct _backend_instance* instance, char* spec, uint8_t flags);
typedef void (*mmbackend_free_channel)(struct _backend_channel* c);
typedef int (*mmbackend_configure)(char* option, char* value);
typedef int (*mmbackend_configure_instance)(struct _backend_instance* instance, char* option, char* value);
typedef int (*mmbackend_process_fd)(size_t nfds, struct _managed_fd* fds);
typedef int (*mmbackend_start)(size_t ninstances, struct _backend_instance** inst);
typedef uint32_t (*mmbackend_interval)();
typedef int (*mmbackend_shutdown)(size_t ninstances, struct _backend_instance** inst);

/* Bit masks for the `flags` parameter to mmbackend_parse_channel */
typedef enum _mmbe_channel_flags{
	mmchannel_input = 0x1,
	mmchannel_output = 0x2
} mmbe_channel_flags;

/* Channel event value, .normalised is used by backends to determine channel values */
typedef struct _channel_value {
	union {
		double dbl;
		uint64_t u64;
	} raw;
	double normalised;
} channel_value;

/* 
 * Backend callback structure
 * Used to register a backend with the core using mm_backend_register()
 */
typedef struct /*_mm_backend*/ {
	char* name;
	mmbackend_configure conf;
	mmbackend_create_instance create;
	mmbackend_configure_instance conf_instance;
	mmbackend_parse_channel channel;
	mmbackend_handle_event handle;
	mmbackend_process_fd process;
	mmbackend_start start;
	mmbackend_shutdown shutdown;
	mmbackend_free_channel channel_free;
	mmbackend_interval interval;
} backend;

/* 
 * Backend instance structure - do not allocate directly!
 * Use the memory returned by mm_instance()
 */
typedef struct _backend_instance {
	backend* backend;
	uint64_t ident;
	void* impl;
	char* name;
} instance;

/* 
 * Instance channel structure
 * Backends may either manage their own channel registry or use the global
 * channel store via the mm_channel() API
 */
typedef struct _backend_channel {
	instance* instance;
	uint64_t ident;
	void* impl;
} channel;

/*
 * File descriptor structure passed for backend handling
 * Register for the core event loop using mm_manage_fd()
 */
typedef struct _managed_fd {
	int fd;
	backend* backend;
	void* impl;
} managed_fd;

/*
 * Register a new backend.
 */
MM_API int mm_backend_register(backend b);

/*
 * Finds an instance matching the specified backend and identifier.
 * Since setting an identifier for an instance is optional, this may not work
 * depending on the backend. Instance identifiers may for example be set in the
 * backends mmbackend_start call.
 */
MM_API instance* mm_instance_find(char* backend, uint64_t ident);

/*
 * This function is the main interface to the core-provided channel registry.
 * This API is just a convenience function. Creating and managing a
 * backend-internal channel store is possible (and encouraged for performance
 * reasons).
 *
 * Channels are identified by the (instance, ident) tuple within the registry.
 *
 * This API provides a pointer to a channel structure, pre-filled with the
 * provided instance reference and identifier.
 * The `create` parameter is a boolean flag indicating whether a channel
 * matching the `ident` parameter should be created in the global channel store
 * if none exists yet. If the instance already registered a channel matching
 * `ident`, a pointer to the existing channel is returned.
 * 
 * When returning pointers from a backend-local channel store, the
 * returned pointers must stay valid over the lifetime of the instance and
 * provide valid `instance` members, as they are used for callbacks.
 * For each channel with a non-NULL `impl` field registered using
 * this function, the backend will receive a call to its channel_free
 * function (if it exists).
 */
MM_API channel* mm_channel(instance* i, uint64_t ident, uint8_t create);

/*
 * When using the core-provided channel registry, the identification
 * member of the structure must only be updated using this API.
 * The tuple of (instance, ident) is used as key to the backing
 * storage of the channel registry, thus the registry must be notified
 * of changes.
 */
MM_API void mm_channel_update(channel* c, uint64_t ident);

/*
 * Register (manage = 1) or unregister (manage = 0) a file descriptor to be
 * selected on. The backend will be notified when the descriptor becomes ready
 * to read via its registered mmbackend_process_fd call. The `impl` argument
 * will be provided within the corresponding managed_fd structure upon callback.
 */
MM_API int mm_manage_fd(int fd, char* backend, int manage, void* impl);

/*
 * Notifies the core of a channel event. Called by backends to inject events
 * gathered from their backing implementation.
 */
MM_API int mm_channel_event(channel* c, channel_value v);

/*
 * Query all active instances for a given backend.
 * *i will need to be freed by the caller.
 */
MM_API int mm_backend_instances(char* backend, size_t* n, instance*** i);

/*
 * Query an internal timestamp, which is updated every core iteration.
 * This timestamp should not be used as a performance counter, but can be used
 * for timeouting. Resolution is milliseconds.
 */
MM_API uint64_t mm_timestamp();

/*
 * Create a channel-to-channel mapping. This API should not be used by backends.
 * It is only exported for core modules.
 */
int mm_map_channel(channel* from, channel* to);
#endif
