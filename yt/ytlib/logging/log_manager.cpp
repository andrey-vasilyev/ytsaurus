#include "stdafx.h"
#include "log_manager.h"
#include "writer.h"

#include <ytlib/misc/property.h>
#include <ytlib/misc/pattern_formatter.h>
#include <ytlib/misc/raw_formatter.h>
#include <ytlib/misc/periodic_invoker.h>

#include <ytlib/actions/action_queue_detail.h>

#include <ytlib/ytree/ypath_client.h>
#include <ytlib/ytree/ypath_service.h>
#include <ytlib/ytree/yson_serializable.h>

#include <ytlib/profiling/profiler.h>

#include <util/system/defaults.h>
#include <util/system/sigset.h>

#ifdef _win_
    #include <io.h>
#else
    #include <unistd.h>
#endif

namespace NYT {
namespace NLog {

using namespace NYTree;

////////////////////////////////////////////////////////////////////////////////

// TODO: review this and that
static const char* const SystemPattern = "$(datetime) $(level) $(category) $(message)";

static const char* const DefaultStdErrWriterName = "StdErr";
static const ELogLevel DefaultStdErrMinLevel= ELogLevel::Info;
static const char* const DefaultStdErrPattern = "$(datetime) $(level) $(category) $(message)";

static const char* const AllCategoriesName = "*";

static TLogger SILENT_UNUSED Logger(SystemLoggingCategory);
static NProfiling::TProfiler SILENT_UNUSED Profiler("/logging");

////////////////////////////////////////////////////////////////////////////////

#ifdef _linux_
#include <sys/inotify.h>
#endif

class TNotificationHandle
    : private TNonCopyable
{
public:
    TNotificationHandle()
        : Fd_(-1)
    {
#ifdef _linux_
        Fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        YCHECK(Fd_ > 0);
#endif
    }

    ~TNotificationHandle()
    {
#ifdef _linux_
        YCHECK(Fd_ > 0);
        ::close(Fd_);
#endif
    }

    int Poll()
    {
#ifdef _linux_
        YCHECK(Fd_ > 0);

        char buffer[sizeof(struct inotify_event) + NAME_MAX + 1];
        auto rv = ::read(Fd_, buffer, sizeof(buffer));

        if (rv < 0) {
            if (errno != EAGAIN) {
                LOG_ERROR(
                    TError::FromSystem(errno),
                    "Unable to poll inotify() descriptor %d",
                    Fd_);
            }
        } else if (rv > 0) {
            YASSERT(rv >= sizeof(struct inotify_event));
            struct inotify_event* event = (struct inotify_event*)buffer;

            if (event->mask & IN_ATTRIB) {
                LOG_DEBUG(
                    "Watch %d has triggered metadata change (IN_ATTRIB)",
                    event->wd);
            }
            if (event->mask & IN_DELETE_SELF) {
                LOG_DEBUG(
                    "Watch %d has triggered a deletion (IN_DELETE_SELF)",
                    event->wd);
            }
            if (event->mask & IN_MOVE_SELF) {
                LOG_DEBUG(
                    "Watch %d has triggered a movement (IN_MOVE_SELF)",
                    event->wd);
            }

            return event->wd;
        } else {
            // Do nothing.
        }
#endif
        return 0;
    }

    DEFINE_BYVAL_RO_PROPERTY(int, Fd);
};

class TNotificationWatch
    : private TNonCopyable
{
public:
    TNotificationWatch(
        TNotificationHandle* handle,
        const Stroka& path,
        TClosure callback)
        : Fd_(-1)
        , Wd_(-1)
        , Path(path)
        , Callback(std::move(callback))

    {
        Fd_ = handle->GetFd();
        YCHECK(Fd_ > 0);

        CreateWatch();
    }

    ~TNotificationWatch()
    {
        DropWatch();
    }

    DEFINE_BYVAL_RO_PROPERTY(int, Fd);
    DEFINE_BYVAL_RO_PROPERTY(int, Wd);

    void Run()
    {
        Callback.Run();
        // Reinitialize watch to hook to the newly created file.
        DropWatch();
        CreateWatch();
    }

private:
    void CreateWatch()
    {
        YCHECK(Wd_ <= 0);
#ifdef _linux_
        Wd_ = inotify_add_watch(
            Fd_,
            Path.c_str(),
            IN_ATTRIB | IN_DELETE_SELF | IN_MOVE_SELF);

        if (Wd_ < 0) {
            LOG_ERROR(
                TError::FromSystem(errno),
                "Unable to register watch for path %s",
                Path.Quote().c_str());
            Wd_ = -1;
        } else if (Wd_ > 0) {
            LOG_DEBUG(
                "Registered watch %d for path %s",
                Wd_,
                Path.Quote().c_str());
        } else {
            YUNREACHABLE();
        }
#else
        Wd_ = -1;
#endif
    }

    void DropWatch()
    {
#ifdef _linux_
        if (Wd_ > 0) {
            LOG_DEBUG(
                "Unregistering watch %d for path %s",
                Wd_,
                Path.Quote().c_str());
            inotify_rm_watch(Fd_, Wd_);
        }
#endif
        Wd_ = -1;
    }

private:
    Stroka Path;
    TClosure Callback;
};

struct TRule
    : public TYsonSerializable
{
    typedef TIntrusivePtr<TRule> TPtr;

    bool AllCategories;
    yhash_set<Stroka> Categories;

    ELogLevel MinLevel;
    ELogLevel MaxLevel;

    std::vector<Stroka> Writers;

    TRule()
        : AllCategories(false)
    {
        RegisterParameter("categories", Categories).NonEmpty();
        RegisterParameter("min_level", MinLevel).Default(ELogLevel::Minimum);
        RegisterParameter("max_level", MaxLevel).Default(ELogLevel::Maximum);
        RegisterParameter("writers", Writers).NonEmpty();
    }

    virtual void OnLoaded() override
    {
        if (Categories.size() == 1 && *Categories.begin() == AllCategoriesName) {
            AllCategories = true;
        }
    }

    bool IsApplicable(const Stroka& category) const
    {
        return AllCategories || Categories.find(category) != Categories.end();
    }

    bool IsApplicable(const Stroka& category, ELogLevel level) const
    {
        return
            MinLevel <= level && level <= MaxLevel &&
            IsApplicable(category);
    }
};

////////////////////////////////////////////////////////////////////////////////

typedef std::vector<ILogWriterPtr> TLogWriters;

class TLogConfig;
typedef TIntrusivePtr<TLogConfig> TLogConfigPtr;

////////////////////////////////////////////////////////////////////////////////

class TLogConfig
    : public TYsonSerializable
{
public:
    /*!
     * Needs to be public for TYsonSerializable.
     * Not for public use.
     * Use #CreateDefault instead.
     */
    TLogConfig()
        : Version(0)
    {
        RegisterParameter("flush_period", FlushPeriod)
            .Default(TDuration::Zero());
        RegisterParameter("watch_period", WatchPeriod)
            .Default(TDuration::Seconds(1));
        RegisterParameter("writers", WriterConfigs);
        RegisterParameter("rules", Rules);

        RegisterValidator([&] () {
            FOREACH (const auto& rule, Rules) {
                FOREACH (const Stroka& writer, rule->Writers) {
                    if (WriterConfigs.find(writer) == WriterConfigs.end()) {
                        THROW_ERROR_EXCEPTION("Unknown writer: %s", ~writer.Quote());
                    }
                }
            }
        });
    }

    TLogWriters GetWriters(const TLogEvent& event)
    {
        // Place a return value on top to promote RVO.
        TLogWriters writers;
        std::pair<Stroka, ELogLevel> cacheKey(event.Category, event.Level);
        auto it = CachedWriters.find(cacheKey);
        if (it != CachedWriters.end())
            return it->second;

        yhash_set<Stroka> writerIds;
        FOREACH (auto& rule, Rules) {
            if (rule->IsApplicable(event.Category, event.Level)) {
                writerIds.insert(rule->Writers.begin(), rule->Writers.end());
            }
        }

        FOREACH (const Stroka& writerId, writerIds) {
            auto writerIt = Writers.find(writerId);
            YASSERT(writerIt != Writers.end());
            writers.push_back(writerIt->second);
        }

        YCHECK(CachedWriters.insert(std::make_pair(cacheKey, writers)).second);
        return writers;
    }

    ELogLevel GetMinLevel(const Stroka& category) const
    {
        ELogLevel level = ELogLevel::Maximum;
        FOREACH (const auto& rule, Rules) {
            if (rule->IsApplicable(category)) {
                level = Min(level, rule->MinLevel);
            }
        }
        return level;
    }

    void FlushWriters()
    {
        FOREACH (auto& pair, Writers) {
            pair.second->Flush();
        }
    }

    void WatchWriters()
    {
        int previousWd = -1, currentWd = -1;
        while ((currentWd = NotificationHandle.Poll()) > 0) {
            if (currentWd == previousWd) {
                continue;
            }
            auto&& it = NotificationWatchesIndex.find(currentWd);
            auto&& jt = NotificationWatchesIndex.end();
            YCHECK(it != jt);

            auto* watch = it->second;
            watch->Run();

            if (watch->GetWd() != currentWd) {
                NotificationWatchesIndex.erase(it);
                YCHECK(NotificationWatchesIndex.insert(
                    std::make_pair(watch->GetWd(), watch)).second);
            }

            previousWd = currentWd;
        }
    }

    void ReloadWriters()
    {
        AtomicIncrement(Version);
        FOREACH (auto& pair, Writers) {
            pair.second->Reload();
        }
    }

    static TLogConfigPtr CreateDefault()
    {
        auto config = New<TLogConfig>();

        config->Writers.insert(std::make_pair(
            DefaultStdErrWriterName,
            New<TStdErrLogWriter>(DefaultStdErrPattern)));

        auto rule = New<TRule>();

        rule->AllCategories = true;
        rule->MinLevel = DefaultStdErrMinLevel;
        rule->Writers.push_back(DefaultStdErrWriterName);

        config->Rules.push_back(rule);

        return config;
    }

    static TLogConfigPtr CreateFromNode(INodePtr node, const TYPath& path = "")
    {
        auto config = New<TLogConfig>();
        config->Load(node, true, true, path);
        config->CreateWriters();
        return config;
    }

    int GetVersion() const
    {
        return Version;
    }

    TDuration GetFlushPeriod() const
    {
        return FlushPeriod;
    }

    TDuration GetWatchPeriod() const
    {
        return WatchPeriod;
    }

private:
    void CreateWriters()
    {
        FOREACH (const auto& pair, WriterConfigs) {
            const auto& name = pair.first;
            const auto& config = pair.second;
            const auto& pattern = config->Pattern;

            ILogWriterPtr writer = nullptr;
            std::unique_ptr<TNotificationWatch> notify = nullptr;

            switch (config->Type) {
                case ILogWriter::EType::StdOut:
                    writer = New<TStdOutLogWriter>(pattern);
                    break;
                case ILogWriter::EType::StdErr:
                    writer = New<TStdErrLogWriter>(pattern);
                    break;
                case ILogWriter::EType::File:
                    writer = New<TFileLogWriter>(config->FileName, pattern);
                    notify = std::unique_ptr<TNotificationWatch>(
                        new TNotificationWatch(
                            &NotificationHandle,
                            config->FileName.c_str(),
                            BIND(&ILogWriter::Reload, writer)));
                    break;
                case ILogWriter::EType::Raw:
                    writer = New<TRawFileLogWriter>(config->FileName);
                    notify = std::unique_ptr<TNotificationWatch>(
                        new TNotificationWatch(
                            &NotificationHandle,
                            config->FileName.c_str(),
                            BIND(&ILogWriter::Reload, writer)));
                    break;
                default:
                    YUNREACHABLE();
            }

            if (writer) {
                YCHECK(Writers.insert(
                    std::make_pair(name, std::move(writer))).second);
            }

            if (notify) {
                YCHECK(NotificationWatchesIndex.insert(
                    std::make_pair(notify->GetWd(), notify.get())).second);
                NotificationWatches.emplace_back(std::move(notify));
            }

            AtomicIncrement(Version);
        }
    }

    TAtomic Version;

    TDuration FlushPeriod;
    TDuration WatchPeriod;

    std::vector<TRule::TPtr> Rules;
    yhash_map<Stroka, ILogWriter::TConfig::TPtr> WriterConfigs;

    yhash_map<Stroka, ILogWriterPtr> Writers;
    ymap<std::pair<Stroka, ELogLevel>, TLogWriters> CachedWriters;

    TNotificationHandle NotificationHandle;
    std::vector<std::unique_ptr<TNotificationWatch>> NotificationWatches;
    std::map<int, TNotificationWatch*> NotificationWatchesIndex;
};

////////////////////////////////////////////////////////////////////////////////

namespace {

void ReloadSignalHandler(int signal)
{
    NLog::TLogManager::Get()->Reopen();
}

} // namespace

class TLogManager::TImpl
    : public TActionQueueBase
{
public:
    TImpl()
        : TActionQueueBase("Logging", false)
        , QueueInvoker(New<TQueueInvoker>("", this, false))
        // Version forces this very module's Logger object to update to our own
        // default configuration (default level etc.).
        , Version(-1)
        , EnqueueCounter("/enqueue_rate")
        , WriteCounter("/write_rate")
        , ReopenEnqueued(false)
    {
        SystemWriters.push_back(New<TStdErrLogWriter>(SystemPattern));
        DoUpdateConfig(TLogConfig::CreateDefault());
        Start();
    }

    ~TImpl()
    {
        Shutdown();
    }

    void Configure(INodePtr node, const TYPath& path = "")
    {
        if (IsRunning()) {
            auto config = TLogConfig::CreateFromNode(node, path);
            ConfigsToUpdate.Enqueue(config);
            Signal();
        }
    }

    void Configure(const Stroka& fileName, const TYPath& path)
    {
        try {
            TIFStream configStream(fileName);
            auto root = ConvertToNode(&configStream);
            auto configNode = GetNodeByYPath(root, path);
            Configure(configNode, path);
        } catch (const std::exception& ex) {
            LOG_ERROR(ex, "Error while configuring logging");
        }
    }

    void Shutdown()
    {
        TActionQueueBase::Shutdown();
        Config->FlushWriters();
    }

    virtual void OnThreadStart()
    {
#ifdef _unix_
        // Set mask.
        sigset_t ss;
        sigemptyset(&ss);
        sigaddset(&ss, SIGHUP);
        sigprocmask(SIG_UNBLOCK, &ss, NULL);

        // Set handler.
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = &ReloadSignalHandler;

        YCHECK(sigaction(SIGHUP, &sa, NULL) == 0);
#endif
    }

    /*!
     * In some cases (when configuration is being updated at the same time),
     * the actual version is greater than the version returned by this method.
     */
    int GetConfigVersion()
    {
        return Version;
    }

    int GetConfigRevision()
    {
        return Config->GetVersion();
    }

    void GetLoggerConfig(
        const Stroka& category,
        ELogLevel* minLevel,
        int* configVersion)
    {
        TGuard<TSpinLock> guard(&SpinLock);
        *minLevel = Config->GetMinLevel(category);
        *configVersion = Version;
    }

    void Enqueue(const TLogEvent& event)
    {
        if (!IsRunning()) {
            return;
        }

        Profiler.Increment(EnqueueCounter);
        LogEventQueue.Enqueue(event);
        Signal();

        if (event.Level == ELogLevel::Fatal) {
            // Flush everything and die.
            Shutdown();

            // Last-minute information.
            TRawFormatter<1024> formatter;
            formatter.AppendString("*** Fatal error encountered in ");
            formatter.AppendString(event.Function);
            formatter.AppendString(" (");
            formatter.AppendString(event.FileName);
            formatter.AppendString(":");
            formatter.AppendNumber(event.Line);
            formatter.AppendString(") ***\n");
            formatter.AppendString(event.Message.c_str());
            formatter.AppendString("\n*** Aborting ***\n");

            auto unused = ::write(2, formatter.GetData(), formatter.GetBytesWritten());
            (void)unused;

            std::terminate();
        }
    }

    virtual bool DequeueAndExecute() override
    {
        auto actionsExecuted = QueueInvoker->DequeueAndExecute();

        bool configsUpdated = false;
        TLogConfigPtr config;
        while (ConfigsToUpdate.Dequeue(&config)) {
            DoUpdateConfig(config);
            configsUpdated = true;
        }

        bool eventsWritten = false;
        TLogEvent event;
        while (LogEventQueue.Dequeue(&event)) {
            // To avoid starvation of config update
            while (ConfigsToUpdate.Dequeue(&config)) {
                DoUpdateConfig(config);
            }

            if (ReopenEnqueued) {
                ReopenEnqueued = false;
                Config->ReloadWriters();
            }

            Write(event);
            eventsWritten = true;
        }

        if (eventsWritten && Config->GetFlushPeriod() == TDuration::Zero()) {
            Config->FlushWriters();
        }

        return actionsExecuted || configsUpdated || eventsWritten;
    }

    void Reopen()
    {
        ReopenEnqueued = true;
    }

private:
    typedef std::vector<ILogWriterPtr> TWriters;

    TWriters GetWriters(const TLogEvent& event)
    {
        if (event.Category == SystemLoggingCategory) {
            return SystemWriters;
        } else {
            return Config->GetWriters(event);
        }
    }

    void Write(const TLogEvent& event)
    {
        FOREACH (auto& writer, GetWriters(event)) {
            Profiler.Increment(WriteCounter);
            writer->Write(event);
        }
    }

    void DoUpdateConfig(TLogConfigPtr config)
    {
        if (Config) {
            Config->FlushWriters();
        }

        {
            TGuard<TSpinLock> guard(&SpinLock);

            Config = config;
            AtomicIncrement(Version);

            if (FlushInvoker) {
                FlushInvoker->Stop();
                FlushInvoker.Reset();
            }

            if (WatchInvoker) {
                WatchInvoker->Stop();
                WatchInvoker.Reset();
            }

            if (Config->GetFlushPeriod() != TDuration::Zero()) {
                FlushInvoker = New<TPeriodicInvoker>(
                    QueueInvoker,
                    BIND(&TImpl::DoFlushWritersPeriodically, MakeStrong(this)),
                    Config->GetFlushPeriod());
                FlushInvoker->Start();
            }

            if (Config->GetWatchPeriod() != TDuration::Zero()) {
                WatchInvoker = New<TPeriodicInvoker>(
                    QueueInvoker,
                    BIND(&TImpl::DoWatchWritersPeriodically, MakeStrong(this)),
                    Config->GetWatchPeriod());
                WatchInvoker->Start();
            }
        }
    }

    void DoFlushWritersPeriodically()
    {
        Config->FlushWriters();
    }

    void DoWatchWritersPeriodically()
    {
        Config->WatchWriters();
    }

    TQueueInvokerPtr QueueInvoker;

    // Configuration.
    TAtomic Version;

    TLogConfigPtr Config;
    NProfiling::TRateCounter EnqueueCounter;
    NProfiling::TRateCounter WriteCounter;
    TSpinLock SpinLock;

    TLockFreeQueue<TLogConfigPtr> ConfigsToUpdate;
    TLockFreeQueue<TLogEvent> LogEventQueue;

    TWriters SystemWriters;

    volatile bool ReopenEnqueued;

    TPeriodicInvokerPtr FlushInvoker;
    TPeriodicInvokerPtr WatchInvoker;
};

////////////////////////////////////////////////////////////////////////////////

TLogManager::TLogManager()
    : Impl(new TImpl())
{ }

TLogManager* TLogManager::Get()
{
    return Singleton<TLogManager>();
}

void TLogManager::Configure(INodePtr node)
{
    Impl->Configure(node);
}

void TLogManager::Configure(const Stroka& fileName, const TYPath& path)
{
    Impl->Configure(fileName, path);
}

void TLogManager::Shutdown()
{
    Impl->Shutdown();
}

int TLogManager::GetConfigVersion()
{
    return Impl->GetConfigVersion();
}

int TLogManager::GetConfigRevision()
{
    return Impl->GetConfigRevision();
}

void TLogManager::GetLoggerConfig(
    const Stroka& category,
    ELogLevel* minLevel,
    int* configVersion)
{
    Impl->GetLoggerConfig(category, minLevel, configVersion);
}

void TLogManager::Enqueue(const TLogEvent& event)
{
    Impl->Enqueue(event);
}

void TLogManager::Reopen()
{
    Impl->Reopen();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NLog
} // namespace NYT
