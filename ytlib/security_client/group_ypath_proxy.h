#pragma once

#include <yt/ytlib/security_client/group_ypath.pb.h>

#include <yt/core/ytree/ypath_proxy.h>

namespace NYT {
namespace NSecurityClient {

////////////////////////////////////////////////////////////////////////////////

struct TGroupYPathProxy
    : public NYTree::TYPathProxy
{
    DEFINE_YPATH_PROXY(RPC_PROXY_DESC(Group));

    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, AddMember);
    DEFINE_MUTATING_YPATH_PROXY_METHOD(NProto, RemoveMember);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityClient
} // namespace NYT
