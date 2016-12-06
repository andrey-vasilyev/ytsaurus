#include "channel.h"

namespace NYT {
namespace NNodeTrackerClient {

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

class TNodeChannelFactory
    : public INodeChannelFactory
{
public:
    TNodeChannelFactory(IChannelFactoryPtr channelFactory, const TNetworkPreferenceList& networks)
        : ChannelFactory_(channelFactory)
        , Networks_(networks)
    { }

    virtual IChannelPtr CreateChannel(const TNodeDescriptor& descriptor) override
    {
        return CreateChannel(descriptor.Addresses());
    }

    virtual IChannelPtr CreateChannel(const TAddressMap& addresses) override
    {
        const auto& address = GetAddress(addresses, Networks_);
        return CreateChannel(address);
    }

    virtual IChannelPtr CreateChannel(const Stroka& address) override
    {
        return ChannelFactory_->CreateChannel(address);
    }

private:
    const IChannelFactoryPtr ChannelFactory_;
    const TNetworkPreferenceList Networks_;
};

INodeChannelFactoryPtr CreateNodeChannelFactory(
    IChannelFactoryPtr channelFactory,
    const TNetworkPreferenceList& networks)
{
    return New<TNodeChannelFactory>(channelFactory, networks);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NNodeTrackerClient
} // namespace NYT
