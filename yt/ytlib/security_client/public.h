#pragma once

#include <core/misc/guid.h>

#include <ytlib/object_client/public.h>

namespace NYT {
namespace NSecurityClient {

////////////////////////////////////////////////////////////////////////////////

typedef NObjectClient::TObjectId TAccountId;
typedef NObjectClient::TObjectId TSubjectId;
typedef NObjectClient::TObjectId TUserId;
typedef NObjectClient::TObjectId TGroupId;

extern const Stroka TmpAccountName;
extern const Stroka SysAccountName;

extern const Stroka RootUserName;
extern const Stroka GuestUserName;

extern const Stroka EveryoneGroupName;
extern const Stroka UsersGroupName;

DECLARE_ENUM(ESecurityAction,
    ((Undefined)(0))  // Intermediate state, used internally.
    ((Allow)    (1))  // Let'em go!
    ((Deny)     (2))  // No way!
);

DECLARE_ENUM(EErrorCode,
    ((AuthenticationError)     (900))
    ((AuthorizationError)      (901))
    ((AccountLimitExceeded)    (902))
    ((UserBanned)              (903))
    ((RequestRateLimitExceeded)(904))
);

////////////////////////////////////////////////////////////////////////////////

} // namespace NSecurityClient
} // namespace NYT

