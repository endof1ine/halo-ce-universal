/*
HOST_SYSCALL.C

The guest's system calls (its musl's, port/android/guest/libc), emulated
with newlib and libnx: the guest is built for Linux, so its calls, flags,
structures and errno values are Linux's (AArch64's, with 32-bit pointers),
and each is translated here.

The guest's file descriptors are this file's: an entry is a newlib file, or
a directory (which Linux reads with getdents64, newlib with readdir).
Standard output and error go to the log.
*/

#include "host.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <switch.h>
#include <time.h>
#include <unistd.h>

#include "guest_syscall.h"

#define GUEST(type, value) ((type)(uintptr_t)(uint32_t)(value))

/* the guest's (Linux's) constants */
#define LINUX_O_ACCMODE 03
#define LINUX_O_CREAT 0100
#define LINUX_O_EXCL 0200
#define LINUX_O_TRUNC 01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DIRECTORY 040000
#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000
#define LINUX_F_DUPFD 0
#define LINUX_F_GETFD 1
#define LINUX_F_SETFD 2
#define LINUX_F_GETFL 3
#define LINUX_F_SETFL 4
#define LINUX_F_DUPFD_CLOEXEC 1030
#define LINUX_SEEK_SET 0
#define LINUX_CLOCK_REALTIME 0
#define LINUX_CLOCK_REALTIME_COARSE 5
#define LINUX_TIMER_ABSTIME 1
#define LINUX_FUTEX_WAIT 0
#define LINUX_FUTEX_WAKE 1
#define LINUX_FUTEX_REQUEUE 3
#define LINUX_FUTEX_CMP_REQUEUE 4
#define LINUX_FUTEX_WAIT_BITSET 9
#define LINUX_FUTEX_WAKE_BITSET 10
#define LINUX_FUTEX_CMD_MASK 0x7f
#define LINUX_SIGABRT 6
#define LINUX_DT_DIR 4
#define LINUX_DT_REG 8
/* the guest's _llseek (port/android/guest/libc/arch/arm64_32/bits/syscall.h.in) */
#define GUEST_SYS_llseek 65536

/* the guest's structures */
struct guest_timespec
{
	int32_t seconds;
	int32_t nanoseconds;
};

struct guest_iovec
{
	uint32_t base;
	uint32_t length;
};

/* port/android/guest/libc/arch/arm64_32/kstat.h */
struct guest_kstat
{
	uint64_t st_dev;
	uint64_t st_ino;
	uint32_t st_mode;
	uint32_t st_nlink;
	uint32_t st_uid;
	uint32_t st_gid;
	uint64_t st_rdev;
	uint64_t pad;
	int64_t st_size;
	int32_t st_blksize;
	int32_t pad2;
	int64_t st_blocks;
	int64_t st_atime_sec;
	int64_t st_atime_nsec;
	int64_t st_mtime_sec;
	int64_t st_mtime_nsec;
	int64_t st_ctime_sec;
	int64_t st_ctime_nsec;
	uint32_t unused[2];
};

struct guest_dirent64
{
	uint64_t d_ino;
	int64_t d_off;
	uint16_t d_reclen;
	uint8_t d_type;
	char d_name[];
};

/* musl's struct sysinfo with 32-bit longs */
struct guest_sysinfo
{
	uint32_t uptime;
	uint32_t loads[3];
	uint32_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
	uint16_t procs, pad;
	uint32_t totalhigh, freehigh;
	uint32_t mem_unit;
	char reserved[256];
};

/* ---------- errno */

int host_linux_errno(int error)
{
	switch (error)
	{
	case 0: return 0;
	case EPERM: return 1;
	case ENOENT: return 2;
	case ESRCH: return 3;
	case EINTR: return 4;
	case EIO: return 5;
	case ENXIO: return 6;
	case E2BIG: return 7;
	case ENOEXEC: return 8;
	case EBADF: return 9;
	case ECHILD: return 10;
	case EAGAIN: return 11;
	case ENOMEM: return 12;
	case EACCES: return 13;
	case EFAULT: return 14;
	case EBUSY: return 16;
	case EEXIST: return 17;
	case EXDEV: return 18;
	case ENODEV: return 19;
	case ENOTDIR: return 20;
	case EISDIR: return 21;
	case EINVAL: return 22;
	case ENFILE: return 23;
	case EMFILE: return 24;
	case ENOTTY: return 25;
	case EFBIG: return 27;
	case ENOSPC: return 28;
	case ESPIPE: return 29;
	case EROFS: return 30;
	case EMLINK: return 31;
	case EPIPE: return 32;
	case EDOM: return 33;
	case ERANGE: return 34;
	case EDEADLK: return 35;
	case ENAMETOOLONG: return 36;
	case ENOLCK: return 37;
	case ENOSYS: return 38;
	case ENOTEMPTY: return 39;
	case ELOOP: return 40;
	case EOVERFLOW: return 75;
	case ENOTSOCK: return 88;
	case EMSGSIZE: return 90;
	case EPROTONOSUPPORT: return 93;
	case EOPNOTSUPP: return 95;
	case ENOTSUP: return 95;
	case EAFNOSUPPORT: return 97;
	case EADDRINUSE: return 98;
	case EADDRNOTAVAIL: return 99;
	case ENETDOWN: return 100;
	case ENETUNREACH: return 101;
	case ECONNABORTED: return 103;
	case ECONNRESET: return 104;
	case ENOBUFS: return 105;
	case EISCONN: return 106;
	case ENOTCONN: return 107;
	case ETIMEDOUT: return 110;
	case ECONNREFUSED: return 111;
	case EHOSTDOWN: return 112;
	case EHOSTUNREACH: return 113;
	case EALREADY: return 114;
	case EINPROGRESS: return 115;
	case ECANCELED: return 125;
	default: return 5; /* EIO */
	}
}

int host_errno(void)
{
	return host_linux_errno(errno);
}

static long failure(void)
{
	return -host_linux_errno(errno);
}

static long result_of(long value)
{
	return value < 0 ? failure() : value;
}

/* ---------- the guest's file descriptors

A file's descriptor is newlib's own, as the host's posix_* functions
(port/linux/src/posix_files.c) take the guest's descriptors to newlib. A
directory's is one of this file's, from DIRECTORY_BASE: Linux reads
directories with getdents64, newlib with readdir. */

#define FILE_LOCKS 1024
#define DIRECTORY_BASE 0x4000
#define DIRECTORY_COUNT 64

/* reads and writes at a position seek, read and seek back (newlib has no
positional ones of its own), so a file's operations take its lock */
static Mutex file_locks[FILE_LOCKS];
static int file_flags[FILE_LOCKS];

struct guest_directory
{
	int used;
	DIR *stream;
	char *path;
	int flags;
	long long position; /* entries read */
	struct dirent pending; /* an entry that did not fit */
	int has_pending;
	Mutex lock;
};

static struct guest_directory directories[DIRECTORY_COUNT];
static Mutex directory_table_lock;

static int is_directory(long long descriptor)
{
	return descriptor >= DIRECTORY_BASE && descriptor < DIRECTORY_BASE + DIRECTORY_COUNT;
}

/* the directory, locked; NULL (unlocked) when it is not open */
static struct guest_directory *directory_get(long long descriptor)
{
	struct guest_directory *directory;

	if (!is_directory(descriptor))
		return NULL;
	directory = &directories[descriptor - DIRECTORY_BASE];
	mutexLock(&directory->lock);
	if (!directory->used)
	{
		mutexUnlock(&directory->lock);
		return NULL;
	}
	return directory;
}

static void directory_put(struct guest_directory *directory)
{
	mutexUnlock(&directory->lock);
}

static void file_lock(long long descriptor)
{
	if (descriptor >= 0 && descriptor < FILE_LOCKS)
		mutexLock(&file_locks[descriptor]);
}

static void file_unlock(long long descriptor)
{
	if (descriptor >= 0 && descriptor < FILE_LOCKS)
		mutexUnlock(&file_locks[descriptor]);
}

/* a path relative to a directory descriptor as an absolute one */
static const char *path_at(long long descriptor, const char *path, char *buffer, size_t size)
{
	struct guest_directory *directory;

	if (!path || path[0] == '/' || (int)descriptor == LINUX_AT_FDCWD || strchr(path, ':'))
		return path;
	directory = directory_get(descriptor);
	if (!directory)
		return NULL;
	snprintf(buffer, size, "%s/%s", directory->path, path);
	directory_put(directory);
	return buffer;
}

static int host_open_flags(int flags)
{
	int result = flags & LINUX_O_ACCMODE;

	if (flags & LINUX_O_CREAT)
		result |= O_CREAT;
	if (flags & LINUX_O_EXCL)
		result |= O_EXCL;
	if (flags & LINUX_O_TRUNC)
		result |= O_TRUNC;
	if (flags & LINUX_O_APPEND)
		result |= O_APPEND;
	if (flags & LINUX_O_NONBLOCK)
		result |= O_NONBLOCK;
	return result;
}

static long open_directory(const char *path, int flags)
{
	DIR *stream = opendir(path);
	int index;

	if (!stream)
		return failure();
	mutexLock(&directory_table_lock);
	for (index = 0; index < DIRECTORY_COUNT; index++)
	{
		if (!directories[index].used)
		{
			struct guest_directory *directory = &directories[index];

			mutexLock(&directory->lock);
			directory->used = 1;
			directory->stream = stream;
			directory->path = strdup(path);
			directory->flags = flags;
			directory->position = 0;
			directory->has_pending = 0;
			mutexUnlock(&directory->lock);
			mutexUnlock(&directory_table_lock);
			return DIRECTORY_BASE + index;
		}
	}
	mutexUnlock(&directory_table_lock);
	closedir(stream);
	return -24; /* EMFILE */
}

static long guest_openat(long long descriptor, const char *guest_path, int flags, int mode)
{
	char buffer[1024];
	const char *path = path_at(descriptor, guest_path, buffer, sizeof(buffer));
	struct stat information;
	int host;

	if (!path)
		return -9; /* EBADF */
	if ((flags & LINUX_O_DIRECTORY) ||
		(!(flags & LINUX_O_CREAT) && stat(path, &information) == 0 && S_ISDIR(information.st_mode)))
		return open_directory(path, flags);
	host = open(path, host_open_flags(flags), mode);
	if (host < 0)
		return failure();
	if (host >= DIRECTORY_BASE)
	{
		close(host);
		return -24;
	}
	if (host < FILE_LOCKS)
		file_flags[host] = flags;
	return host;
}

static long guest_close(long long descriptor)
{
	struct guest_directory *directory;

	if (descriptor >= 0 && descriptor < 3)
		return 0;
	if ((directory = directory_get(descriptor)) != NULL)
	{
		closedir(directory->stream);
		free(directory->path);
		directory->path = NULL;
		directory->used = 0;
		directory_put(directory);
		return 0;
	}
	if (is_directory(descriptor))
		return -9;
	return close((int)descriptor) == 0 ? 0 : failure();
}

static long guest_dup(long long descriptor)
{
	int copy;

	if (is_directory(descriptor))
		return -22;
	copy = dup((int)descriptor);
	if (copy < 0)
		return failure();
	if (copy < FILE_LOCKS && descriptor >= 0 && descriptor < FILE_LOCKS)
		file_flags[copy] = file_flags[descriptor];
	return copy;
}

/* ---------- standard output and error */

struct log_stream
{
	char line[1024];
	size_t length;
};

static struct log_stream log_streams[2];
static Mutex log_lock;

static void log_bytes(int fd, const char *bytes, size_t size)
{
	struct log_stream *stream = &log_streams[fd == 2];
	size_t index;

	mutexLock(&log_lock);
	for (index = 0; index < size; index++)
	{
		char c = bytes[index];

		if (c == '\n' || stream->length == sizeof(stream->line) - 1)
		{
			stream->line[stream->length] = 0;
			host_logf(fd == 2 ? HOST_LOG_WARN : HOST_LOG_INFO, "%s", stream->line);
			stream->length = 0;
			if (c == '\n')
				continue;
		}
		stream->line[stream->length++] = c;
	}
	mutexUnlock(&log_lock);
}

/* ---------- reading and writing */

static long guest_read(long long descriptor, void *buffer, size_t size)
{
	long result;

	if (is_directory(descriptor))
		return -21; /* EISDIR */
	if (descriptor == 0)
		return 0;
	file_lock(descriptor);
	result = result_of(read((int)descriptor, buffer, size));
	file_unlock(descriptor);
	return result;
}

static long guest_write(long long descriptor, const void *buffer, size_t size)
{
	long result;

	if (descriptor == 1 || descriptor == 2)
	{
		log_bytes((int)descriptor, buffer, size);
		return (long)size;
	}
	if (is_directory(descriptor))
		return -21;
	file_lock(descriptor);
	result = result_of(write((int)descriptor, buffer, size));
	file_unlock(descriptor);
	return result;
}

/* positional, as Linux's: the position is kept */
static long guest_pread(long long descriptor, void *buffer, size_t size, int64_t offset, int writing)
{
	off_t position;
	long result;

	if (is_directory(descriptor))
		return -21;
	file_lock(descriptor);
	position = lseek((int)descriptor, 0, SEEK_CUR);
	if (position < 0 || lseek((int)descriptor, (off_t)offset, SEEK_SET) < 0)
	{
		result = failure();
	}
	else
	{
		result = writing ? result_of(write((int)descriptor, buffer, size)) :
			result_of(read((int)descriptor, buffer, size));
		lseek((int)descriptor, position, SEEK_SET);
	}
	file_unlock(descriptor);
	return result;
}

static long guest_vector(long long descriptor, uint64_t vector, int count, int64_t offset, int positional,
	int writing)
{
	const struct guest_iovec *guest_vector = GUEST(const struct guest_iovec *, vector);
	long total = 0;
	int index;

	if (count < 0 || count > 1024)
		return -22;
	for (index = 0; index < count; index++)
	{
		void *base = GUEST(void *, guest_vector[index].base);
		uint32_t length = guest_vector[index].length;
		long done;

		if (!length)
			continue;
		if (positional)
			done = guest_pread(descriptor, base, length, offset + total, writing);
		else if (writing)
			done = guest_write(descriptor, base, length);
		else
			done = guest_read(descriptor, base, length);
		if (done < 0)
			return total ? total : done;
		total += done;
		if ((uint32_t)done < length)
			break;
	}
	return total;
}

static long guest_lseek(long long descriptor, int64_t offset, int whence, int64_t *result)
{
	struct guest_directory *directory = directory_get(descriptor);
	off_t position;

	if (directory)
	{
		/* (only rewinding: musl's rewinddir) */
		if (whence == LINUX_SEEK_SET && offset == 0)
		{
			rewinddir(directory->stream);
			directory->position = 0;
			directory->has_pending = 0;
			*result = 0;
			directory_put(directory);
			return 0;
		}
		directory_put(directory);
		return -22;
	}
	if (is_directory(descriptor))
		return -9;
	file_lock(descriptor);
	position = lseek((int)descriptor, (off_t)offset, whence);
	file_unlock(descriptor);
	if (position < 0)
		return failure();
	*result = position;
	return 0;
}

/* ---------- directories */

static long guest_getdents64(long long descriptor, uint64_t buffer, uint32_t size)
{
	struct guest_directory *directory = directory_get(descriptor);
	char *out = GUEST(char *, buffer);
	uint32_t used = 0;

	if (!directory)
		return is_directory(descriptor) || descriptor < 0 ? -9 : -20; /* EBADF, ENOTDIR */
	for (;;)
	{
		struct dirent *item;
		struct guest_dirent64 *record;
		size_t name_length;
		uint32_t length;

		if (directory->has_pending)
		{
			item = &directory->pending;
		}
		else
		{
			item = readdir(directory->stream);
			if (!item)
				break;
		}
		name_length = strlen(item->d_name);
		length = (uint32_t)((offsetof(struct guest_dirent64, d_name) + name_length + 1 + 7) & ~(size_t)7);
		if (used + length > size)
		{
			if (item != &directory->pending)
			{
				directory->pending = *item;
				directory->has_pending = 1;
			}
			if (!used)
			{
				directory_put(directory);
				return -22; /* EINVAL: the buffer is too small */
			}
			break;
		}
		record = (struct guest_dirent64 *)(out + used);
		record->d_ino = (uint64_t)directory->position + 1;
		record->d_off = directory->position + 1;
		record->d_reclen = (uint16_t)length;
		record->d_type = item->d_type == DT_DIR ? LINUX_DT_DIR : LINUX_DT_REG;
		memcpy(record->d_name, item->d_name, name_length + 1);
		used += length;
		directory->position++;
		directory->has_pending = 0;
	}
	directory_put(directory);
	return used;
}

/* ---------- file information */

static void kstat_fill(struct guest_kstat *result, const struct stat *information)
{
	memset(result, 0, sizeof(*result));
	result->st_dev = 1;
	result->st_ino = (uint64_t)information->st_ino;
	result->st_mode = (uint32_t)information->st_mode;
	result->st_nlink = 1;
	result->st_size = (int64_t)information->st_size;
	result->st_blksize = 4096;
	result->st_blocks = ((int64_t)information->st_size + 511) / 512;
	result->st_atime_sec = (int64_t)information->st_mtime;
	result->st_mtime_sec = (int64_t)information->st_mtime;
	result->st_ctime_sec = (int64_t)information->st_mtime;
}

static long guest_fstat(long long descriptor, uint64_t result)
{
	struct guest_directory *directory;
	struct stat information;
	int status;

	if (descriptor >= 0 && descriptor < 3)
	{
		memset(&information, 0, sizeof(information));
		information.st_mode = S_IFCHR | 0666;
		kstat_fill(GUEST(struct guest_kstat *, result), &information);
		return 0;
	}
	if ((directory = directory_get(descriptor)) != NULL)
	{
		status = stat(directory->path, &information);
		directory_put(directory);
	}
	else if (is_directory(descriptor))
	{
		return -9;
	}
	else
	{
		status = fstat((int)descriptor, &information);
	}
	if (status != 0)
		return failure();
	kstat_fill(GUEST(struct guest_kstat *, result), &information);
	return 0;
}

static long guest_fstatat(long long directory, const char *guest_path, uint64_t result, int flags)
{
	char buffer[1024];
	const char *path;
	struct stat information;

	if ((flags & LINUX_AT_EMPTY_PATH) && (!guest_path || !*guest_path))
		return guest_fstat(directory, result);
	path = path_at(directory, guest_path, buffer, sizeof(buffer));
	if (!path)
		return -9;
	if (stat(path, &information) != 0)
		return failure();
	kstat_fill(GUEST(struct guest_kstat *, result), &information);
	return 0;
}

/* ---------- time */

/* the time of day at start-up, against the system tick */
static int64_t realtime_base_ns;
static uint64_t realtime_base_tick;

static void time_initialize(void)
{
	struct timespec now;

	clock_gettime(CLOCK_REALTIME, &now);
	realtime_base_ns = (int64_t)now.tv_sec * 1000000000ll + now.tv_nsec;
	realtime_base_tick = armGetSystemTick();
}

static int64_t clock_ns(int clock)
{
	uint64_t tick = armGetSystemTick();

	if (clock == LINUX_CLOCK_REALTIME || clock == LINUX_CLOCK_REALTIME_COARSE)
		return realtime_base_ns + (int64_t)armTicksToNs(tick - realtime_base_tick);
	return (int64_t)armTicksToNs(tick);
}

static void timespec_out(uint64_t address, int64_t ns)
{
	struct guest_timespec *result = GUEST(struct guest_timespec *, address);

	if (!address)
		return;
	result->seconds = (int32_t)(ns / 1000000000ll);
	result->nanoseconds = (int32_t)(ns % 1000000000ll);
}

/* -1 when there is none */
static int64_t timespec_ns(uint64_t address)
{
	const struct guest_timespec *value = GUEST(const struct guest_timespec *, address);

	if (!address)
		return -1;
	return (int64_t)value->seconds * 1000000000ll + value->nanoseconds;
}

static void sleep_ns(int64_t ns)
{
	if (ns > 0)
		svcSleepThread(ns);
}

/* ---------- futexes: svcWaitForAddress and svcSignalToAddress */

static long guest_futex(uint64_t address, int operation, uint32_t value, uint64_t timeout, uint64_t address2,
	uint32_t value3)
{
	int command = operation & LINUX_FUTEX_CMD_MASK;
	void *word = GUEST(void *, address);
	Result result;

	(void)address2;
	(void)value3;
	switch (command)
	{
	case LINUX_FUTEX_WAIT:
	case LINUX_FUTEX_WAIT_BITSET:
	{
		/* (musl waits only with relative timeouts) */
		int64_t ns = timespec_ns(timeout);

		result = svcWaitForAddress(word, ArbitrationType_WaitIfEqual, (s32)value, ns < 0 ? -1 : ns);
		if (R_SUCCEEDED(result))
			return 0;
		if (R_VALUE(result) == KERNELRESULT(TimedOut))
			return -110; /* ETIMEDOUT */
		if (R_VALUE(result) == KERNELRESULT(InvalidState))
			return -11; /* EAGAIN: the value had changed */
		return -4; /* EINTR */
	}
	case LINUX_FUTEX_WAKE:
	case LINUX_FUTEX_WAKE_BITSET:
		svcSignalToAddress(word, SignalType_Signal, 0, (s32)(value > 0x7fffffff ? -1 : (int32_t)value));
		return 0;
	case LINUX_FUTEX_REQUEUE:
	case LINUX_FUTEX_CMP_REQUEUE:
	{
		/* no requeue: wake those to be moved too. musl's condition
		variables, its only user, take such a wake for a requeue (the
		woken thread takes the mutex itself) */
		uint64_t count = (uint64_t)value + (uint32_t)timeout;

		svcSignalToAddress(word, SignalType_Signal, 0, count > 0x7fffffff ? -1 : (s32)count);
		return 0;
	}
	default:
		return -38; /* ENOSYS */
	}
}

/* ---------- process and miscellany */

static uint32_t thread_id(void)
{
	u64 id = 0;

	svcGetThreadId(&id, CUR_THREAD_HANDLE);
	return (uint32_t)(id & 0x3fffffff) | 1;
}

static long guest_uname(uint64_t buffer)
{
	static const char *const fields[6] = {"Horizon", "switch", "1", "1", "aarch64", ""};
	char *out = GUEST(char *, buffer);
	int index;

	memset(out, 0, 6 * 65);
	for (index = 0; index < 6; index++)
		snprintf(out + index * 65, 65, "%s", fields[index]);
	return 0;
}

static long guest_sysinfo(uint64_t buffer)
{
	struct guest_sysinfo *result = GUEST(struct guest_sysinfo *, buffer);
	u64 total = 0;

	svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
	memset(result, 0, sizeof(*result));
	result->uptime = (uint32_t)(armTicksToNs(armGetSystemTick()) / 1000000000ull);
	/* (newlib holds the whole heap from the start: what the guest can
	still have is the arena's room, which this does not track; a quarter
	of the memory is a fair figure for the game's checks) */
	result->totalram = (uint32_t)(total > 0xffffffffull ? 0xffffffffull : total);
	result->freeram = result->totalram / 4;
	result->procs = 1;
	result->mem_unit = 1;
	return 0;
}

/* ---------- dispatch */

long long host_syscall(long long number, long long a, long long b, long long c, long long d, long long e,
	long long f)
{
	char path_buffer[1024];

	switch (number)
	{
	case SYS_read:
		return guest_read(a, GUEST(void *, b), (uint32_t)c);
	case SYS_write:
		return guest_write(a, GUEST(const void *, b), (uint32_t)c);
	case SYS_readv:
		return guest_vector(a, (uint64_t)b, (int)c, 0, 0, 0);
	case SYS_writev:
		return guest_vector(a, (uint64_t)b, (int)c, 0, 0, 1);
	case SYS_preadv:
		return guest_vector(a, (uint64_t)b, (int)c, d, 1, 0);
	case SYS_pwritev:
		return guest_vector(a, (uint64_t)b, (int)c, d, 1, 1);
	case SYS_pread64:
		return guest_pread(a, GUEST(void *, b), (uint32_t)c, d, 0);
	case SYS_pwrite64:
		return guest_pread(a, GUEST(void *, b), (uint32_t)c, d, 1);
	case SYS_openat:
		return guest_openat(a, GUEST(const char *, b), (int)c, (int)d);
	case SYS_close:
		return guest_close(a);
	case SYS_lseek:
	{
		int64_t position;
		long result = guest_lseek(a, b, (int)c, &position);

		return result ? result : (position > 0x7fffffffll ? -75 /* EOVERFLOW */ : (long)position);
	}
	case GUEST_SYS_llseek:
	{
		int64_t position;
		long result = guest_lseek(a, (int64_t)(((uint64_t)(uint32_t)b << 32) | (uint32_t)c), (int)e, &position);

		if (!result)
			*GUEST(int64_t *, d) = position;
		return result;
	}
	case SYS_getdents64:
		return guest_getdents64(a, (uint64_t)b, (uint32_t)c);
	case SYS_fstat:
		return guest_fstat(a, (uint64_t)b);
	case SYS_newfstatat:
		return guest_fstatat(a, GUEST(const char *, b), (uint64_t)c, (int)d);
	case SYS_statx:
		return -38; /* musl falls back to fstatat */
	case SYS_unlinkat:
	{
		const char *path = path_at(a, GUEST(const char *, b), path_buffer, sizeof(path_buffer));

		if (!path)
			return -9;
		return result_of((c & LINUX_AT_REMOVEDIR) ? rmdir(path) : unlink(path));
	}
	case SYS_renameat:
	case SYS_renameat2:
	{
		char other_buffer[1024];
		const char *from = path_at(a, GUEST(const char *, b), path_buffer, sizeof(path_buffer));
		const char *to = path_at(c, GUEST(const char *, d), other_buffer, sizeof(other_buffer));

		if (!from || !to)
			return -9;
		/* (Linux replaces an existing file; FAT does not) */
		if (rename(from, to) != 0 && (errno != EEXIST || unlink(to) != 0 || rename(from, to) != 0))
			return failure();
		return 0;
	}
	case SYS_mkdirat:
	{
		const char *path = path_at(a, GUEST(const char *, b), path_buffer, sizeof(path_buffer));

		return path ? result_of(mkdir(path, (mode_t)c)) : -9;
	}
	case SYS_faccessat:
	{
		const char *path = path_at(a, GUEST(const char *, b), path_buffer, sizeof(path_buffer));
		struct stat information;

		if (!path)
			return -9;
		return stat(path, &information) == 0 ? 0 : failure();
	}
	case SYS_ftruncate:
	{
		long result;

		if (is_directory(a))
			return -21;
		file_lock(a);
		result = result_of(ftruncate((int)a, (off_t)b));
		file_unlock(a);
		return result;
	}
	case SYS_fsync:
	case SYS_fdatasync:
		if (a >= 3 && !is_directory(a))
			fsync((int)a);
		return 0;
	case SYS_fcntl:
		switch ((int)b)
		{
		case LINUX_F_GETFL:
		{
			struct guest_directory *directory = directory_get(a);
			long flags;

			if (directory)
			{
				flags = directory->flags;
				directory_put(directory);
				return flags;
			}
			if (a >= 0 && a < 3)
				return a ? 1 : 0;
			return a >= 0 && a < FILE_LOCKS ? file_flags[a] : 2;
		}
		case LINUX_F_DUPFD:
		case LINUX_F_DUPFD_CLOEXEC:
			return guest_dup(a);
		case LINUX_F_GETFD:
		case LINUX_F_SETFD:
		case LINUX_F_SETFL:
			return 0;
		default:
			return -22;
		}
	case SYS_dup:
	case SYS_dup3:
		return guest_dup(a);
	case SYS_getcwd:
	{
		char *buffer = GUEST(char *, a);

		if (!getcwd(buffer, (size_t)(uint32_t)b))
			return failure();
		return (long)strlen(buffer) + 1;
	}
	case SYS_chdir:
		return result_of(chdir(GUEST(const char *, a)));
	case SYS_readlinkat:
		return -22;
	case SYS_ioctl:
		/* (musl asks whether standard output is a terminal) */
		return -25;
	case SYS_pipe2:
		return -38;
	case SYS_utimensat:
	case SYS_fchmod:
	case SYS_fchmodat:
	case SYS_flock:
		return 0;
	case SYS_umask:
		return 022;

	case SYS_clock_gettime:
		timespec_out((uint64_t)b, clock_ns((int)a));
		return 0;
	case SYS_clock_getres:
		timespec_out((uint64_t)b, 1);
		return 0;
	case SYS_gettimeofday:
	{
		struct guest_timespec *result = GUEST(struct guest_timespec *, a);
		int64_t ns = clock_ns(LINUX_CLOCK_REALTIME);

		if (result)
		{
			result->seconds = (int32_t)(ns / 1000000000ll);
			result->nanoseconds = (int32_t)((ns % 1000000000ll) / 1000);
		}
		return 0;
	}
	case SYS_nanosleep:
	{
		int64_t ns = timespec_ns((uint64_t)a);

		if (ns < 0)
			return -14; /* EFAULT */
		sleep_ns(ns);
		return 0;
	}
	case SYS_clock_nanosleep:
	{
		int64_t ns = timespec_ns((uint64_t)c);

		if (ns < 0)
			return -14;
		sleep_ns((b & LINUX_TIMER_ABSTIME) ? ns - clock_ns((int)a) : ns);
		return 0;
	}
	case SYS_futex:
		return guest_futex((uint64_t)a, (int)b, (uint32_t)c, (uint64_t)d, (uint64_t)e, (uint32_t)f);
	case SYS_ppoll:
	{
		/* (files are always ready; the guest polls sockets through the
		host's posix_* functions) */
		int64_t ns = timespec_ns((uint64_t)c);

		if ((uint32_t)b == 0)
		{
			sleep_ns(ns < 0 ? 1000000ll : ns);
			return 0;
		}
		return (long)(uint32_t)b;
	}

	case SYS_mmap:
		return host_guest_mmap((uint64_t)a, (uint64_t)b, (int)c, (int)d, (int)e, f);
	case SYS_munmap:
		return host_guest_munmap((uint64_t)a, (uint64_t)b);
	case SYS_mprotect:
		return host_guest_mprotect((uint64_t)a, (uint64_t)b, (int)c);
	case SYS_madvise:
		return 0;
	case SYS_mremap:
	case SYS_brk:
		/* musl then falls back to mmap and copying */
		return -12;

	case SYS_exit:
	case SYS_exit_group:
		host_exit((int)a);
	case SYS_set_tid_address:
	case SYS_gettid:
		return thread_id();
	case SYS_getpid:
		return 2;
	case SYS_getppid:
		return 1;
	case SYS_getuid:
	case SYS_geteuid:
	case SYS_getgid:
	case SYS_getegid:
		return 0;
	case SYS_sched_yield:
		svcSleepThread(YieldType_WithCoreMigration);
		return 0;
	case SYS_getrandom:
		randomGet(GUEST(void *, a), (size_t)(uint32_t)b);
		return (long)(uint32_t)b;
	case SYS_kill:
	case SYS_tkill:
	case SYS_tgkill:
	{
		int signal = (int)(number == SYS_tgkill ? c : b);

		if (signal == LINUX_SIGABRT)
			host_fatal("The game stopped (abort). See debug.txt in /switch/halo.");
		return 0;
	}
	case SYS_rt_sigprocmask:
		if (c)
			memset(GUEST(void *, c), 0, 8);
		return 0;
	case SYS_rt_sigaction:
		/* (musl's struct k_sigaction with 32-bit pointers: 20 bytes) */
		if (c)
			memset(GUEST(void *, c), 0, 20);
		return 0;
	case SYS_sigaltstack:
		return 0;
	case SYS_uname:
		return guest_uname((uint64_t)a);
	case SYS_sysinfo:
		return guest_sysinfo((uint64_t)a);
	case SYS_prlimit64:
		if (d)
			memset(GUEST(void *, d), 0xff, 16);
		return 0;
	case SYS_getrlimit:
		if (b)
			memset(GUEST(void *, b), 0xff, 8);
		return 0;

	default:
		host_logf(HOST_LOG_WARN, "guest system call %lld is not supported", number);
		return -38;
	}
}

void host_syscall_initialize(void)
{
	mutexInit(&directory_table_lock);
	mutexInit(&log_lock);
	time_initialize();
}
