#include "permission.h"

#include <yt/core/misc/error.h>
#include <yt/core/misc/string.h>

namespace NYT {
namespace NYTree {

////////////////////////////////////////////////////////////////////////////////

static const Stroka AllPermissionsName("all");

////////////////////////////////////////////////////////////////////////////////

EPermissionSet ParsePermissions(const std::vector<Stroka>& items)
{
    auto result = NonePermissions;
    for (const auto& item : items) {
        if (item == AllPermissionsName) {
            return AllPermissions;
        } else {
            auto permission = ParseEnum<EPermission>(item);
            result |= permission;
        }
    }
    return result;
}

std::vector<Stroka> FormatPermissions(EPermissionSet permissions)
{
    std::vector<Stroka> result;
    for (auto value : TEnumTraits<EPermission>::GetDomainValues()) {
        if ((permissions & value) != NonePermissions) {
            result.push_back(FormatEnum(value));
        }
    }
    return result;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

