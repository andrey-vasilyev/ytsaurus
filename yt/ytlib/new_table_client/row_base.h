#pragma once

#include "public.h"

namespace NYT {
namespace NVersionedTableClient {

////////////////////////////////////////////////////////////////////////////////

//! Checks that #type is allowed to appear in data. Throws on failure.
void ValidateDataValueType(EValueType type);

//! Checks that #type is allowed to appear in keys. Throws on failure.
void ValidateKeyValueType(EValueType type);

//! Checks that #type is allowed to appear in schema. Throws on failure.
void ValidateSchemaValueType(EValueType type);

////////////////////////////////////////////////////////////////////////////////

template <class TValue>
TValue MakeSentinelValue(EValueType type, int id = 0)
{
    TValue result;
    result.Id = id;
    result.Type = type;
    return result;
}

template <class TValue>
TValue MakeInt64Value(i64 value, int id = 0)
{
    TValue result;
    result.Id = id;
    result.Type = EValueType::Int64;
    result.Data.Int64 = value;
    return result;
}

template <class TValue>
TValue MakeUint64Value(ui64 value, int id = 0)
{
    TValue result;
    result.Id = id;
    result.Type = EValueType::Uint64;
    result.Data.Int64 = value;
    return result;
}

template <class TValue>
TValue MakeDoubleValue(double value, int id = 0)
{
    TValue result;
    result.Id = id;
    result.Type = EValueType::Double;
    result.Data.Double = value;
    return result;
}

template <class TValue>
TValue MakeBooleanValue(bool value, int id = 0)
{
    TValue result;
    result.Id = id;
    result.Type = EValueType::Boolean;
    result.Data.Boolean = value;
    return result;
}

template <class TValue>
TValue MakeStringValue(const TStringBuf& value, int id = 0)
{
    TValue result;
    result.Id = id;
    result.Type = EValueType::String;
    result.Length = value.length();
    result.Data.String = value.begin();
    return result;
}

template <class TValue>
TValue MakeAnyValue(const TStringBuf& value, int id = 0)
{
    TValue result;
    result.Id = id;
    result.Type = EValueType::Any;
    result.Length = value.length();
    result.Data.String = value.begin();
    return result;
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
int GetKeyComparerValueCount(const T&, int prefixLength)
{
    return prefixLength;
}

//! Provides a comparer functor for row-like entities
//! trimmed to a given length.
class TKeyComparer
{
public:
    explicit TKeyComparer(int prefixLength)
        : PrefixLength_(prefixLength)
    { }

    template <class TLhs, class TRhs>
    int operator () (const TLhs& lhs, const TRhs& rhs) const
    {
        int lhsLength = GetKeyComparerValueCount(lhs, PrefixLength_);
        int rhsLength = GetKeyComparerValueCount(rhs, PrefixLength_);
        int minLength = std::min(lhsLength, rhsLength);
        for (int index = 0; index < minLength; ++index) {
            int result = CompareRowValues(lhs[index], rhs[index]);
            if (result != 0) {
                return result;
            }
        }
        return lhsLength - rhsLength;
    }

private:
    int PrefixLength_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NVersionedTableClient
} // namespace NYT
