#define _GNU_SOURCE

#include "daemon.h"
#include "bhist.config.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/*
 * Local authorization for management commands. The query protocol does not
 * use this result, and a client-supplied user name is not an authenticated
 * identity.
 */

/*
 * Format a management error when an output buffer is available.
 * @param[out] error: Optional buffer receiving the formatted message.
 * @param[in] errorSize: Size of error.
 * @param[in] format: printf-style message format followed by its arguments.
 * @return: None.
 */
static void
setError(char *error, size_t errorSize, const char *format, ...)
{
    va_list arguments;

    if (error == NULL || errorSize == 0)
        return;
    va_start(arguments, format);
    (void)vsnprintf(error, errorSize, format, arguments);
    va_end(arguments);
}

/*
 * Check the original local identity before entering a management command.
 * @param[in] config: Configuration containing the resolved administrators.
 * @param[in] callerUid: Real UID saved at program entry.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 for root or a configured administrator, otherwise -1.
 */
int
checkAdminUser(const struct config *config, uid_t callerUid,
               char *error, size_t errorSize)
{
    struct passwd *caller;
    size_t i;

    if (getuid() != callerUid || geteuid() != callerUid) {
        setError(error, errorSize,
                 "management commands require matching real and effective users");
        return -1;
    }
    if (callerUid == 0)
        return 0;
    caller = getpwuid(callerUid);
    if (caller == NULL) {
        setError(error, errorSize, "cannot resolve calling user ID %lu",
                 (unsigned long)callerUid);
        return -1;
    }
    if (config != NULL) {
        for (i = 0; i < config->adminCount; i++) {
            if (strcmp(caller->pw_name, config->admins[i]) == 0)
                return 0;
        }
    }
    setError(error, errorSize,
             "this management command must be run as root or a cluster administrator");
    return -1;
}

/*
 * Establish the selected administrator identity before dispatching a command.
 * @param[in] user: Configured administrator selected as the runtime account.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1; callers must exit on failure.
 */
int
switchServiceUser(const char *user, char *error, size_t errorSize)
{
    struct passwd *account;
    uid_t uid;
    gid_t gid;

    if (user == NULL || user[0] == '\0') {
        setError(error, errorSize, "no service administrator is configured");
        return -1;
    }
    account = getpwnam(user);
    if (account == NULL) {
        setError(error, errorSize, "cannot resolve service administrator %s",
                 user);
        return -1;
    }
    uid = account->pw_uid;
    gid = account->pw_gid;
    if (uid == 0) {
        setError(error, errorSize,
                 "service administrator %s must have a non-root user ID", user);
        return -1;
    }
    if (getuid() != geteuid()) {
        setError(error, errorSize,
                 "management commands require matching real and effective users");
        return -1;
    }
    if (getuid() != 0) {
        if (getuid() == uid)
            return 0;
        setError(error, errorSize,
                 "run this command as root or the service administrator %s", user);
        return -1;
    }
    if (initgroups(user, gid) != 0) {
        setError(error, errorSize, "cannot initialize groups for %s: %s",
                 user, strerror(errno));
        return -1;
    }
    if (setgid(gid) != 0) {
        setError(error, errorSize, "cannot switch to group %lu for %s: %s",
                 (unsigned long)gid, user, strerror(errno));
        return -1;
    }
    if (setuid(uid) != 0) {
        setError(error, errorSize, "cannot switch to user %s: %s",
                 user, strerror(errno));
        return -1;
    }
    return 0;
}
/*
 * Shared PID-lock and daemon behavior for the server and loader. A daemon
 * locks and fsyncs its PID file before notifying the starting parent through
 * the startup pipe. Status and stop rely on that lock instead of kill(pid, 0),
 * avoiding false positives from PID reuse or a partially started process.
 *
 * Directory creation accepts absolute paths only, creates each missing path
 * component, and never changes the mode or owner of an existing directory.
 */

/* Carries daemon startup status from the forked child to its caller. */
struct startupMessage {
    int result;
    int savedErrno;
    pid_t pid;
};

/*
 * Extract the parent directory from an absolute file path.
 * @param[in] path: Absolute file path.
 * @param[out] parent: Buffer receiving the parent directory.
 * @param[in] parentSize: Size of parent.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
parentDirectory(const char *path, char *parent, size_t parentSize,
                     char *error, size_t errorSize)
{
    const char *slash;
    size_t length;

    if (path == NULL || path[0] != '/') {
        setError(error, errorSize, "path must be absolute: %s",
                 path != NULL ? path : "(null)");
        return -1;
    }
    slash = strrchr(path, '/');
    if (slash == NULL) {
        setError(error, errorSize, "path has no parent: %s", path);
        return -1;
    }
    length = slash == path ? 1U : (size_t)(slash - path);
    if (length >= parentSize) {
        setError(error, errorSize, "parent directory is too long: %s", path);
        return -1;
    }
    memcpy(parent, path, length);
    parent[length] = '\0';
    return 0;
}

/*
 * Create one directory or accept an existing directory at the same path.
 * @param[in] path: Absolute directory path for one component.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
ensureOneDirectory(const char *path, char *error, size_t errorSize)
{
    struct stat st;

    if (mkdir(path, 0755) == 0)
        return 0;
    if (errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode))
        return 0;
    setError(error, errorSize, "cannot create directory %s: %s",
             path, strerror(errno));
    return -1;
}

/*
 * Create every missing component of an absolute directory path.
 * @param[in] path: Directory path to create.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
ensureDirectory(const char *path, char *error, size_t errorSize)
{
    char copy[4096];
    char *component;
    char *cursor;

    if (path == NULL || path[0] != '/' ||
        snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy)) {
        setError(error, errorSize,
                 "directory path must be absolute and not too long");
        return -1;
    }
    if (strcmp(copy, "/") == 0)
        return ensureOneDirectory(copy, error, errorSize);

    component = copy + 1;
    for (cursor = component; ; cursor++) {
        char saved;
        size_t componentLength;

        if (*cursor != '/' && *cursor != '\0')
            continue;
        saved = *cursor;
        *cursor = '\0';
        componentLength = (size_t)(cursor - component);
        if (componentLength == 0 ||
            (componentLength == 1 && component[0] == '.') ||
            (componentLength == 2 && component[0] == '.' &&
             component[1] == '.')) {
            setError(error, errorSize,
                     "directory path contains an invalid component: %s",
                     path);
            return -1;
        }
        if (ensureOneDirectory(copy, error, errorSize) != 0)
            return -1;
        if (saved == '\0')
            break;
        *cursor = saved;
        component = cursor + 1;
    }
    return 0;
}

/*
 * Inspect the owner of a whole-file advisory write lock.
 * @param[in] fd: Open PID-file descriptor.
 * @param[out] pidOut: Optional owner PID.
 * @return: 1 when locked, 0 when unlocked, otherwise -1.
 */
static int
lockOwner(int fd, pid_t *pidOut)
{
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(fd, F_GETLK, &lock) != 0)
        return -1;
    if (lock.l_type == F_UNLCK)
        return 0;
    if (pidOut != NULL)
        *pidOut = lock.l_pid;
    return 1;
}

/*
 * Attempt to acquire the whole-file advisory write lock without waiting.
 * @param[in] fd: Open PID-file descriptor.
 * @return: 0 when acquired, 1 when held elsewhere, otherwise -1.
 */
static int
tryLock(int fd)
{
    struct flock lock;

    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 0;
    if (fcntl(fd, F_SETLK, &lock) == 0)
        return 0;
    if (errno == EACCES || errno == EAGAIN)
        return 1;
    return -1;
}

/*
 * Inspect the lock state of a PID file.
 * @param[in] path: PID file path.
 * @param[out] pidOut: PID recorded for a running instance.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 1 when running, 0 when stopped, otherwise -1.
 */
int
pidFileStatus(const char *path, pid_t *pidOut,
                   char *error, size_t errorSize)
{
    int fd;
    int result;

    if (pidOut != NULL)
        *pidOut = 0;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;
        setError(error, errorSize, "cannot open pid file %s: %s", path,
                 strerror(errno));
        return -1;
    }
    result = lockOwner(fd, pidOut);
    if (result < 0)
        setError(error, errorSize, "cannot inspect pid lock %s: %s", path,
                 strerror(errno));
    close(fd);
    return result;
}

/*
 * Remove an unlocked stale PID file.
 * @param[in] path: PID file path.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 when removed or absent, otherwise -1.
 */
int
pidFileRemoveStale(const char *path, char *error, size_t errorSize)
{
    struct stat opened;
    struct stat current;
    int fd;
    int result;

    fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT)
            return 0;
        setError(error, errorSize, "cannot open stale pid file %s: %s",
                 path, strerror(errno));
        return -1;
    }
    result = tryLock(fd);
    if (result != 0) {
        if (result > 0)
            setError(error, errorSize, "pid file is locked: %s", path);
        else
            setError(error, errorSize, "cannot lock pid file %s: %s",
                     path, strerror(errno));
        close(fd);
        return result > 0 ? 1 : -1;
    }
    if (fstat(fd, &opened) != 0) {
        setError(error, errorSize, "cannot stat pid file %s: %s", path,
                 strerror(errno));
        close(fd);
        return -1;
    }
    /*
     * Another start may replace the path after this process acquires the lock.
     * Remove it only when the path still names the inode opened above.
     */
    if (stat(path, &current) == 0) {
        if (current.st_dev == opened.st_dev &&
            current.st_ino == opened.st_ino && unlink(path) != 0 &&
            errno != ENOENT) {
            setError(error, errorSize,
                     "cannot remove stale pid file %s: %s",
                     path, strerror(errno));
            close(fd);
            return -1;
        }
    } else if (errno != ENOENT) {
        setError(error, errorSize, "cannot stat pid file %s: %s", path,
                 strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

/*
 * Replace and durably write the PID stored in a locked PID file.
 * @param[in] fd: Locked PID-file descriptor.
 * @param[in] pid: Daemon PID to persist.
 * @return: 0 on success, otherwise -1.
 */
static int
writePid(int fd, pid_t pid)
{
    char buffer[64];
    int length;
    ssize_t written;

    length = snprintf(buffer, sizeof(buffer), "%ld\n", (long)pid);
    if (length <= 0 || (size_t)length >= sizeof(buffer) ||
        ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0)
        return -1;
    written = write(fd, buffer, (size_t)length);
    if (written != length)
        return -1;
    return fsync(fd);
}

/*
 * Send a complete daemon startup result through the startup pipe.
 * @param[in] fd: Writable startup-pipe descriptor.
 * @param[in] result: Daemon initialization result.
 * @param[in] savedErrno: errno captured by the daemon child.
 * @param[in] pid: Started daemon PID when successful.
 * @return: None; a closed pipe is tolerated because the parent has gone.
 */
static void
sendStartupMessage(int fd, int result, int savedErrno, pid_t pid)
{
    struct startupMessage message;
    const unsigned char *cursor;
    size_t remaining;

    message.result = result;
    message.savedErrno = savedErrno;
    message.pid = pid;
    cursor = (const unsigned char *)&message;
    remaining = sizeof(message);
    while (remaining > 0) {
        ssize_t written;

        written = write(fd, cursor, remaining);
        if (written > 0) {
            cursor += (size_t)written;
            remaining -= (size_t)written;
        } else if (written < 0 && errno == EINTR) {
            continue;
        } else {
            break;
        }
    }
}

/*
 * Receive one complete daemon startup result from the child.
 * @param[in] fd: Readable startup-pipe descriptor.
 * @param[out] message: Startup result structure.
 * @return: 0 on success, otherwise -1 for EOF or read failure.
 */
static int
receiveStartupMessage(int fd, struct startupMessage *message)
{
    unsigned char *cursor;
    size_t remaining;

    cursor = (unsigned char *)message;
    remaining = sizeof(*message);
    while (remaining > 0) {
        ssize_t received;

        received = read(fd, cursor, remaining);
        if (received > 0) {
            cursor += (size_t)received;
            remaining -= (size_t)received;
        } else if (received < 0 && errno == EINTR) {
            continue;
        } else {
            return -1;
        }
    }
    return 0;
}

/*
 * Redirect daemon standard descriptors to /dev/null and its runtime log.
 * @param[in] logPath: Log file receiving standard output and error.
 * @return: 0 on success, otherwise -1.
 */
static int
redirectStandardFds(const char *logPath)
{
    int flags;
    int logFd;
    int nullFd;

    nullFd = open("/dev/null", O_RDONLY);
    if (nullFd < 0)
        return -1;
    logFd = open(logPath, O_WRONLY | O_APPEND | O_CREAT, 0644);
    if (logFd < 0) {
        close(nullFd);
        return -1;
    }
    if (fchmod(logFd, 0644) != 0) {
        close(nullFd);
        close(logFd);
        return -1;
    }
    if (dup2(nullFd, STDIN_FILENO) < 0 ||
        dup2(logFd, STDOUT_FILENO) < 0 ||
        dup2(logFd, STDERR_FILENO) < 0) {
        close(nullFd);
        close(logFd);
        return -1;
    }
    if (nullFd > STDERR_FILENO)
        close(nullFd);
    if (logFd > STDERR_FILENO)
        close(logFd);
    flags = fcntl(STDOUT_FILENO, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(STDOUT_FILENO, F_SETFD, flags & ~FD_CLOEXEC);
    flags = fcntl(STDERR_FILENO, F_GETFD, 0);
    if (flags >= 0)
        (void)fcntl(STDERR_FILENO, F_SETFD, flags & ~FD_CLOEXEC);
    return 0;
}

/*
 * Run a callback as a daemon while holding a PID-file lock.
 * @param[in] pidPath: PID file path.
 * @param[in] logPath: Path receiving standard output and standard error.
 * @param[in] callback: Daemon entry point.
 * @param[in] context: Callback context.
 * @param[out] pidOut: Started daemon PID.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 when started, 1 when already running, otherwise -1.
 */
int
daemonStart(const char *pidPath, const char *logPath,
                 daemonCallback callback, void *context,
                 pid_t *pidOut, char *error, size_t errorSize)
{
    struct startupMessage message;
    pid_t existingPid;
    pid_t child;
    int pipeFds[2];
    int status;

    if (pidOut != NULL)
        *pidOut = 0;
    if (pidPath == NULL || pidPath[0] != '/' || logPath == NULL ||
        logPath[0] != '/' || callback == NULL) {
        setError(error, errorSize, "invalid daemon paths or callback");
        return -1;
    }
    status = pidFileStatus(pidPath, &existingPid, error, errorSize);
    if (status != 0) {
        if (status > 0 && pidOut != NULL)
            *pidOut = existingPid;
        return status > 0 ? 1 : -1;
    }
    if (pipe(pipeFds) != 0) {
        setError(error, errorSize, "cannot create daemon pipe: %s",
                 strerror(errno));
        return -1;
    }
    child = fork();
    if (child < 0) {
        setError(error, errorSize, "cannot fork daemon: %s",
                 strerror(errno));
        close(pipeFds[0]);
        close(pipeFds[1]);
        return -1;
    }
    if (child == 0) {
        int lockFd;
        int lockResult;
        int callbackResult;
        int savedErrno;

        close(pipeFds[0]);
        (void)umask(022);
        if (setsid() < 0 || chdir("/") != 0 ||
            redirectStandardFds(logPath) != 0) {
            savedErrno = errno;
            sendStartupMessage(pipeFds[1], -1, savedErrno, 0);
            _exit(1);
        }
        lockFd = open(pidPath, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (lockFd < 0) {
            savedErrno = errno;
            sendStartupMessage(pipeFds[1], -1, savedErrno, 0);
            _exit(1);
        }
        if (fchmod(lockFd, 0644) != 0) {
            savedErrno = errno;
            sendStartupMessage(pipeFds[1], -1, savedErrno, 0);
            close(lockFd);
            _exit(1);
        }
        /* Keep the fcntl lock for the complete callback lifetime. */
        lockResult = tryLock(lockFd);
        if (lockResult != 0) {
            savedErrno = errno;
            sendStartupMessage(pipeFds[1],
                                lockResult > 0 ? 1 : -1,
                                savedErrno, 0);
            close(lockFd);
            _exit(lockResult > 0 ? 0 : 1);
        }
        if (writePid(lockFd, getpid()) != 0) {
            savedErrno = errno;
            sendStartupMessage(pipeFds[1], -1, savedErrno, 0);
            close(lockFd);
            _exit(1);
        }
        /*
         * Report success only after session creation, redirection, PID locking,
         * and fsync have completed.
         */
        sendStartupMessage(pipeFds[1], 0, 0, getpid());
        close(pipeFds[1]);
        callbackResult = callback(context);
        (void)unlink(pidPath);
        close(lockFd);
        _exit(callbackResult == 0 ? 0 : 1);
    }

    close(pipeFds[1]);
    memset(&message, 0, sizeof(message));
    if (receiveStartupMessage(pipeFds[0], &message) != 0) {
        int savedErrno;

        savedErrno = errno;
        close(pipeFds[0]);
        (void)waitpid(child, NULL, 0);
        setError(error, errorSize,
                  "daemon exited before writing its pid: %s",
                  strerror(savedErrno != 0 ? savedErrno : EIO));
        return -1;
    }
    close(pipeFds[0]);
    if (message.result != 0) {
        (void)waitpid(child, NULL, 0);
        if (message.result > 0) {
            status = pidFileStatus(pidPath, &existingPid,
                                        error, errorSize);
            if (status > 0 && pidOut != NULL)
                *pidOut = existingPid;
            return 1;
        }
        setError(error, errorSize, "cannot start daemon: %s",
                 strerror(message.savedErrno != 0 ?
                 message.savedErrno : EIO));
        return -1;
    }
    if (pidOut != NULL)
        *pidOut = message.pid;
    return 0;
}

/*
 * Sleep for a millisecond duration while retrying interrupted nanosleep calls.
 * @param[in] milliseconds: Requested sleep duration.
 * @return: None.
 */
static void
sleepMs(int milliseconds)
{
    struct timespec duration;

    duration.tv_sec = milliseconds / 1000;
    duration.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    while (nanosleep(&duration, &duration) != 0 && errno == EINTR)
        ;
}

/*
 * Stop the daemon identified by a locked PID file.
 * @param[in] pidPath: PID file path.
 * @param[in] timeoutMs: Graceful-stop timeout in milliseconds.
 * @param[out] pidOut: Target daemon PID.
 * @param[out] wasRunning: Whether an instance was running before this call.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 when stopped or already stopped, otherwise -1.
 */
int
daemonStop(const char *pidPath, int timeoutMs,
                pid_t *pidOut, int *wasRunning,
                char *error, size_t errorSize)
{
    pid_t pid;
    int elapsed;
    int status;

    if (pidOut != NULL)
        *pidOut = 0;
    if (wasRunning != NULL)
        *wasRunning = 0;
    status = pidFileStatus(pidPath, &pid, error, errorSize);
    if (status < 0)
        return -1;
    if (status == 0)
        return pidFileRemoveStale(pidPath, error, errorSize);
    if (pidOut != NULL)
        *pidOut = pid;
    if (wasRunning != NULL)
        *wasRunning = 1;
    if (kill(pid, SIGTERM) != 0 && errno != ESRCH) {
        setError(error, errorSize, "cannot stop pid %ld: %s", (long)pid,
                 strerror(errno));
        return -1;
    }
    for (elapsed = 0; elapsed < timeoutMs; elapsed += 100) {
        sleepMs(100);
        status = pidFileStatus(pidPath, NULL, error, errorSize);
        if (status == 0)
            return pidFileRemoveStale(pidPath, error, errorSize);
        if (status < 0)
            return -1;
    }
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH) {
        setError(error, errorSize, "cannot kill pid %ld: %s", (long)pid,
                 strerror(errno));
        return -1;
    }
    for (elapsed = 0; elapsed < 5000; elapsed += 100) {
        sleepMs(100);
        status = pidFileStatus(pidPath, NULL, error, errorSize);
        if (status == 0)
            return pidFileRemoveStale(pidPath, error, errorSize);
        if (status < 0)
            return -1;
    }
    setError(error, errorSize, "pid %ld did not release %s", (long)pid,
             pidPath);
    return -1;
}
