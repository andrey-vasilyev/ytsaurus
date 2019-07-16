#include "acl.h"
#include "security_manager.h"
#include "subject.h"

#include <yt/server/master/cell_master/serialize.h>

#include <yt/ytlib/security_client/acl.h>

#include <yt/core/ytree/fluent.h>
#include <yt/core/ytree/node.h>
#include <yt/core/ytree/permission.h>
#include <yt/core/ytree/yson_serializable.h>

namespace NYT::NSecurityServer {

using namespace NYTree;
using namespace NYson;
using namespace NSecurityClient;
using namespace NObjectServer;
using namespace NCellMaster;

////////////////////////////////////////////////////////////////////////////////

TAccessControlEntry::TAccessControlEntry()
    : Action(ESecurityAction::Undefined)
    , InheritanceMode(EAceInheritanceMode::ObjectAndDescendants)
{ }

TAccessControlEntry::TAccessControlEntry(
    ESecurityAction action,
    TSubject* subject,
    EPermissionSet permissions,
    EAceInheritanceMode inheritanceMode)
    : Action(action)
    , Subjects{subject}
    , Permissions(permissions)
    , InheritanceMode(inheritanceMode)
{ }

void TAccessControlEntry::Persist(NCellMaster::TPersistenceContext& context)
{
    using NYT::Persist;
    Persist(context, Subjects);
    Persist(context, Permissions);
    Persist(context, Action);
    Persist(context, InheritanceMode);
    // COMPAT(babenko)
    if (context.GetVersion() >= EMasterSnapshotVersion::ColumnarAcls) {
        Persist(context, Columns);
    }
}

void Serialize(const TAccessControlEntry& ace, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("action").Value(ace.Action)
            .Item("subjects").DoListFor(ace.Subjects, [] (TFluentList fluent, TSubject* subject) {
                fluent.Item().Value(subject->GetName());
            })
            .Item("permissions").Value(FormatPermissions(ace.Permissions))
            .Item("inheritance_mode").Value(ace.InheritanceMode)
            .DoIf(ace.Columns.has_value(), [&] (auto fluent) {
                fluent
                    .Item("columns").Value(ace.Columns);
            })
        .EndMap();
}

////////////////////////////////////////////////////////////////////////////////

void Load(NCellMaster::TLoadContext& context, TAccessControlList& acl)
{
    Load(context, acl.Entries);
}

void Save(NCellMaster::TSaveContext& context, const TAccessControlList& acl)
{
    Save(context, acl.Entries);
}

void Serialize(const TAccessControlList& acl, IYsonConsumer* consumer)
{
    BuildYsonFluently(consumer)
        .Value(acl.Entries);
}

void Deserialize(
    TAccessControlList& acl,
    INodePtr node,
    TSecurityManagerPtr securityManager,
    std::vector<TString>* missingSubjects)
{
    auto serializableAcl = ConvertTo<TSerializableAccessControlList>(node);
    for (const auto& serializableAce : serializableAcl.Entries) {
        TAccessControlEntry ace;

        // Action
        ace.Action = serializableAce.Action;

        // Subject
        for (const auto& name : serializableAce.Subjects) {
            auto* subject = securityManager->FindSubjectByName(name);
            if (missingSubjects && !subject) {
                missingSubjects->push_back(name);
                continue;
            }
            if (!IsObjectAlive(subject)) {
                THROW_ERROR_EXCEPTION("No such subject %Qv",
                    name);
            }
            ace.Subjects.push_back(subject);
        }

        // Permissions
        ace.Permissions = serializableAce.Permissions;

        // Inheritance mode
        ace.InheritanceMode = serializableAce.InheritanceMode;

        // Columns
        ace.Columns = std::move(serializableAce.Columns);

        acl.Entries.push_back(ace);
    }
}

////////////////////////////////////////////////////////////////////////////////

TAccessControlDescriptor::TAccessControlDescriptor(TObjectBase* object)
    : Object_(object)
{ }

void TAccessControlDescriptor::Clear()
{
    ClearEntries();
    SetOwner(nullptr);
}

TSubject* TAccessControlDescriptor::GetOwner() const
{
    return Owner_;
}

void TAccessControlDescriptor::SetOwner(TSubject* owner)
{
    if (Owner_ == owner)
        return;

    if (Owner_) {
        Owner_->UnlinkObject(Object_);
    }

    Owner_ = owner;

    if (Owner_) {
        Owner_->LinkObject(Object_);
    }
}

void TAccessControlDescriptor::AddEntry(const TAccessControlEntry& ace)
{
    for (auto* subject : ace.Subjects) {
        subject->LinkObject(Object_);
    }
    Acl_.Entries.push_back(ace);
}

void TAccessControlDescriptor::ClearEntries()
{
    for (const auto& ace : Acl_.Entries) {
        for (auto* subject : ace.Subjects) {
            subject->UnlinkObject(Object_);
        }
    }
    Acl_.Entries.clear();
}

void TAccessControlDescriptor::SetEntries(const TAccessControlList& acl)
{
    ClearEntries();
    for (const auto& ace : acl.Entries) {
        AddEntry(ace);
    }
}

void TAccessControlDescriptor::OnSubjectDestroyed(TSubject* subject, TSubject* defaultOwner)
{
    // Remove the subject from every ACE.
    for (auto& ace : Acl_.Entries) {
        auto it = std::remove_if(
            ace.Subjects.begin(),
            ace.Subjects.end(),
            [=] (TSubject* aceSubject) {
                return subject == aceSubject;
            });
        ace.Subjects.erase(it, ace.Subjects.end());
    }

    // Remove all empty ACEs.
    {
        auto it = std::remove_if(
            Acl_.Entries.begin(),
            Acl_.Entries.end(),
            [] (const TAccessControlEntry& ace) {
                return ace.Subjects.empty();
            });
        Acl_.Entries.erase(it, Acl_.Entries.end());
    }

    // Reset owner to default, if needed.
    if (Owner_ == subject) {
        Owner_ = nullptr; // prevent updating LinkedObjects
        SetOwner(defaultOwner);
    }
}

void TAccessControlDescriptor::Save(NCellMaster::TSaveContext& context) const
{
    using NYT::Save;
    Save(context, Acl_);
    Save(context, Inherit_);
    Save(context, Owner_);
}

void TAccessControlDescriptor::Load(NCellMaster::TLoadContext& context)
{
    using NYT::Load;
    Load(context, Acl_);
    Load(context, Inherit_);
    Load(context, Owner_);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NSecurityServer

