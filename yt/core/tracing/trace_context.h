#pragma once

#include "public.h"

#include <yt/core/misc/property.h>
#include <yt/core/misc/guid.h>

#include <yt/core/profiling/public.h>

#include <atomic>

namespace NYT::NTracing {

////////////////////////////////////////////////////////////////////////////////

//! TSpanContext represents span identity propagated across the network.
//!
//! See https://opentracing.io/specification/
struct TSpanContext
{
    TTraceId TraceId = InvalidTraceId;
    TSpanId SpanId = InvalidSpanId;
    bool Sampled = false;
    bool Debug = false;

    TSpanContext CreateChild();
};

void FormatValue(TStringBuilderBase* builder, TSpanContext spanContext, TStringBuf spec);
TString ToString(TSpanContext spanContext);

////////////////////////////////////////////////////////////////////////////////

struct TFollowsFrom {};

//! TTraceContext accumulates information associated with single tracing span.
class TTraceContext
    : public TIntrinsicRefCounted
{
public:
    TTraceContext(
        TSpanContext parent,
        const TString& name,
        TTraceContextPtr parentContext = nullptr);
    TTraceContext(
        TFollowsFrom,
        TSpanContext parent,
        const TString& name,
        TTraceContextPtr parentContext = nullptr);

    void Finish();

    bool IsSampled() const;
    bool IsDebug() const;

    TSpanContext GetContext() const;
    TTraceId GetTraceId() const;
    TSpanId GetSpanId() const;
    TSpanId GetParentSpanId() const;
    TSpanId GetFollowsFromSpanId() const;

    TString GetName() const;

    TInstant GetStartTime() const;
    TDuration GetDuration() const;

    using TTagList = SmallVector<std::pair<TString, TString>, 4>;
    const TTagList& GetTags() const;

    void SetName(const TString& name);
    void SetSampled();
    void AddTag(const TString& tagKey, const TString& tagValue);
    void ResetStartTime();

    TTraceContextPtr CreateChild(const TString& name);

    void IncrementElapsedCpuTime(NProfiling::TCpuDuration delta);
    NProfiling::TCpuDuration GetElapsedCpuTime() const;
    void FlushElapsedTime();
    TDuration GetElapsedTime() const;

private:
    const TSpanId ParentSpanId_ = InvalidSpanId;
    const TSpanId FollowsFromSpanId_ = InvalidSpanId;

    TSpinLock Lock_;
    NProfiling::TCpuInstant StartTime_;
    NProfiling::TCpuDuration Duration_;
    TSpanContext SpanContext_;
    TString Name_;
    TTagList Tags_;
    bool Finished_ = false;

    std::atomic<NProfiling::TCpuDuration> ElapsedCpuTime_ = {0};
    TTraceContextPtr ParentContext_;
};

DEFINE_REFCOUNTED_TYPE(TTraceContext)

////////////////////////////////////////////////////////////////////////////////

TTraceContext* GetCurrentTraceContext();
TTraceId GetCurrentTraceId();
void FlushCurrentTraceContextTime();

TTraceContextPtr CreateRootTraceContext(const TString& spanName);
TTraceContextPtr CreateChildTraceContext(const TString& spanName);

template <class T>
void AddTag(const TString& tagName, const T& tagValue);

////////////////////////////////////////////////////////////////////////////////

//! TTraceContextGuard installs trace into the current fiber implicit trace slot.
class TTraceContextGuard
{
public:
    explicit TTraceContextGuard(TTraceContextPtr traceContext);
    TTraceContextGuard(TTraceContextGuard&& other);
    ~TTraceContextGuard();

    bool IsActive() const;
    void Release();

private:
    bool Active_;
    TTraceContextPtr OldTraceContext_;
};

////////////////////////////////////////////////////////////////////////////////

class TNullTraceContextGuard
{
public:
    TNullTraceContextGuard();
    TNullTraceContextGuard(TNullTraceContextGuard&& other);
    ~TNullTraceContextGuard();

    bool IsActive() const;
    void Release();

private:
    bool Active_;
    TTraceContextPtr OldTraceContext_;
};

////////////////////////////////////////////////////////////////////////////////

class TTraceContextFinishGuard
{
public:
    explicit TTraceContextFinishGuard(TTraceContextPtr traceContext);
    ~TTraceContextFinishGuard();

    TTraceContextFinishGuard(const TTraceContextFinishGuard&) = delete;
    TTraceContextFinishGuard(TTraceContextFinishGuard&&) = default;

    TTraceContextFinishGuard& operator=(const TTraceContextFinishGuard&) = delete;
    TTraceContextFinishGuard& operator=(TTraceContextFinishGuard&&) = default;

private:
    TTraceContextPtr TraceContext_;
};

////////////////////////////////////////////////////////////////////////////////

class TChildTraceContextGuard
{
public:
    explicit TChildTraceContextGuard(const TString& spanName);
    TChildTraceContextGuard(TChildTraceContextGuard&& other) = default;

private:
    TTraceContextGuard TraceContextGuard_;
    TTraceContextFinishGuard FinishGuard_;
};

////////////////////////////////////////////////////////////////////////////////
// For internal use only.

TTraceContextPtr SwitchTraceContext(TTraceContextPtr traceContext);
void InstallTraceContext(NProfiling::TCpuInstant now, TTraceContextPtr newTraceContext);
TTraceContextPtr UninstallTraceContext(NProfiling::TCpuInstant now);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NTracing

#define TRACE_CONTEXT_INL_H_
#include "trace_context-inl.h"
#undef TRACE_CONTEXT_INL_H_
