#pragma once

#include "public.h"

#include <yt/core/actions/signal.h>

namespace NYT::NCellMaster {

////////////////////////////////////////////////////////////////////////////////

class TConfigManager
    : public TRefCounted
{
public:
    explicit TConfigManager(NCellMaster::TBootstrap* bootstrap);
    ~TConfigManager();

    void Initialize();

    const TDynamicClusterConfigPtr& GetConfig() const;
    void SetConfig(TDynamicClusterConfigPtr config);

    DECLARE_SIGNAL(void(TDynamicClusterConfigPtr), ConfigChanged);

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TConfigManager)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCellMaster
