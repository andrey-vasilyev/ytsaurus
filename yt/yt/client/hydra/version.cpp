#include "version.h"

#include <yt/yt/core/misc/format.h>

namespace NYT::NHydra {

TReachableState::TReachableState(int segmentId, i64 sequenceNumber) noexcept
    : SegmentId(segmentId)
    , SequenceNumber(sequenceNumber)
{ }

std::strong_ordering TReachableState::operator <=> (const TReachableState& other) const
{
    if (SegmentId != other.SegmentId) {
        return SegmentId <=> other.SegmentId;
    }
    return SequenceNumber <=> other.SequenceNumber;
}

void FormatValue(TStringBuilderBase* builder, TReachableState state, TStringBuf /* spec */)
{
    builder->AppendFormat("%v:%v", state.SegmentId, state.SequenceNumber);
}

TString ToString(TReachableState state)
{
    return ToStringViaBuilder(state);
}

////////////////////////////////////////////////////////////////////////////////

TElectionPriority::TElectionPriority(int term, int lastMutationTerm, int segmentId, i64 sequenceNumber) noexcept
    : Term(term)
    , LastMutationTerm(lastMutationTerm)
    , ReachableState(segmentId, sequenceNumber)
{ }

TElectionPriority::TElectionPriority(int term, int lastMutationTerm, TReachableState reachableState) noexcept
    : Term(term)
    , LastMutationTerm(lastMutationTerm)
    , ReachableState(reachableState)
{ }

std::strong_ordering TElectionPriority::operator <=> (const TElectionPriority& other) const
{
    if (LastMutationTerm != other.LastMutationTerm) {
        return LastMutationTerm <=> other.LastMutationTerm;
    }
    return ReachableState.SequenceNumber <=> other.ReachableState.SequenceNumber;
}

void FormatValue(TStringBuilderBase* builder, TElectionPriority priority, TStringBuf /* spec */)
{
    builder->AppendFormat("{Term: %v, MutationTerm: %v, State: %v}",
        priority.Term,
        priority.LastMutationTerm,
        priority.ReachableState);
}

TString ToString(TElectionPriority state)
{
    return ToStringViaBuilder(state);
}

////////////////////////////////////////////////////////////////////////////////

TVersion::TVersion(int segmentId, int recordId) noexcept
    : SegmentId(segmentId)
    , RecordId(recordId)
{ }

std::strong_ordering TVersion::operator <=> (const TVersion& other) const
{
    if (SegmentId != other.SegmentId) {
        return SegmentId <=> other.SegmentId;
    }
    return RecordId <=> other.RecordId;
}

TRevision TVersion::ToRevision() const
{
    return (static_cast<TRevision>(SegmentId) << 32) | static_cast<TRevision>(RecordId);
}

TVersion TVersion::FromRevision(TRevision revision)
{
    return TVersion(revision >> 32, revision & 0xffffffff);
}

TVersion TVersion::Advance(int delta) const
{
    YT_ASSERT(delta >= 0);
    return TVersion(SegmentId, RecordId + delta);
}

TVersion TVersion::Rotate() const
{
    return TVersion(SegmentId + 1, 0);
}

void FormatValue(TStringBuilderBase* builder, TVersion version, TStringBuf /* spec */)
{
    builder->AppendFormat("%v:%v", version.SegmentId, version.RecordId);
}

TString ToString(TVersion version)
{
    return ToStringViaBuilder(version);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NHydra
