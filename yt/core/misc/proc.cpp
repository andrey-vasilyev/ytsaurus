#include "proc.h"
#include "process.h"
#include "string.h"

#include <yt/core/logging/log.h>

#include <yt/core/misc/common.h>
#include <yt/core/misc/fs.h>
#include <yt/core/misc/string.h>

#include <yt/core/tools/registry.h>

#include <yt/core/ytree/convert.h>

#include <yt/core/misc/fs.h>

#include <util/stream/file.h>

#include <util/string/vector.h>

#include <util/system/info.h>
#include <util/system/yield.h>
#include <util/system/fstat.h>
#include <util/folder/iterator.h>

#ifdef _unix_
    #include <stdio.h>
    #include <dirent.h>
    #include <errno.h>
    #include <pwd.h>
    #include <sys/ioctl.h>
    #include <sys/types.h>
    #include <sys/resource.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif
#ifdef _linux_
    #include <pty.h>
    #include <grp.h>
    #include <utmp.h>
    #include <sys/prctl.h>
#endif
#ifdef _darwin_
    #include <util.h>
#endif

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

static const NLogging::TLogger Logger("Proc");

const int RemoveAsRootAttemptCount = 5;

////////////////////////////////////////////////////////////////////////////////

std::vector<int> GetPidsByUid(int uid)
{
#ifdef _linux_
    std::vector<int> result;

    DIR *dirStream = ::opendir("/proc");
    YCHECK(dirStream != nullptr);

    struct dirent *ep;
    while ((ep = ::readdir(dirStream)) != nullptr) {
        const char* begin = ep->d_name;
        char* end = nullptr;
        int pid = static_cast<int>(strtol(begin, &end, 10));
        if (begin == end) {
            // Not a pid.
            continue;
        }

        auto path = Format("/proc/%v", pid);
        struct stat buf;
        int res = ::stat(~path, &buf);

        if (res == 0) {
            if (buf.st_uid == uid || uid == -1) {
                result.push_back(pid);
            }
        } else {
            // Assume that the process has already completed.
            auto errno_ = errno;
            LOG_DEBUG(TError::FromSystem(), "Failed to get UID for PID %v: stat failed",
                pid);
            YCHECK(errno_ == ENOENT || errno_ == ENOTDIR);
        }
    }

    YCHECK(::closedir(dirStream) == 0);
    return result;

#else
    return std::vector<int>();
#endif
}

i64 GetProcessRss(int pid)
{
#ifdef _linux_
    Stroka path = "/proc/self/statm";
    if (pid != -1) {
        path = Format("/proc/%v/statm", pid);
    }

    TIFStream memoryStatFile(path);
    auto memoryStatFields = splitStroku(memoryStatFile.ReadLine(), " ");
    return FromString<i64>(memoryStatFields[1]) * NSystemInfo::GetPageSize();
#else
    return 0;
#endif
}

Stroka GetProcessName(int pid)
{
#ifdef _linux_
    Stroka path = Format("/proc/%v/comm", pid);
    return Trim(TFileInput(path).ReadAll(), "\n");
#else
    return "";
#endif
}

std::vector<Stroka> GetProcessCommandLine(int pid)
{
#ifdef _linux_
    Stroka path = Format("/proc/%v/cmdline", pid);
    auto raw = TFileInput(path).ReadAll();
    std::vector<Stroka> result;
    auto begin = 0;
    while (begin < raw.length()) {
        auto end = raw.find('\0', begin);
        if (end == Stroka::npos) {
            result.push_back(raw.substr(begin));
            begin = raw.length();
        } else {
            result.push_back(raw.substr(begin, end - begin));
            begin = end + 1;
        }
    }

    return result;
#else
    return std::vector<Stroka>();
#endif
}

#ifdef _unix_

void RemoveDirAsRoot(const Stroka& path)
{
    // Child process
    SafeSetUid(0);
    execl("/bin/rm", "/bin/rm", "-rf", path.c_str(), (void*)nullptr);

    THROW_ERROR_EXCEPTION("Failed to remove directory %Qv: execl failed",
        path) << TError::FromSystem();
}

void RemoveDirContentAsRoot(const Stroka& path)
{
    // Child process
    SafeSetUid(0);

    if (!TFileStat(path).IsDir()) {
        THROW_ERROR_EXCEPTION("Path %Qv is not directory",
            path);
    }

    auto isRemovable = [&] (TStlIterator<TDirIterator>::TIterator it) {
        if (it->fts_info == FTS_DOT || it->fts_info == FTS_D) {
            return false;
        }
        if (path.StartsWith(it->fts_path)) {
            return false;
        }

        return true;
    };

    bool removed = false;
    std::vector<TError> attemptErrors;
    for (int attempt = 0; attempt < RemoveAsRootAttemptCount; ++attempt) {
        std::vector<TError> innerErrors;
        {
            TDirIterator dir(path);
            for (auto it = dir.Begin(); it != dir.End(); ++it) {
                try {
                    if (isRemovable(it)) {
                        NFS::Remove(it->fts_path);
                    }
                } catch (const std::exception& ex) {
                    innerErrors.push_back(TError("Failed to remove path %v", it->fts_path)
                        << ex);
                }
            }
        }

        std::vector<Stroka> unremovableItems;
        {
            TDirIterator dir(path);
            for (auto it = dir.Begin(); it != dir.End(); ++it) {
                if (isRemovable(it)) {
                    unremovableItems.push_back(it->fts_path);
                }
            }
        }

        if (unremovableItems.empty()) {
            removed = true;
            break;
        }

        auto error = TError("Failed to remove items %v in directory %v",
            unremovableItems,
            path);

        error = NFS::AttachLsofOutput(error, path);
        error = NFS::AttachFindOutput(error, path);
        error.InnerErrors() = std::move(innerErrors);

        attemptErrors.push_back(error);

        Sleep(TDuration::Seconds(1));
    }

    if (!removed) {
        auto error = TError("Failed to remove directory %v contents", path);
        error.InnerErrors() = std::move(attemptErrors);
        THROW_ERROR error;
    }
}

void MountTmpfsAsRoot(TMountTmpfsConfigPtr config)
{
    SafeSetUid(0);
    NFS::MountTmpfs(config->Path, config->UserId, config->Size);
}

void UmountAsRoot(TUmountConfigPtr config)
{
    SafeSetUid(0);
    NFS::Umount(config->Path, config->Detach);
}

TError StatusToError(int status)
{
    if (WIFEXITED(status) && (WEXITSTATUS(status) == 0)) {
        return TError();
    } else if (WIFSIGNALED(status)) {
        int signalNumber = WTERMSIG(status);
        return TError(
            EProcessErrorCode::Signal,
            "Process terminated by signal %v",
            signalNumber)
            << TErrorAttribute("signal", signalNumber);
    } else if (WIFSTOPPED(status)) {
        int signalNumber = WSTOPSIG(status);
        return TError(
            EProcessErrorCode::Signal,
            "Process stopped by signal %v",
            signalNumber)
            << TErrorAttribute("signal", signalNumber);
    } else if (WIFEXITED(status)) {
        int exitCode = WEXITSTATUS(status);
        return TError(
            EProcessErrorCode::NonZeroExitCode,
            "Process exited with code %v",
            exitCode)
            << TErrorAttribute("exit_code", exitCode);
    } else {
        return TError("Unknown status %v", status);
    }
}

TError ProcessInfoToError(const siginfo_t& processInfo)
{
    switch (processInfo.si_code) {
        case CLD_EXITED: {
            auto exitCode = processInfo.si_status;
            if (exitCode == 0) {
                return TError();
            } else {
                return TError(
                    EProcessErrorCode::NonZeroExitCode,
                    "Process exited with code %v",
                    exitCode)
                    << TErrorAttribute("exit_code", exitCode);
            }
        }

        case CLD_KILLED:
        case CLD_DUMPED: {
            int signal = processInfo.si_status;
            return TError(
                EProcessErrorCode::Signal,
                "Process terminated by signal %v",
                signal)
                << TErrorAttribute("signal", signal)
                << TErrorAttribute("core_dumped", processInfo.si_code == CLD_DUMPED);
        }

        default:
            return TError("Unknown signal code %v",
                processInfo.si_code);
    }
}

bool TryExecve(const char* path, const char* const* argv, const char* const* env)
{
    ::execve(
        path,
        const_cast<char* const*>(argv),
        const_cast<char* const*>(env));
    // If we are still here, it's an error.
    return false;
}

bool TryDup2(int oldFD, int newFD)
{
    while (true) {
        auto res = HandleEintr(::dup2, oldFD, newFD);

        if (res != -1) {
            return true;
        }

        if (errno == EBUSY) {
            continue;
        }

        return false;
    }
}

bool TryClose(int fd, bool ignoreBadFD)
{
    while (true) {
        auto res = ::close(fd);
        if (res != -1) {
            return true;
        }

        switch (errno) {
            // Please read
            // http://lkml.indiana.edu/hypermail/linux/kernel/0509.1/0877.html and
            // http://rb.yandex-team.ru/arc/r/44030/
            // before editing.
            case EINTR:
                return true;
            case EBADF:
                return ignoreBadFD;
            default:
                return false;
        }
    }
}

void SafeClose(int fd, bool ignoreBadFD)
{
    if (!TryClose(fd, ignoreBadFD)) {
        THROW_ERROR TError::FromSystem();
    }
}

void SafeDup2(int oldFD, int newFD)
{
    if (!TryDup2(oldFD, newFD)) {
        THROW_ERROR_EXCEPTION("dup2 failed")
            << TErrorAttribute("old_fd", oldFD)
            << TErrorAttribute("new_fd", newFD)
            << TError::FromSystem();
    }
}

void SafeSetCloexec(int fd)
{
    int getResult = ::fcntl(fd, F_GETFD);
    if (getResult == -1) {
        THROW_ERROR_EXCEPTION("Error creating pipe: fcntl failed to get descriptor flags")
            << TError::FromSystem();
    }

    int setResult = ::fcntl(fd, F_SETFD, getResult | FD_CLOEXEC);
    if (setResult == -1) {
        THROW_ERROR_EXCEPTION("Error creating pipe: fcntl failed to set descriptor flags")
            << TError::FromSystem();
    }
}

void SetPermissions(const Stroka& path, int permissions)
{
#ifdef _linux_
    auto res = HandleEintr(::chmod, ~path, permissions);

    if (res == -1) {
        THROW_ERROR_EXCEPTION("Failed to set permissions for descriptor")
            << TErrorAttribute("path", path)
            << TErrorAttribute("permissions", permissions)
            << TError::FromSystem();
    }
#endif
}

void SetPermissions(int fd, int permissions)
{
    const auto& procPath = Format("/proc/self/fd/%v", fd);
    SetPermissions(procPath, permissions);
}

void SetUid(int uid)
{
    // Set unprivileged uid and gid for user process.
    if (setuid(0) != 0) {
        THROW_ERROR_EXCEPTION("Unable to set zero uid")
            << TError::FromSystem();
    }
    if (setgroups(0, nullptr) != 0) {
        THROW_ERROR_EXCEPTION("Unable to set zero gid")
            << TError::FromSystem();
    }
#ifdef _linux_
    if (setresgid(uid, uid, uid) != 0) {
        THROW_ERROR_EXCEPTION("Unable to set uids")
            << TErrorAttribute("uid", uid)
            << TError::FromSystem();
    }
    if (setresuid(uid, uid, uid) != 0) {
        THROW_ERROR_EXCEPTION("Unable to set gids")
            << TErrorAttribute("gid", uid)
            << TError::FromSystem();
    }
#else
    if (setuid(uid) != 0) {
        THROW_ERROR_EXCEPTION("Unable to set uid")
            << TErrorAttribute("uid", uid)
            << TError::FromSystem();
    }

    if (setgid(uid) != 0) {
        THROW_ERROR_EXCEPTION("Unable to set gid")
            << TErrorAttribute("gid", uid)
            << TError::FromSystem();
    }
#endif
}

void SafePipe(int fd[2])
{
#ifdef _linux_
    auto result = ::pipe2(fd, O_CLOEXEC);
    if (result == -1) {
        THROW_ERROR_EXCEPTION("Error creating pipe")
            << TError::FromSystem();
    }
#else
    {
        int result = ::pipe(fd);
        if (result == -1) {
            THROW_ERROR_EXCEPTION("Error creating pipe: pipe creation failed")
                << TError::FromSystem();
        }
    }
    SafeSetCloexec(fd[0]);
    SafeSetCloexec(fd[1]);
#endif
}

int SafeDup(int fd)
{
    auto result = ::dup(fd);
    if (result == -1) {
        THROW_ERROR_EXCEPTION("Error duplicating fd")
            << TError::FromSystem();
    }
    return result;
}

void SafeOpenPty(int* masterFD, int* slaveFD, int height, int width)
{
#ifndef YT_IN_ARCADIA
    {
        struct termios tt = { };
        tt.c_iflag = TTYDEF_IFLAG & ~ISTRIP;
        tt.c_oflag = TTYDEF_OFLAG;
        tt.c_lflag = TTYDEF_LFLAG;
        tt.c_cflag = (TTYDEF_CFLAG & ~(CS7|PARENB|HUPCL)) | CS8;
        tt.c_cc[VERASE] = '\x7F';
        cfsetispeed(&tt, B38400);
        cfsetospeed(&tt, B38400);

        struct winsize ws = { };
        struct winsize* wsPtr = nullptr;
        if (height > 0 && width > 0) {
            ws.ws_row = height;
            ws.ws_col = width;
            wsPtr = &ws;
        }

        int result = ::openpty(masterFD, slaveFD, nullptr, &tt, wsPtr);
        if (result == -1) {
            THROW_ERROR_EXCEPTION("Error creating pty: pty creation failed")
                << TError::FromSystem();
        }
    }
    SafeSetCloexec(*masterFD);
#else
    THROW_ERROR_EXCEPTION("Unsupported");
#endif
}

void SafeLoginTty(int slaveFD)
{
#ifndef YT_IN_ARCADIA
    int result = ::login_tty(slaveFD);
    if (result == -1) {
        THROW_ERROR_EXCEPTION("Error attaching pty to standard streams")
            << TError::FromSystem();
    }
#else
    THROW_ERROR_EXCEPTION("Unsupported");
#endif
}

void SafeSetTtyWindowSize(int fd, int height, int width)
{
    if (height > 0 && width > 0) {
        struct winsize ws;
        int result = ::ioctl(fd, TIOCGWINSZ, &ws);
        if (result == -1) {
            THROW_ERROR_EXCEPTION("Error reading tty window size")
                << TError::FromSystem();
        }
        if (ws.ws_row != height || ws.ws_col != width) {
            ws.ws_row = height;
            ws.ws_col = width;
            result = ::ioctl(fd, TIOCSWINSZ, &ws);
            if (result == -1) {
                THROW_ERROR_EXCEPTION("Error setting tty window size")
                    << TError::FromSystem();
            }
        }
    }
}

bool TryMakeNonblocking(int fd)
{
    auto res = fcntl(fd, F_GETFL);

    if (res == -1) {
        return false;
    }

    res = fcntl(fd, F_SETFL, res | O_NONBLOCK);

    if (res == -1) {
        return false;
    }

    return true;
}

void SafeMakeNonblocking(int fd)
{
    if (!TryMakeNonblocking(fd)) {
        THROW_ERROR_EXCEPTION("Failed to set nonblocking mode for descriptor %v", fd)
            << TError::FromSystem();
    }
}

void SafeSetUid(int uid)
{
    if (setuid(uid) != 0) {
        THROW_ERROR_EXCEPTION("setuid failed to set uid to %v", uid)
            << TError::FromSystem();
    }
}

Stroka SafeGetUsernameByUid(int uid)
{
    int bufferSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    if (bufferSize < 0) {
        THROW_ERROR_EXCEPTION("Failed to get username, sysconf(_SC_GETPW_R_SIZE_MAX) failed")
            << TError::FromSystem();
    }
    char buffer[bufferSize];
    struct passwd pwd, *pwdptr = nullptr;
    int result = getpwuid_r(uid, &pwd, buffer, bufferSize, &pwdptr);
    if (result != 0 || pwdptr == nullptr) {
        // Return #uid in case of absent uid in the system.
        return "#" + ToString(uid);
    }
    return pwdptr->pw_name;
}

void KillAllByUid(int uid)
{
    SafeSetUid(0);

    auto pidsToKill = GetPidsByUid(uid);
    if (pidsToKill.empty()) {
        return;
    }

    while (true) {
        for (int pid : pidsToKill) {
            auto result = kill(pid, 9);
            if (result == -1) {
                YCHECK(errno == ESRCH);
            }
        }

        pidsToKill = GetPidsByUid(uid);
        if (pidsToKill.empty())
            break;

        ThreadYield();
    }
}

#else

bool TryClose(int /* fd */, bool /* ignoreBadFD */)
{
    Y_UNIMPLEMENTED();
}

void SafeClose(int /* fd */, bool /* ignoreBadFD */)
{
    Y_UNIMPLEMENTED();
}

bool TryDup2(int /* oldFD */, int /* newFD */)
{
    Y_UNIMPLEMENTED();
}

void SafeDup2(int /* oldFD */, int /* newFD */)
{
    Y_UNIMPLEMENTED();
}

void SafeSetCloexec(int /* fd */)
{
    Y_UNIMPLEMENTED();
}

bool TryExecve(const char /* *path */, const char* /* argv[] */, const char* /* env[] */)
{
    Y_UNIMPLEMENTED();
}

TError StatusToError(int /* status */)
{
    Y_UNIMPLEMENTED();
}

void RemoveDirAsRoot(const Stroka& /* path */)
{
    Y_UNIMPLEMENTED();
}

void RemoveDirContentAsRoot(const Stroka& /* path */)
{
    YUNIMPLEMENTED();
}

void KillAllByUid(int /* uid */)
{
    YUNIMPLEMENTED();
}

void MountTmpfsAsRoot(TMountTmpfsConfigPtr /* config */)
{
    Y_UNIMPLEMENTED();
}

void KillAllByUid(int /* uid */)
{
    Y_UNIMPLEMENTED();
}

void UmountAsRoot(const Stroka& /* path */)
{
    Y_UNIMPLEMENTED();
}

void CloseAllDescriptors()
{
    Y_UNIMPLEMENTED();
}

void SetPermissions(int /* fd */, int /* permissions */)
{
    Y_UNIMPLEMENTED();
}

void SafePipe(int /* fd */ [2])
{
    Y_UNIMPLEMENTED();
}

int SafeDup(int /* fd */)
{
    Y_UNIMPLEMENTED();
}

void SafeOpenPty(int* /* masterFD */, int* /* slaveFD */, int /* height */, int /* width */)
{
    Y_UNIMPLEMENTED();
}

void SafeLoginTty(int /* slaveFD */)
{
    Y_UNIMPLEMENTED();
}

void SafeSetTtyWindowSize(int /* slaveFD */, int /* height */, int /* width */)
{
    Y_UNIMPLEMENTED();
}

bool TryMakeNonblocking(int /* fd */)
{
    Y_UNIMPLEMENTED();
}

void SafeMakeNonblocking(int /* fd */)
{
    Y_UNIMPLEMENTED();
}

void SafeSetUid(int /* uid */)
{
    Y_UNIMPLEMENTED();
}

Stroka SafeGetUsernameByUid(int /* uid */)
{
    Y_UNIMPLEMENTED();
}
#endif

void CloseAllDescriptors(const std::vector<int>& exceptFor)
{
#ifdef _linux_
    auto* dirStream = ::opendir("/proc/self/fd");
    YCHECK(dirStream != nullptr);

    int dirFD = ::dirfd(dirStream);
    YCHECK(dirFD >= 0);

    std::vector<int> fds;
    dirent* ep;
    while ((ep = ::readdir(dirStream)) != nullptr) {
        char* begin = ep->d_name;
        char* end = nullptr;
        int fd = static_cast<int>(strtol(begin, &end, 10));
        if (begin == end ||
            fd == dirFD ||
            (std::find(exceptFor.begin(), exceptFor.end(), fd) != exceptFor.end()))
        {
            continue;
        }
        fds.push_back(fd);
    }

    YCHECK(::closedir(dirStream) == 0);

    bool ignoreBadFD = true;
    for (int fd : fds) {
        YCHECK(TryClose(fd, ignoreBadFD));
    }
#endif
}

void CreateStderrFile(Stroka fileName)
{
#ifdef _unix_
    YCHECK(freopen(~fileName, "a", stderr) != nullptr);
#endif
}

bool HasRootPermissions()
{
#ifdef _unix_
    uid_t ruid, euid, suid;
#ifdef _linux_
    YCHECK(getresuid(&ruid, &euid, &suid) == 0);
#else
    ruid = getuid();
    euid = geteuid();
    setuid(0);
    suid = getuid();
    YCHECK(seteuid(euid) == 0);
    YCHECK(setruid(ruid) == 0);
#endif
    return suid == 0;
#else // not _unix_
    return false;
#endif
}

////////////////////////////////////////////////////////////////////////////////

void TKillAllByUidTool::operator()(int uid) const
{
    KillAllByUid(uid);
}

////////////////////////////////////////////////////////////////////////////////

void TRemoveDirAsRootTool::operator()(const Stroka& arg) const
{
    RemoveDirAsRoot(arg);
}

////////////////////////////////////////////////////////////////////////////////

void TRemoveDirContentAsRootTool::operator()(const Stroka& arg) const
{
    RemoveDirContentAsRoot(arg);
}

////////////////////////////////////////////////////////////////////////////////

void TMountTmpfsAsRootTool::operator()(TMountTmpfsConfigPtr arg) const
{
    MountTmpfsAsRoot(arg);
}

////////////////////////////////////////////////////////////////////////////////

void TUmountAsRootTool::operator()(TUmountConfigPtr arg) const
{
    UmountAsRoot(arg);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
