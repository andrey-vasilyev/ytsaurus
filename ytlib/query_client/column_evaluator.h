#pragma once

#include "public.h"
#include "evaluation_helpers.h"

#include <yt/ytlib/query_client/function_registry.h>

#include <yt/ytlib/table_client/unversioned_row.h>

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TColumnEvaluator
    : public TRefCounted
{
public:
    TColumnEvaluator(
        const TTableSchema& schema,
        int keySize,
        const IFunctionRegistryPtr functionRegistry);

    void EvaluateKey(
        TRow fullRow,
        const TRowBufferPtr& buffer,
        int index) const;

    void EvaluateKeys(
        TRow fullRow,
        const TRowBufferPtr& buffer) const;

    TRow EvaluateKeys(
        TRow partialRow,
        const TRowBufferPtr& buffer,
        const NTableClient::TNameTableToSchemaIdMapping& idMapping,
        const TTableSchema& schema) const;

    const std::vector<int>& GetReferenceIds(int index) const;
    TConstExpressionPtr GetExpression(int index) const;

    void PrepareEvaluator();

private:
    const TTableSchema Schema_;
    const int KeySize_;
    const IFunctionRegistryPtr FunctionRegistry_;

    std::vector<TCGExpressionCallback> Evaluators_;
    std::vector<TCGVariables> Variables_;
    std::vector<std::vector<int>> ReferenceIds_;
    std::vector<TConstExpressionPtr> Expressions_;
    std::vector<std::vector<std::vector<bool>>> AllLiteralArgs_;
};

DEFINE_REFCOUNTED_TYPE(TColumnEvaluator);

////////////////////////////////////////////////////////////////////////////////

class TColumnEvaluatorCache
    : public TRefCounted
{
public:
    explicit TColumnEvaluatorCache(
        TColumnEvaluatorCacheConfigPtr config,
        const IFunctionRegistryPtr functionRegistry);
    ~TColumnEvaluatorCache();

    TColumnEvaluatorPtr Find(const TTableSchema& schema, int keySize);

private:
    class TImpl;

    DECLARE_NEW_FRIEND();

    const TIntrusivePtr<TImpl> Impl_;
};

DEFINE_REFCOUNTED_TYPE(TColumnEvaluatorCache);

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

