#pragma once

#include "common.h"
#include "election_manager.pb.h"

#include <ytlib/rpc/service.h>
#include <ytlib/rpc/client.h>

namespace NYT {
namespace NElection {

////////////////////////////////////////////////////////////////////////////////

class TElectionManagerProxy
    : public NRpc::TProxyBase
{
public:
    typedef TIntrusivePtr<TElectionManagerProxy> TPtr;

    DECLARE_ENUM(EState,
        (Stopped)
        (Voting)
        (Leading)
        (Following)
    );

    static Stroka GetServiceName()
    {
        return "ElectionManager";
    }


    DECLARE_ENUM(EErrorCode,
        ((InvalidState)(1))
        ((InvalidLeader)(2))
        ((InvalidEpoch)(3))
    );

    TElectionManagerProxy(NRpc::IChannel* channel)
        : TProxyBase(channel, GetServiceName())
    { }

    DEFINE_RPC_PROXY_METHOD(NElection::NProto, PingFollower)
    DEFINE_RPC_PROXY_METHOD(NElection::NProto, GetStatus)

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NElection
} // namespace NYT
