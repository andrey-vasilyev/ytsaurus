#include "stdafx.h"
#include "mutation.h"

#include <core/concurrency/delayed_executor.h>

namespace NYT {
namespace NHydra {

////////////////////////////////////////////////////////////////////////////////

TMutation::TMutation(IHydraManagerPtr hydraManager)
    : HydraManager(std::move(hydraManager))
{ }

TFuture<TErrorOr<TMutationResponse>>  TMutation::Commit()
{
    return HydraManager->CommitMutation(Request).Apply(
        BIND(&TMutation::OnCommitted, MakeStrong(this)));
}

TMutationPtr TMutation::SetId(const TMutationId& id)
{
    Request.Id = id;
    return this;
}

TMutationPtr TMutation::SetRequestData(TSharedRef data, Stroka type)
{
    Request.Data = std::move(data);
    Request.Type = std::move(type);
    return this;
}

TMutationPtr TMutation::SetAction(TClosure action)
{
    Request.Action = std::move(action);
    return this;
}

TMutationPtr TMutation::OnSuccess(TClosure onSuccess)
{
    YASSERT(!OnSuccess_);
    OnSuccess_ = BIND([=] (const TMutationResponse&) {
        onSuccess.Run();
    });
    return this;
}

TMutationPtr TMutation::OnSuccess(TCallback<void(const TMutationResponse&)> onSuccess)
{
    YASSERT(!OnSuccess_);
    OnSuccess_ = std::move(onSuccess);
    return this;
}

TMutationPtr TMutation::OnError(TCallback<void(const TError&)> onError)
{
    YASSERT(!OnError_);
    OnError_ = std::move(onError);
    return this;
}

TErrorOr<TMutationResponse> TMutation::OnCommitted(TErrorOr<TMutationResponse> result)
{
    if (result.IsOK()) {
        if (OnSuccess_) {
            OnSuccess_.Run(result.GetValue());
        }
    } else {
        if (OnError_) {
            OnError_.Run(result);
        }
    }
    return std::move(result);
}

////////////////////////////////////////////////////////////////////////////////

TMutationPtr CreateMutation(
    IHydraManagerPtr hydraManager)
{
    return New<TMutation>(
        std::move(hydraManager));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NHydra
} // namespace NYT
