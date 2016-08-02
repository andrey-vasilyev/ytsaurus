#pragma once

#include "public.h"

namespace NYT {
namespace NHiveClient {

////////////////////////////////////////////////////////////////////////////////

struct ITransactionParticipant
    : public virtual TRefCounted
{
    virtual const TCellId& GetCellId() const = 0;
    virtual bool IsValid() const = 0;

    virtual TFuture<void> PrepareTransaction(const TTransactionId& transactionId) = 0;
    virtual TFuture<void> CommitTransaction(const TTransactionId& transactionId, TTimestamp commitTimestamp) = 0;
    virtual TFuture<void> AbortTransaction(const TTransactionId& transactionId) = 0;
};

DEFINE_REFCOUNTED_TYPE(ITransactionParticipant)

////////////////////////////////////////////////////////////////////////////////

} // namespace NHiveClient
} // namespace NYT
