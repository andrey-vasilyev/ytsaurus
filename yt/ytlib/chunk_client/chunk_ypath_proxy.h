#pragma once

#include "public.h"

#include <ytlib/chunk_client/chunk_ypath.pb.h>
#include <ytlib/chunk_client/chunk_owner_ypath.pb.h>

#include <ytlib/object_client/object_ypath_proxy.h>

namespace NYT {
namespace NChunkClient {

////////////////////////////////////////////////////////////////////////////////

struct TChunkYPathProxy
    : public NObjectClient::TObjectYPathProxy
{

    DEFINE_YPATH_PROXY_METHOD(NProto, Confirm);

    // NB: works only for table chunks.
    DEFINE_YPATH_PROXY_METHOD(NChunkClient::NProto, Fetch);
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NChunkClient
} // namespace NYT
