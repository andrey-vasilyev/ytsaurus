#include "stdafx.h"
#include "proc.h"

#include <util/stream/file.h>

#include <util/string/vector.h>

#include <util/system/fs.h>
#include <util/system/info.h>

#include <util/folder/iterator.h>
#include <util/folder/dirut.h>
#include <util/folder/filelist.h>

#ifdef _unix_
    #include <spawn.h>
    #include <stdio.h>
    #include <dirent.h>
    #include <sys/types.h>
    #include <sys/wait.h>
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

i64 GetProcessRss(int pid)
{
    Stroka path = "/proc/self/statm";
    if (pid != -1) {
        path = Sprintf("/proc/%d/statm", pid);
    }

    TIFStream memoryStatFile(path);
    auto memoryStatFields = splitStroku(memoryStatFile.ReadLine(), " ");
    return FromString<i64>(memoryStatFields[1]) * NSystemInfo::GetPageSize();
}

#ifdef _unix_

i64 GetUserRss(int uid)
{
    YCHECK(uid > 0);
    // Column of rss of all processes for given user in kb.
    auto command = Sprintf("ps -u %d -o rss --no-headers", uid);

    // Read + close on exec.
    FILE *fd = popen(~command, "re");

    if (!fd) {
        THROW_ERROR_EXCEPTION(
            "Failed to get memory usage for UID %d: popen failed",
            uid) << TError::FromSystem();
    }

    i64 result = 0;
    int rss;
    int n;
    while ((n = fscanf(fd, "%d", &rss)) != EOF) {
        if (n == 1) {
            result += rss;
        } else {
            THROW_ERROR_EXCEPTION(
                "Failed to get memory usage for UID %d: fscanf failed",
                uid) << TError::FromSystem();
        }
    }

    // ToDo(psushin): consider checking pclose errors.
    pclose(fd);
    return result * 1024;
}

// The caller must be sure that it has root privileges.
void KillallByUser(int uid)
{
    YCHECK(uid > 0);
    auto pid = fork();

    // We are forking here in order not to give the root priviledges to the parent process ever,
    // because we cannot know what other threads are doing.
    if (pid == 0) {
        // Child process
        YCHECK(setuid(0) == 0);
        YCHECK(setuid(uid) == 0);
        // Send sigkill to all available processes.
        auto res = kill(-1, 9);
        if (res == -1) {
            YCHECK(errno == ESRCH);
        }
        _exit(0);
    }

    // Parent process
    if (pid < 0) {
        THROW_ERROR_EXCEPTION(
            "Failed to kill processes for uid %d: fork failed",
            uid) << TError::FromSystem();
    }

    int status = 0;
    {
        int result = waitpid(pid, &status, WUNTRACED);
        if (result < 0) {
            THROW_ERROR_EXCEPTION(
                "Failed to kill processes for uid %d: waitpid failed",
                uid) << TError::FromSystem();
        }
        YCHECK(result == pid);
    }

    auto statusError = StatusToError(status);
    if (!statusError.IsOK()) {
        THROW_ERROR_EXCEPTION(
            "Failed to kill processes for uid %d: waitpid failed",
            uid) << statusError;
    }
}

void RemoveDirAsRoot(const Stroka& path)
{
    // Allocation after fork can lead to a deadlock inside LFAlloc.
    // To avoid allocation we list contents of the directory before fork.

    // Copy-paste from RemoveDirWithContents (util/folder/dirut.cpp)
    auto path_ = path;
    SlashFolderLocal(path_);

    TDirIterator dir(path_);
    std::vector<Stroka> contents;

    for (TDirIterator::TIterator it = dir.Begin(); it != dir.End(); ++it) {
        switch (it->fts_info) {
            case FTS_F:
            case FTS_DEFAULT:
            case FTS_DP:
            case FTS_SL:
            case FTS_SLNONE:
                contents.push_back(it->fts_path);
                break;
        }
    }

    auto pid = fork();
    // We are forking here in order not to give the root privileges to the parent process ever,
    // because we cannot know what other threads are doing.
    if (pid == 0) {
        // Child process
        YCHECK(setuid(0) == 0);
        for (int i = 0; i < contents.size(); ++i) {
            if (NFs::Remove(~contents[i])) {
                _exit(1);
            }
        }

        _exit(0);
    }

    auto throwError = [=] (const Stroka& msg, const TError& error) {
        THROW_ERROR_EXCEPTION(
            "Failed to remove directory %s: %s",
            ~path,
            ~msg) << error;
    };

    // Parent process
    if (pid < 0) {
        throwError("fork failed", TError::FromSystem());
    }

    int status = 0;
    {
        int result = waitpid(pid, &status, WUNTRACED);
        if (result < 0) {
            throwError("waitpid failed", TError());
        }
        YCHECK(result == pid);
    }

    auto statusError = StatusToError(status);
    if (!statusError.IsOK()) {
        throwError("waitpid failed", statusError);
    }
}

TError StatusToError(int status)
{
    if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
        return TError();
    } else if (WIFSIGNALED(status)) {
        return TError("Process terminated by signal %d",  WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        return TError("Process stopped by signal %d",  WSTOPSIG(status));
    } else if (WIFEXITED(status)) {
        return TError("Process exited with value %d",  WEXITSTATUS(status));
    } else {
        return TError("Unknown status %d", status);
    }
}

void CloseAllDescriptors()
{
    // Called after fork.
    // Avoid allocations, may lead to deadlock in LFAlloc.

#ifdef _linux_
    DIR *dp = ::opendir("/proc/self/fd");
    YCHECK(dp != NULL);

    int dirfd = ::dirfd(dp);
    YCHECK(dirfd >= 0);

    struct dirent *ep;
    while ((ep = ::readdir(dp)) != nullptr) {
        char* begin = ep->d_name;
        char* end = nullptr;
        int fd = static_cast<int>(strtol(begin, &end, 10));
        if (fd != dirfd && begin != end) {
            YCHECK(::close(fd) == 0);
        }
    }

    YCHECK(::closedir(dp) == 0);
#endif
}

std::vector<int> GetAllDescriptors()
{
    std::vector<int> result;

#ifdef _linux_
    DIR *dp = ::opendir("/proc/self/fd");
    YCHECK(dp != NULL);

    int dirfd = ::dirfd(dp);
    YCHECK(dirfd >= 0);

    struct dirent *ep;
    while ((ep = ::readdir(dp)) != nullptr) {
        char* begin = ep->d_name;
        char* end = nullptr;
        int fd = static_cast<int>(strtol(begin, &end, 10));
        if (fd != dirfd && begin != end) {
            result.push_back(fd);
        }
    }

    YCHECK(::closedir(dp) == 0);
#endif
    return result;
}

void SafeClose(int fd, bool ignoreInvalidFd)
{
    while (true) {
        auto res = close(fd);
        if (res == -1) {
            switch (errno) {
            case EINTR:
                break;

            case EBADF:
                if (ignoreInvalidFd) {
                    return;
                } // otherwise fall through and throw exception.

            default:
                THROW_ERROR_EXCEPTION("close failed")
                    << TError::FromSystem();
            }
        } else {
            return;
        }
    }
}

int SetMemoryLimit(rlim_t memoryLimit)
{
    struct rlimit rlimit = {memoryLimit, RLIM_INFINITY};

    return setrlimit(RLIMIT_AS, &rlimit);
}

int Spawn(const char* path,
          std::initializer_list<const char*> arguments,
          const std::vector<int>& fileIdsToClose)
{
    auto storeStrings = [](std::initializer_list<const char*> strings) -> std::vector<std::vector<char>> {
        std::vector<std::vector<char>> result;
        FOREACH (auto item, strings) {
            result.push_back(std::vector<char>(item, item + strlen(item) + 1));
        }
        return result;
    };

    posix_spawn_file_actions_t fileActions;
    YCHECK(posix_spawn_file_actions_init(&fileActions) == 0);

    FOREACH (auto fileId, fileIdsToClose) {
        YCHECK(posix_spawn_file_actions_addclose(&fileActions, fileId) == 0);
    }

    posix_spawnattr_t attributes;
    YCHECK(posix_spawnattr_init(&attributes) == 0);
#ifdef POSIX_SPAWN_USEVFORK
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_USEVFORK);
#endif

    std::vector<std::vector<char>> argContainer = storeStrings(arguments);

    std::vector<char *> args;
    FOREACH (auto& x, argContainer) {
        args.push_back(&x[0]);
    }
    args.push_back(NULL);

    int processId;
    int errCode = posix_spawnp(&processId,
                               path,
                               &fileActions,
                               &attributes,
                               &args[0],
                               NULL);

    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&fileActions);

    if (errCode != 0) {
        THROW_ERROR_EXCEPTION("posix_spawn failed. Path=%s. %s", path, strerror(errCode));
    }
    return processId;
}

#else

void KillallByUser(int uid)
{
    UNUSED(uid);
    YUNIMPLEMENTED();
}

TError StatusToError(int status)
{
    UNUSED(status);
    YUNIMPLEMENTED();
}

i64 GetUserRss(int uid)
{
    UNUSED(uid);
    YUNIMPLEMENTED();
}

void RemoveDirAsRoot(const Stroka& path)
{
    UNUSED(path);
    YUNIMPLEMENTED();
}

void CloseAllDescriptors()
{
    YUNIMPLEMENTED();
}

void SafeClose(int fd, bool ignoreInvalidFd)
{
    YUNIMPLEMENTED();
}

int Spawn(const char* path,
          std::initializer_list<const char*> arguments,
          const std::vector<int>& fileIdsToClose)
{
    UNUSED(path);
    UNUSED(arguments);
    UNUSED(fileIdsToClose);
    YUNIMPLEMENTED();
}

std::vector<int> GetAllDescriptors()
{
    YUNIMPLEMENTED();
}

#endif


////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
