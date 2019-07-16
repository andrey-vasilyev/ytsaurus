#include "mutation_context.h"

#include <yt/core/concurrency/fls.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

TMutationContext::TMutationContext(
    TMutationContext* parent,
    const TMutationRequest& request)
    : Parent_(parent)
    , Version_(Parent_->GetVersion())
    , Request_(request)
    , Timestamp_(Parent_->GetTimestamp())
{ }

TMutationContext::TMutationContext(
    TVersion version,
    const TMutationRequest& request,
    TInstant timestamp,
    ui64 randomSeed)
    : Parent_(nullptr)
    , Version_(version)
    , Request_(request)
    , Timestamp_(timestamp)
    , RandomGenerator_(randomSeed)
{ }

TVersion TMutationContext::GetVersion() const
{
    return Version_;
}

const TMutationRequest& TMutationContext::Request() const
{
    return Request_;
}

TInstant TMutationContext::GetTimestamp() const
{
    return Timestamp_;
}

TRandomGenerator& TMutationContext::RandomGenerator()
{
    return Parent_ ? Parent_->RandomGenerator() : RandomGenerator_;
}

TMutationResponse& TMutationContext::Response()
{
    return Response_;
}

void TMutationContext::SetResponseKeeperSuppressed(bool value)
{
    ResponseKeeperSuppressed_ = value;
}

bool TMutationContext::GetResponseKeeperSuppressed()
{
    return ResponseKeeperSuppressed_;
}

static NConcurrency::TFls<TMutationContext*> CurrentMutationContext;

TMutationContext* TryGetCurrentMutationContext()
{
    return *CurrentMutationContext;
}

TMutationContext* GetCurrentMutationContext()
{
    auto* context = TryGetCurrentMutationContext();
    YT_ASSERT(context);
    return context;
}

bool HasMutationContext()
{
    return TryGetCurrentMutationContext() != nullptr;
}

void SetCurrentMutationContext(TMutationContext* context)
{
    *CurrentMutationContext = context;
}

////////////////////////////////////////////////////////////////////////////////

TMutationContextGuard::TMutationContextGuard(TMutationContext* context)
    : Context_(context)
    , SavedContext_(TryGetCurrentMutationContext())
{
    YT_ASSERT(Context_);
    SetCurrentMutationContext(Context_);
}

TMutationContextGuard::~TMutationContextGuard()
{
    YT_ASSERT(GetCurrentMutationContext() == Context_);
    SetCurrentMutationContext(SavedContext_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
