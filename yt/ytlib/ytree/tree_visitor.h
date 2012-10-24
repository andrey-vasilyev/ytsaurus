#pragma once

#include "attribute_provider.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

void VisitTree(
    INodePtr root,
    IYsonConsumer* consumer,
    const TAttributeFilter& attributeFilter = TAttributeFilter::All);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
