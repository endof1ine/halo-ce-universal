/*
HOST_MAIN.C

Entry point of the Switch port, a homebrew program (halo.nro) started from
the Homebrew Menu in a game's place (hold R while starting a game).

It checks the console gives it what the game needs (host_memory.c), maps
the guest image (the game, built as ILP32 code, in the program's RomFS),
gives it an environment naming where the game data and saves live, and runs
its main() on this thread, with its stack below 4 GB (host_thread.c).

Storage (port/switch/README.md): the game data (maps/) and the settings
(config.toml) are in /switch/halo on the SD card; saves go to its save/
folder; the log of the game is debug.txt there, this file's host.log.
*/

#include "host.h"
#include "tomlc17.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <switch.h>
#include <unistd.h>

void host_syscall_initialize(void);

/* ---------- logging */

static FILE *log_file;
static int log_socket = -1;
static Mutex log_lock;

static void log_line(int priority, const char *text)
{
	static const char *const levels[] = {"", "", "", "", "I", "W", "E", "F"};
	char line[1200];
	int length;
	uint64_t ms = armTicksToNs(armGetSystemTick()) / 1000000ull;

	length = snprintf(line, sizeof(line), "%5llu.%03llu %s %s\n", (unsigned long long)(ms / 1000),
		(unsigned long long)(ms % 1000), priority >= 0 && priority < 8 ? levels[priority] : "?", text);
	if (length < 0)
		return;
	if (length >= (int)sizeof(line))
		length = sizeof(line) - 1;
	mutexLock(&log_lock);
	if (log_file)
	{
		fwrite(line, 1, (size_t)length, log_file);
		/* (a crash ends the process without flushing; warnings and errors
		are worth the write) */
		if (priority >= HOST_LOG_WARN)
			fflush(log_file);
	}
	if (log_socket >= 0)
		send(log_socket, line, (size_t)length, 0);
	mutexUnlock(&log_lock);
}

void host_logf(int priority, const char *format, ...)
{
	char text[1024];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	log_line(priority, text);
}

/* the guest's log (Android priorities) */
void host_log(int priority, const char *text)
{
	log_line(priority, text);
}

static void log_flush(void)
{
	mutexLock(&log_lock);
	if (log_file)
		fflush(log_file);
	mutexUnlock(&log_lock);
}

/* Atmosphère's newest crash report, to the nxlink host: a crash of the
last run, read without taking the SD card out */
static void send_last_crash_report(void)
{
	const char *folder = "sdmc:/atmosphere/crash_reports";
	char newest[256] = "", path[512], buffer[4096];
	struct dirent *entry;
	DIR *directory;
	FILE *file;
	size_t length, total = 0;

	if (log_socket < 0 || !(directory = opendir(folder)))
		return;
	/* (the names start with the time of the crash) */
	while ((entry = readdir(directory)))
		if (strstr(entry->d_name, ".log") && strcmp(entry->d_name, newest) > 0)
			snprintf(newest, sizeof(newest), "%s", entry->d_name);
	closedir(directory);
	if (!*newest)
		return;
	snprintf(path, sizeof(path), "%s/%s", folder, newest);
	if (!(file = fopen(path, "rb")))
		return;
	length = (size_t)snprintf(buffer, sizeof(buffer), "== newest crash report: %s\n", newest);
	send(log_socket, buffer, length, 0);
	while (total < 32 * 1024 && (length = fread(buffer, 1, sizeof(buffer), file)) > 0)
	{
		send(log_socket, buffer, length, 0);
		total += length;
	}
	fclose(file);
	send(log_socket, "\n== end of crash report\n", 25, 0);
}

/* ---------- ending */

void host_fatal(const char *format, ...)
{
	char message[1024];
	ErrorApplicationConfig error;
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(message, sizeof(message), format, arguments);
	va_end(arguments);
	log_line(7, message);
	log_flush();
	host_audio_pause(1);
	host_input_stop_rumble();
	if (R_SUCCEEDED(errorApplicationCreate(&error, message, NULL)))
		errorApplicationShow(&error);
	/* the guest's threads cannot be stopped one by one, and the memory
	lent to the guest stays mapped: the process ends, back to HOME */
	svcExitProcess();
	__builtin_unreachable();
}

void host_abort(const char *reason)
{
	host_fatal("The game stopped: %s\n\nSee debug.txt and host.log in /switch/halo.", reason);
}

void host_exit(int code)
{
	host_logf(HOST_LOG_INFO, "the game exited (%d)", code);
	log_flush();
	host_audio_pause(1);
	host_input_stop_rumble();
	svcExitProcess();
	__builtin_unreachable();
}

/* ---------- settings */

/* config.toml as last read: at the first setting asked for, and again each
time the game's settings menus write it (host_config_changed) */
static Mutex config_lock;
static toml_result_t config;
static int config_loaded;

static void config_load(void)
{
	if (config_loaded)
		toml_free(config);
	config = toml_parse_file_ex(HOST_DATA_ROOT "/config.toml");
	config_loaded = 1;
}

int host_config_boolean_default(const char *key, int missing)
{
	toml_datum_t value;
	int result = missing;

	mutexLock(&config_lock);
	if (!config_loaded)
		config_load();
	if (config.ok)
	{
		value = toml_seek(config.toptab, key);
		if (value.type == TOML_BOOLEAN)
			result = value.u.boolean;
	}
	mutexUnlock(&config_lock);
	return result;
}

/* the guest wrote a setting (port_config.c's config_write): the input's are
taken up now; the swap interval's when the guest sets it again
(platform_display_apply) */
void host_config_changed(void)
{
	mutexLock(&config_lock);
	config_load();
	mutexUnlock(&config_lock);
	host_input_settings_read();
}

int host_config_boolean(const char *key)
{
	return host_config_boolean_default(key, 0);
}

double host_config_real(const char *key, double missing)
{
	toml_result_t config = toml_parse_file_ex(HOST_DATA_ROOT "/config.toml");
	toml_datum_t value;
	double result = missing;

	if (!config.ok)
		return missing;
	value = toml_seek(config.toptab, key);
	if (value.type == TOML_FP64)
		result = value.u.fp64;
	else if (value.type == TOML_INT64)
		result = (double)value.u.int64;
	toml_free(config);
	return result;
}

/* ---------- the CPU's clock */

/* while a map loads (sdl_platform.c's halo_map_loading), nothing is drawn:
the CPU at its boosted clock, which lowers the GPU's */
void host_cpu_boost(int boost)
{
	appletSetCpuBoostMode(boost ? ApmCpuBoostMode_FastLoad : ApmCpuBoostMode_Normal);
}

/* ---------- paths */

void host_android_path(int which, char *buffer, uint32_t size)
{
	snprintf(buffer, size, "%s", which ? HOST_SAVE_ROOT : HOST_DATA_ROOT);
}

static int file_exists(const char *path)
{
	struct stat information;

	return stat(path, &information) == 0;
}

/* ---------- the guest's environment */

#define ENVIRONMENT_MAXIMUM 32

struct environment
{
	char entries[ENVIRONMENT_MAXIMUM][256];
	int count;
};

static void environment_set(struct environment *environment, const char *name, const char *value)
{
	if (environment->count < ENVIRONMENT_MAXIMUM)
		snprintf(environment->entries[environment->count++], sizeof(environment->entries[0]), "%s=%s", name, value);
}

/* POSIX TZ for the console's time zone (the guest's musl has no zone
database) */
static void time_zone(char *buffer, size_t size)
{
	TimeCalendarTime calendar;
	TimeCalendarAdditionalInfo information;
	u64 now = 0;
	long offset = 0;

	if (R_SUCCEEDED(timeGetCurrentTime(TimeType_Default, &now)) &&
		R_SUCCEEDED(timeToCalendarTimeWithMyRule(now, &calendar, &information)))
		offset = -(long)information.offset;
	snprintf(buffer, size, "<L>%s%ld:%02ld", offset < 0 ? "-" : "", labs(offset) / 3600, (labs(offset) / 60) % 60);
}

/* argv and the environment, in guest memory */
static uint32_t make_boot(const struct environment *environment)
{
	size_t size = 0x10000;
	char *memory = host_low_map(size, 1);
	struct halo_guest_boot *boot = (struct halo_guest_boot *)memory;
	uint32_t *argv, *environ_list;
	char *strings;
	int index;

	if (!memory)
		host_fatal("There is no memory for the game's environment.");
	argv = (uint32_t *)(memory + sizeof(*boot));
	environ_list = argv + 2;
	strings = (char *)(environ_list + ENVIRONMENT_MAXIMUM + 1);
	strcpy(strings, "halo");
	argv[0] = (uint32_t)(uintptr_t)strings;
	argv[1] = 0;
	strings += strlen(strings) + 1;
	for (index = 0; index < environment->count; index++)
	{
		size_t length = strlen(environment->entries[index]) + 1;

		memcpy(strings, environment->entries[index], length);
		environ_list[index] = (uint32_t)(uintptr_t)strings;
		strings += length;
	}
	environ_list[index] = 0;
	boot->argc = 1;
	boot->argv = (uint32_t)(uintptr_t)argv;
	boot->environment = (uint32_t)(uintptr_t)environ_list;
	boot->page_size = 0x1000;
	return (uint32_t)(uintptr_t)boot;
}

/* ---------- the image */

static void *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	void *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0 &&
		(data = malloc((size_t)length)) != NULL)
	{
		if (fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		*size = (size_t)length;
	}
	fclose(file);
	return data;
}

/* ---------- main */

#define MAIN_STACK_SIZE (16 * 1024 * 1024)

static void *game_main(void *boot)
{
	host_run_guest_main((uint32_t)(uintptr_t)boot);
}

/* the sockets the netcode asks for: buffers of 1 MB at most
(transport_endpoint_winsock.c, p2p.c), and the Homebrew Menu's default
sessions */
static const SocketInitConfig socket_config = {
	.tcp_tx_buf_size = 0x8000,
	.tcp_rx_buf_size = 0x10000,
	.tcp_tx_buf_max_size = 0x100000,
	.tcp_rx_buf_max_size = 0x100000,
	.udp_tx_buf_size = 0x2400,
	.udp_rx_buf_size = 0xa500,
	.sb_efficiency = 8,
	.num_bsd_sessions = 3,
	.bsd_service_type = BsdServiceType_User,
};

int main(int argc, char *argv[])
{
	struct environment *environment = calloc(1, sizeof(*environment));
	const char *reason;
	char zone[64];
	size_t image_size = 0;
	void *image;
	uint32_t boot;

	(void)argc;
	(void)argv;
	mutexInit(&log_lock);
	/* the game's main thread has core 0 to itself (host_thread.c) */
	svcSetThreadCoreMask(CUR_THREAD_HANDLE, 0, 1u << 0);
	mkdir("/switch", 0777);
	mkdir(HOST_DATA_ROOT, 0777);
	mkdir(HOST_SAVE_ROOT, 0777);
	log_file = fopen(HOST_DATA_ROOT "/host.log", "w");
	if (R_SUCCEEDED(socketInitialize(&socket_config)) && __nxlink_host.s_addr)
		log_socket = nxlinkConnectToHost(false, false);
	send_last_crash_report();
	host_logf(HOST_LOG_INFO, "Halo for Switch starting");
	host_syscall_initialize();
	host_watch_start();

	/* loading at the CPU's boosted clock (the GPU's lowered meanwhile) */
	appletSetCpuBoostMode(ApmCpuBoostMode_FastLoad);
	reason = host_memory_initialize();
	if (reason)
		host_fatal("%s", reason);
	if (R_FAILED(romfsInit()))
		host_fatal("Cannot open the program's RomFS.");
	image = read_file("romfs:/halo_guest.elf", &image_size);
	if (!image)
		host_fatal("Cannot read the game from the program's RomFS.");
	if (host_load_image(image, image_size) != 0)
		host_fatal("Cannot load the game; see host.log in /switch/halo.");
	free(image);
	romfsExit();
	appletSetCpuBoostMode(ApmCpuBoostMode_Normal);
	if (!file_exists(HOST_DATA_ROOT "/maps/ui.map"))
		host_fatal("The Halo game data was not found.\n\nCopy the maps folder of an Xbox disc image of Halo: "
			"Combat Evolved (extract-xiso -x) to /switch/halo/maps on the SD card.");

	environment_set(environment, "HOME", HOST_SAVE_ROOT);
	environment_set(environment, "HALO_DATA_ROOT", HOST_DATA_ROOT);
	environment_set(environment, "HALO_SAVE_ROOT", HOST_SAVE_ROOT);
	/* 480 lines in the screen's 16:9 (d3d8_gl.c), unless
	display.screen_width says otherwise */
	environment_set(environment, "HALO_DISPLAY_WIDTH", "852");
	time_zone(zone, sizeof(zone));
	environment_set(environment, "TZ", zone);
	boot = make_boot(environment);
	free(environment);

	host_input_initialize();
	/* no dimming or sleeping while the game plays (it has no idle input
	during a cutscene) */
	appletSetMediaPlaybackState(true);
	host_profile_start();
	host_logf(HOST_LOG_INFO, "starting the game (data %s, saves %s)", HOST_DATA_ROOT, HOST_SAVE_ROOT);
	host_thread_run_on_guest_stack(game_main, (void *)(uintptr_t)boot, MAIN_STACK_SIZE);
	host_fatal("the game returned");
}
