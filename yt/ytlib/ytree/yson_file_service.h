#pragma once

#include "public.h"

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

//! Creates a service for performing simple, non-cached YPath
//! requests to a given file.
NYTree::IYPathServicePtr CreateYsonFileService(const Stroka& fileName);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT
