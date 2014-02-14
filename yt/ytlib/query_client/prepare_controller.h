#pragma once

#include "public.h"
#include "plan_fragment.h"

namespace NYT {
namespace NQueryClient {

////////////////////////////////////////////////////////////////////////////////

class TPrepareController
{
public:
    TPrepareController(
        IPrepareCallbacks* callbacks,
        TTimestamp timestamp,
        const Stroka& source);

    ~TPrepareController();

    TPlanFragment Run();

private:
    void ParseSource();
    void GetInitialSplits();
    void CheckAndPruneReferences();
    void TypecheckExpressions();
    void MoveAggregateExpressions();

    IPrepareCallbacks* Callbacks_;
    const Stroka& Source_;
    TPlanContextPtr Context_;
    const TOperator* Head_;

};

////////////////////////////////////////////////////////////////////////////////

} // namespace NQueryClient
} // namespace NYT

