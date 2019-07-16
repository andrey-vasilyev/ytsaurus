#pragma once

#include <yt/server/lib/object_server/public.h>

namespace NYT::NObjectServer {

////////////////////////////////////////////////////////////////////////////////

namespace NProto {

class TReqCreateForeignObject;
class TReqRemoveForeignObject;

} // namespace NProto

////////////////////////////////////////////////////////////////////////////////

DEFINE_BIT_ENUM(ETypeFlags,
    ((None)                   (0x0000))
    ((ReplicateCreate)        (0x0001)) // replicate object creation
    ((ReplicateDestroy)       (0x0002)) // replicate object destruction
    ((ReplicateAttributes)    (0x0004)) // replicate object attribute changes
    ((Creatable)              (0x0008)) // objects of this type can be created at runtime
    ((Externalizable)         (0x0010)) // objects of this (versioned) type can be externalized to another cell (e.g. tables, files)
    ((ForbidInheritAclChange) (0x0020)) // inherit_acl attribute cannot be changed
    ((ForbidLocking)          (0x0040)) // no locks can be taken objects of this (versioned) type
    ((TwoPhaseCreation)       (0x0080)) // employ two-phase creation protocol
);

////////////////////////////////////////////////////////////////////////////////

DECLARE_REFCOUNTED_CLASS(TObjectManager)
DECLARE_REFCOUNTED_CLASS(TGarbageCollector)

DECLARE_REFCOUNTED_CLASS(TObjectManagerConfig)
DECLARE_REFCOUNTED_CLASS(TDynamicObjectManagerConfig)
DECLARE_REFCOUNTED_CLASS(TObjectServiceConfig)

class TObjectBase;
class TNonversionedObjectBase;

class TObjectProxyBase;

class TAttributeSet;

struct TObjectTypeMetadata;

DECLARE_ENTITY_TYPE(TSchemaObject, TObjectId, ::THash<TObjectId>);

class TMasterObject;

DECLARE_REFCOUNTED_STRUCT(IObjectProxy)
DECLARE_REFCOUNTED_STRUCT(IObjectTypeHandler)

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NObjectServer
