#pragma once

#include "public.h"

#include <yt/client/hydra/version.h>

#include <yt/core/actions/callback.h>

#include <yt/core/misc/random.h>
#include <yt/core/misc/ref.h>

namespace NYT::NHydra {

////////////////////////////////////////////////////////////////////////////////

struct TMutationRequest
{
    TMutationRequest() = default;
    TMutationRequest(const TMutationRequest& other) = default;
    TMutationRequest(TMutationRequest&& other) noexcept = default;

    TMutationRequest& operator = (const TMutationRequest& other) = default;
    TMutationRequest& operator = (TMutationRequest&& other) = default;

    TString Type;
    TSharedRef Data;
    TCallback<void(TMutationContext*)> Handler;
    bool AllowLeaderForwarding = false;
    NRpc::TMutationId MutationId;
    bool Retry = false;
};

DEFINE_ENUM(EMutationResponseOrigin,
    (Commit)
    (LeaderForwarding)
    (ResponseKeeper)
);

struct TMutationResponse
{
    TMutationResponse() = default;
    TMutationResponse(const TMutationResponse& other) = default;
    TMutationResponse(TMutationResponse&& other) = default;

    TMutationResponse& operator = (const TMutationResponse& other) = default;
    TMutationResponse& operator = (TMutationResponse&& other) = default;

    EMutationResponseOrigin Origin = EMutationResponseOrigin::Commit;
    TSharedRefArray Data;
};

////////////////////////////////////////////////////////////////////////////////

class TMutationContext
{
public:
    TMutationContext(
        TMutationContext* parent,
        const TMutationRequest& request);

    TMutationContext(
        TVersion version,
        const TMutationRequest& request,
        TInstant timestamp,
        ui64 randomSeed);

    TVersion GetVersion() const;
    const TMutationRequest& Request() const;
    TInstant GetTimestamp() const;
    TRandomGenerator& RandomGenerator();

    // XXX(babenko): replace with SetResponseData
    TMutationResponse& Response();

    void SetResponseKeeperSuppressed(bool value);
    bool GetResponseKeeperSuppressed();

private:
    TMutationContext* const Parent_;
    const TVersion Version_;
    const TMutationRequest& Request_;
    const TInstant Timestamp_;

    TMutationResponse Response_;
    TRandomGenerator RandomGenerator_;
    bool ResponseKeeperSuppressed_ = false;
};

TMutationContext* TryGetCurrentMutationContext();
TMutationContext* GetCurrentMutationContext();
bool HasMutationContext();
void SetCurrentMutationContext(TMutationContext* context);

////////////////////////////////////////////////////////////////////////////////

class TMutationContextGuard
    : public TNonCopyable
{
public:
    explicit TMutationContextGuard(TMutationContext* context);
    ~TMutationContextGuard();

private:
    TMutationContext* Context_;
    TMutationContext* SavedContext_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra

