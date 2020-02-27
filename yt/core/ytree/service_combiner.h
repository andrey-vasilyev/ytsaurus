#pragma once

#include "ypath_detail.h"

namespace NYT::NYTree {

////////////////////////////////////////////////////////////////////////////////

class TServiceCombiner
    : public TYPathServiceBase
{
public:
    explicit TServiceCombiner(
        std::vector<IYPathServicePtr> services,
        std::optional<TDuration> keysUpdatePeriod = std::nullopt);

    void SetUpdatePeriod(TDuration period);

    virtual TResolveResult Resolve(const TYPath& path, const NRpc::IServiceContextPtr& context) override;
    virtual void Invoke(const NRpc::IServiceContextPtr& context) override;

    ~TServiceCombiner();

private:
    class TImpl;
    const TIntrusivePtr<TImpl> Impl_;
};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NYTree

