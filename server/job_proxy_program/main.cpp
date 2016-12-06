#include <yt/server/program/program.h>
#include <yt/server/program/program_cgroup_mixin.h>
#include <yt/server/program/program_config_mixin.h>
#include <yt/server/program/program_tool_mixin.h>

#include <yt/server/job_proxy/config.h>
#include <yt/server/job_proxy/job_proxy.h>

#include <yt/server/misc/configure_singletons.h>

#include <yt/core/misc/proc.h>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

class TJobProxyProgram
    : public TProgram
    , public TProgramToolMixin
    , public TProgramConfigMixin<NJobProxy::TJobProxyConfig>
    , public TProgramCgroupMixin
{
public:
    TJobProxyProgram()
        : TProgram()
        , TProgramToolMixin(Opts_)
        , TProgramConfigMixin(Opts_, false)
        , TProgramCgroupMixin(Opts_)
    {
        // TODO(sandello): IDs here are optional due to tool mixin.
        // One should extract tools into a separate binary!
        Opts_
            .AddLongOption("operation-id", "operation id")
            .StoreMappedResultT<Stroka>(&OperationId_, &CheckGuidArgMapper)
            .RequiredArgument("ID")
            .Optional();
        Opts_
            .AddLongOption("job-id", "job id")
            .StoreMappedResultT<Stroka>(&JobId_, &CheckGuidArgMapper)
            .RequiredArgument("ID")
            .Optional();
    }

protected:
    virtual void DoRun(const NLastGetopt::TOptsParseResult& parseResult) override
    {
        TThread::CurrentThreadSetName("JobProxyMain");

        ConfigureUids();
        ConfigureSignals();
        ConfigureCrashHandler();

        if (HandleToolOptions()) {
            return;
        }

        CloseAllDescriptors();
        CreateStderrFile("stderr");

        if (HandleConfigOptions()) {
            return;
        }

        auto config = GetConfig();

        ConfigureServerSingletons(config);

        if (HandleCgroupOptions()) {
            return;
        }

        // NB: There are some cyclic references here:
        // JobProxy <-> Job
        // JobProxy <-> JobProberService
        // But we (currently) don't care.
        auto jobProxy = New<NJobProxy::TJobProxy>(std::move(config), OperationId_, JobId_);
        jobProxy->Run();
    }

private:
    NJobTrackerClient::TOperationId OperationId_;
    NJobTrackerClient::TJobId JobId_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT

int main(int argc, const char** argv)
{
    return NYT::TJobProxyProgram().Run(argc, argv);
}

