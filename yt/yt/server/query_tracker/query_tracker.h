#pragma once

#include "private.h"

#include <yt/yt/ytlib/api/native/public.h>

#include <yt/yt/client/api/client.h>

#include <yt/yt/core/ypath/public.h>

#include <yt/yt/core/misc/common.h>

#include <yt/yt/core/actions/public.h>

namespace NYT::NQueryTracker {

///////////////////////////////////////////////////////////////////////////////

struct IQueryTracker
    : public TRefCounted
{
    virtual void Start() = 0;

    virtual void Reconfigure(const TQueryTrackerDynamicConfigPtr& config) = 0;
};

DEFINE_REFCOUNTED_TYPE(IQueryTracker)

///////////////////////////////////////////////////////////////////////////////

IQueryTrackerPtr CreateQueryTracker(
    TQueryTrackerDynamicConfigPtr config,
    TString selfAddress,
    IInvokerPtr controlInvoker,
    NAlertManager::IAlertCollectorPtr alertCollector,
    NApi::NNative::IClientPtr stateClient,
    NYPath::TYPath stateRoot,
    int minRequiredStateVersion);

///////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NQueryTracker
