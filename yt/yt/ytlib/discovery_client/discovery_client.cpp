#include "discovery_client.h"
#include "helpers.h"
#include "private.h"
#include "public.h"
#include "request_session.h"

#include <yt/yt/core/actions/future.h>

#include <yt/yt/core/rpc/caching_channel_factory.h>

#include <yt/yt/core/rpc/bus/channel.h>

#include <library/cpp/yt/threading/rw_spin_lock.h>

namespace NYT::NDiscoveryClient {

using namespace NYTree;
using namespace NBus;
using namespace NRpc;
using namespace NConcurrency;

using NYT::FromProto;

////////////////////////////////////////////////////////////////////////////////

class TDiscoveryClient
    : public IDiscoveryClient
{
public:
    TDiscoveryClient(
        TDiscoveryClientConfigPtr config,
        NRpc::IChannelFactoryPtr channelFactory)
        : ChannelFactory_(CreateCachingChannelFactory(std::move(channelFactory)))
        , AddressPool_(New<TServerAddressPool>(
            DiscoveryClientLogger,
            config))
        , Config_(std::move(config))
    { }

    TFuture<std::vector<TMemberInfo>> ListMembers(
        const TString& groupId,
        const TListMembersOptions& options) override
    {
        auto guard = ReaderGuard(Lock_);

        return New<TListMembersRequestSession>(
            AddressPool_,
            Config_,
            ChannelFactory_,
            Logger,
            groupId,
            options)
            ->Run();
    }

    TFuture<TGroupMeta> GetGroupMeta(const TString& groupId) override
    {
        auto guard = ReaderGuard(Lock_);

        return New<TGetGroupMetaRequestSession>(
            AddressPool_,
            Config_,
            ChannelFactory_,
            Logger,
            groupId)
            ->Run();
    }

    void Reconfigure(TDiscoveryClientConfigPtr config) override
    {
        auto guard = WriterGuard(Lock_);

        Config_ = std::move(config);
        AddressPool_->SetConfig(Config_);
    }

private:
    const NLogging::TLogger Logger;
    const NRpc::IChannelFactoryPtr ChannelFactory_;
    const TServerAddressPoolPtr AddressPool_;

    NThreading::TReaderWriterSpinLock Lock_;
    TDiscoveryClientConfigPtr Config_;
};

IDiscoveryClientPtr CreateDiscoveryClient(
    TDiscoveryClientConfigPtr config,
    NRpc::IChannelFactoryPtr channelFactory)
{
    return New<TDiscoveryClient>(
        std::move(config),
        std::move(channelFactory));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NDiscoveryClient
