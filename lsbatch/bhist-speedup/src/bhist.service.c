/*
 * Copyright (C) 2026 Bytedance Ltd. and/or its affiliates
 */

#define _GNU_SOURCE

#include "bhist.service.h"
#include "bhist.config.h"
#include "bhist.util.h"
#include "lproto.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#include <sqlite3.h>

/*
 * Verify the runtime SQLite library supports the SQL features this module
 * needs.  Partial indexes (CREATE INDEX ... WHERE) require SQLite 3.8.0; on
 * older libraries index creation fails with a bare syntax error.
 * @return: 0 when sufficient, otherwise -1 (reason already logged).
 */
int
checkSqliteRuntime(void)
{
    if (sqlite3_libversion_number() < 3008000) {
        ls_syslog(LOG_ERR, "sqlite too old runtime=%s need=3.8.0",
                  sqlite3_libversion());
        return -1;
    }
    return 0;
}

/*
 * Local authorization for management commands. The query protocol does not
 * use this result, and a client-supplied user name is not an authenticated
 * identity.
 */

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
        setError(error, errorSize,
                 "no service administrator is configured; "
                 "define ClusterAdmins in lsf.shared");
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
    /*
     * The kernel marks a process that dropped root as non-dumpable, which
     * hides /proc/<pid>/exe from the administrator and disables core dumps.
     * No privilege is retained after setuid, so restore the default.
     */
    (void)prctl(PR_SET_DUMPABLE, 1, 0, 0, 0);
    return 0;
}
/*
 * Shared service support for the server and loader. Both run in the
 * foreground under systemd or a manual launcher; neither forks into the
 * background or writes a PID file. A running instance is found by its
 * executable path under /proc.
 *
 * Directory creation accepts absolute paths only, creates each missing path
 * component, and never changes the mode or owner of an existing directory.
 */

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
 * Give one root-owned path to the service administrator.
 * @param[in] path: Existing path to reown.
 * @param[in] uid: Administrator user ID.
 * @param[in] gid: Administrator group ID.
 * @param[in] adminUser: Administrator name for logging.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
static int
reownRootPath(const char *path, uid_t uid, gid_t gid,
              const char *adminUser, char *error, size_t errorSize)
{
    struct stat st;

    if (stat(path, &st) != 0 || st.st_uid != 0)
        return 0;
    if (chown(path, uid, gid) != 0) {
        setError(error, errorSize, "cannot give %s to %s: %s",
                 path, adminUser, strerror(errno));
        return -1;
    }
    return 0;
}

/*
 * Create and hand over the runtime paths the service administrator
 * must write. Runs as root before switchServiceUser(): creates the database
 * parent directory, pre-creates the log file, and reowns root-owned
 * leftovers (database directory, database and its journal sidecars, log
 * file). The shared log directory and non-root-owned paths are never
 * reowned.
 * @param[in] dbPath: Absolute database path.
 * @param[in] logPath: Absolute log file path, or NULL to skip.
 * @param[in] adminUser: Service administrator name.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 0 on success, otherwise -1.
 */
int
prepareServicePaths(const char *dbPath, const char *logPath,
                    const char *adminUser,
                    char *error, size_t errorSize)
{
    static const char *const sidecarSuffixes[] = {
        "", "-wal", "-shm", "-journal"
    };
    struct passwd *account;
    char directory[4096];
    char sidecar[4096];
    uid_t uid;
    gid_t gid;
    size_t i;

    if (dbPath == NULL || dbPath[0] != '/' ||
        adminUser == NULL || adminUser[0] == '\0') {
        setError(error, errorSize, "invalid service path argument");
        return -1;
    }
    account = getpwnam(adminUser);
    if (account == NULL) {
        setError(error, errorSize,
                 "cannot resolve service administrator %s", adminUser);
        return -1;
    }
    uid = account->pw_uid;
    gid = account->pw_gid;
    if (uid == 0) {
        setError(error, errorSize,
                 "service administrator %s must have a non-root user ID",
                 adminUser);
        return -1;
    }

    if (parentDirectory(dbPath, directory, sizeof(directory),
                        error, errorSize) != 0)
        return -1;
    if (ensureDirectory(directory, error, errorSize) != 0)
        return -1;
    if (reownRootPath(directory, uid, gid, adminUser,
                      error, errorSize) != 0)
        return -1;

    /* Databases and journal sidecars may remain from earlier root runs. */
    for (i = 0; i < sizeof(sidecarSuffixes) / sizeof(sidecarSuffixes[0]);
         i++) {
        if (snprintf(sidecar, sizeof(sidecar), "%s%s",
                     dbPath, sidecarSuffixes[i]) >= (int)sizeof(sidecar)) {
            setError(error, errorSize, "database path is too long: %s",
                     dbPath);
            return -1;
        }
        if (reownRootPath(sidecar, uid, gid, adminUser,
                          error, errorSize) != 0)
            return -1;
    }

    if (logPath != NULL && logPath[0] == '/') {
        int fd;

        fd = open(logPath, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            setError(error, errorSize, "cannot create log file %s: %s",
                     logPath, strerror(errno));
            return -1;
        }
        close(fd);
        if (reownRootPath(logPath, uid, gid, adminUser,
                          error, errorSize) != 0)
            return -1;
    }
    return 0;
}

/*
 * Read the executable path of a process, dropping the " (deleted)" suffix
 * the kernel appends after the file is replaced, for example by an upgrade.
 * @param[in] pid: Process ID.
 * @param[out] path: Buffer receiving the executable path.
 * @param[in] pathSize: Size of path.
 * @return: 0 on success, otherwise -1 with errno set.
 */
static int
readProcessExe(pid_t pid, char *path, size_t pathSize)
{
    static const char deletedSuffix[] = " (deleted)";
    char link[64];
    ssize_t length;
    size_t suffixLength;

    snprintf(link, sizeof(link), "/proc/%ld/exe", (long)pid);
    length = readlink(link, path, pathSize - 1);
    if (length < 0)
        return -1;
    path[length] = '\0';
    suffixLength = sizeof(deletedSuffix) - 1;
    if ((size_t)length > suffixLength &&
        strcmp(path + length - suffixLength, deletedSuffix) == 0)
        path[length - suffixLength] = '\0';
    return 0;
}

/*
 * Test whether a process runs the given program.
 * /proc/<pid>/exe is readable only by the process owner and root, so other
 * users fall back to argv[0], which is exact because every launcher starts
 * the services through an absolute path.
 * @param[in] pid: Process ID.
 * @param[in] program: Resolved absolute path of the program.
 * @param[out] argCount: Optional number of command-line arguments.
 * @return: 1 when it matches, otherwise 0.
 */
static int
processRunsProgram(pid_t pid, const char *program, int *argCount)
{
    char path[64];
    char cmdline[4096];
    char exe[4096];
    char resolved[4096];
    ssize_t length;
    ssize_t i;
    int fd;
    int count;

    snprintf(path, sizeof(path), "/proc/%ld/cmdline", (long)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    length = read(fd, cmdline, sizeof(cmdline) - 1);
    close(fd);
    /* Kernel threads and zombies have an empty command line. */
    if (length <= 0)
        return 0;
    cmdline[length] = '\0';
    count = 0;
    for (i = 0; i < length; i++) {
        if (cmdline[i] == '\0')
            count++;
    }
    if (cmdline[length - 1] != '\0')
        count++;
    if (argCount != NULL)
        *argCount = count;

    if (readProcessExe(pid, exe, sizeof(exe)) == 0)
        return strcmp(exe, program) == 0;
    if (errno != EACCES && errno != EPERM)
        return 0;
    if (cmdline[0] != '/' || realpath(cmdline, resolved) == NULL)
        return 0;
    return strcmp(resolved, program) == 0;
}

/*
 * Read the state and parent of a process from /proc/<pid>/stat.
 * @param[in] pid: Process ID.
 * @param[out] state: Single-letter process state.
 * @param[out] parent: Parent process ID.
 * @return: 0 on success, otherwise -1.
 */
static int
readProcessStat(pid_t pid, char *state, pid_t *parent)
{
    char path[64];
    char buffer[1024];
    char *cursor;
    ssize_t length;
    long parentId;
    int fd;

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0)
        return -1;
    buffer[length] = '\0';
    /* The command name may contain spaces or ')', so parse after the last one. */
    cursor = strrchr(buffer, ')');
    if (cursor == NULL || sscanf(cursor + 1, " %c %ld", state, &parentId) != 2)
        return -1;
    *parent = (pid_t)parentId;
    return 0;
}

/*
 * Find a running service instance of a program installed beside the caller.
 *
 * A service has no lock or PID file; it is identified by its executable.
 * The loader and server install in the same directory, so the executable
 * is <directory of the calling program>/<name>. A process is the service
 * instance when it runs that executable with no arguments (commands always
 * carry one), is not a zombie, and its parent runs a different program
 * (server query handlers are forked without exec and so share the
 * executable). Each installation has its own executable path, so instances
 * of different clusters never match.
 *
 * @param[in] name: Program file name, such as "bhist-speedup-loader".
 * @param[out] pidOut: PID of the running instance.
 * @param[out] error: Buffer receiving an error message.
 * @param[in] errorSize: Size of error.
 * @return: 1 when an instance runs, 0 when none runs, otherwise -1.
 */
int
findServiceProcess(const char *name, pid_t *pidOut,
                   char *error, size_t errorSize)
{
    char selfExe[4096];
    char sibling[4096];
    char program[4096];
    struct dirent *entry;
    DIR *directory;
    char *slash;
    pid_t self;
    int found;

    if (pidOut != NULL)
        *pidOut = 0;
    self = getpid();
    if (readProcessExe(self, selfExe, sizeof(selfExe)) != 0) {
        setError(error, errorSize, "cannot resolve the running program: %s",
                 strerror(errno));
        return -1;
    }
    slash = strrchr(selfExe, '/');
    if (slash == NULL ||
        snprintf(sibling, sizeof(sibling), "%.*s/%s",
                 (int)(slash - selfExe), selfExe, name) >=
        (int)sizeof(sibling)) {
        setError(error, errorSize, "cannot build the path of %s", name);
        return -1;
    }
    if (realpath(sibling, program) == NULL) {
        setError(error, errorSize, "cannot resolve %s: %s",
                 sibling, strerror(errno));
        return -1;
    }
    directory = opendir("/proc");
    if (directory == NULL) {
        setError(error, errorSize, "cannot list processes in /proc: %s",
                 strerror(errno));
        return -1;
    }
    found = 0;
    while (!found && (entry = readdir(directory)) != NULL) {
        char *end;
        long value;
        pid_t pid;
        pid_t parent;
        char state;
        int argCount;

        errno = 0;
        value = strtol(entry->d_name, &end, 10);
        if (errno != 0 || end == entry->d_name || *end != '\0' || value <= 0)
            continue;
        pid = (pid_t)value;
        if (pid == self ||
            !processRunsProgram(pid, program, &argCount) || argCount != 1 ||
            readProcessStat(pid, &state, &parent) != 0 ||
            state == 'Z' || state == 'X' ||
            processRunsProgram(parent, program, NULL))
            continue;
        if (pidOut != NULL)
            *pidOut = pid;
        found = 1;
    }
    closedir(directory);
    return found;
}

/*
 * Send one state notification to systemd when started as a notify service.
 * Implements the sd_notify datagram protocol without libsystemd; outside
 * systemd NOTIFY_SOCKET is unset and this does nothing.
 * @param[in] state: Newline-separated assignments such as "READY=1".
 * @return: None.
 */
void
serviceNotify(const char *state)
{
    struct sockaddr_un address;
    const char *socketPath;
    size_t pathLength;
    int fd;

    socketPath = getenv("NOTIFY_SOCKET");
    if (socketPath == NULL ||
        (socketPath[0] != '/' && socketPath[0] != '@'))
        return;
    pathLength = strlen(socketPath);
    if (pathLength >= sizeof(address.sun_path))
        return;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socketPath, pathLength);
    /* A leading '@' names a socket in the abstract namespace. */
    if (address.sun_path[0] == '@')
        address.sun_path[0] = '\0';
    fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    (void)sendto(fd, state, strlen(state), MSG_NOSIGNAL,
                 (struct sockaddr *)&address,
                 (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                             pathLength));
    close(fd);
}
