#include "cypress_integration.h"

#include "private.h"
#include "chunk_location.h"
#include "chunk.h"
#include "chunk_view.h"
#include "chunk_list.h"
#include "chunk_replicator.h"
#include "medium.h"

#include <yt/yt/server/master/cell_master/bootstrap.h>
#include <yt/yt/server/master/cell_master/hydra_facade.h>

#include <yt/yt/server/master/chunk_server/chunk_manager.h>
#include <yt/yt/server/master/chunk_server/data_node_tracker.h>

#include <yt/yt/server/master/cypress_server/virtual.h>

#include <yt/yt/server/lib/object_server/helpers.h>

#include <yt/yt/core/misc/collection_helpers.h>

namespace NYT::NChunkServer {

using namespace NYTree;
using namespace NYPath;
using namespace NCypressServer;
using namespace NCellMaster;
using namespace NObjectClient;
using namespace NObjectServer;

////////////////////////////////////////////////////////////////////////////////

class TVirtualChunkLocationMap
    : public TVirtualMapBase
{
public:
    TVirtualChunkLocationMap(TBootstrap* bootstrap, INodePtr owningNode)
        : TVirtualMapBase(owningNode)
        , Bootstrap_(bootstrap)
    { }

private:
    TBootstrap* const Bootstrap_;

    std::vector<TString> GetKeys(i64 sizeLimit) const override
    {
        const auto& nodeTracker = Bootstrap_->GetDataNodeTracker();
        const auto& chunkLocations = nodeTracker->ChunkLocations();
        std::vector<TString> keys;
        keys.reserve(std::min(sizeLimit, std::ssize(chunkLocations)));
        for (auto [locationId, location] : chunkLocations) {
            if (std::ssize(keys) >= sizeLimit) {
                break;
            }
            keys.push_back(ToString(location->GetUuid()));
        }
        return keys;
    }

    i64 GetSize() const override
    {
        const auto& nodeTracker = Bootstrap_->GetDataNodeTracker();
        return nodeTracker->ChunkLocations().GetSize();
    }

    IYPathServicePtr FindItemService(TStringBuf key) const override
    {
        const auto& nodeTracker = Bootstrap_->GetDataNodeTracker();
        auto* location = nodeTracker->FindChunkLocationByUuid(TChunkLocationUuid::FromString(key));
        if (!IsObjectAlive(location)) {
            return nullptr;
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        return objectManager->GetProxy(location);
    }
};

INodeTypeHandlerPtr CreateChunkLocationMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::ChunkLocationMap,
        BIND_NO_PROPAGATE([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualChunkLocationMap>(bootstrap, owningNode);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualChunkMap
    : public TVirtualMulticellMapBase
{
public:
    TVirtualChunkMap(TBootstrap* bootstrap, INodePtr owningNode, EObjectType type)
        : TVirtualMulticellMapBase(bootstrap, owningNode)
        , Type_(type)
    { }

private:
    const EObjectType Type_;

    std::vector<TObjectId> GetFilteredChunkIds(i64 sizeLimit) const
    {
        Bootstrap_->GetHydraFacade()->RequireLeader();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& chunkReplicator = chunkManager->GetChunkReplicator();
        switch (Type_) {
            case EObjectType::LostChunkMap:
                return ToObjectIds(chunkReplicator->LostChunks(), sizeLimit);
            case EObjectType::LostVitalChunkMap:
                return ToObjectIds(chunkReplicator->LostVitalChunks(), sizeLimit);
            case EObjectType::PrecariousChunkMap:
                return ToObjectIds(chunkReplicator->PrecariousChunks(), sizeLimit);
            case EObjectType::PrecariousVitalChunkMap:
                return ToObjectIds(chunkReplicator->PrecariousVitalChunks(), sizeLimit);
            case EObjectType::OverreplicatedChunkMap:
                return ToObjectIds(chunkReplicator->OverreplicatedChunks(), sizeLimit);
            case EObjectType::UnderreplicatedChunkMap:
                return ToObjectIds(chunkReplicator->UnderreplicatedChunks(), sizeLimit);
            case EObjectType::DataMissingChunkMap:
                return ToObjectIds(chunkReplicator->DataMissingChunks(), sizeLimit);
            case EObjectType::ParityMissingChunkMap:
                return ToObjectIds(chunkReplicator->ParityMissingChunks(), sizeLimit);
            case EObjectType::QuorumMissingChunkMap:
                return ToObjectIds(chunkReplicator->QuorumMissingChunks(), sizeLimit);
            case EObjectType::UnsafelyPlacedChunkMap:
                return ToObjectIds(chunkReplicator->UnsafelyPlacedChunks(), sizeLimit);
            case EObjectType::InconsistentlyPlacedChunkMap:
                return ToObjectIds(chunkReplicator->InconsistentlyPlacedChunks(), sizeLimit);
            case EObjectType::ForeignChunkMap:
                return ToObjectIds(chunkManager->ForeignChunks(), sizeLimit);
            case EObjectType::OldestPartMissingChunkMap:
                return ToObjectIds(chunkReplicator->OldestPartMissingChunks(), sizeLimit);
            default:
                YT_ABORT();
        }
    }

    bool FilteredChunksContain(TChunk* chunk) const
    {
        Bootstrap_->GetHydraFacade()->RequireLeader();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& chunkReplicator = chunkManager->GetChunkReplicator();
        switch (Type_) {
            case EObjectType::LostChunkMap:
                return chunkReplicator->LostChunks().contains(chunk);
            case EObjectType::LostVitalChunkMap:
                return chunkReplicator->LostVitalChunks().contains(chunk);
            case EObjectType::PrecariousChunkMap:
                return chunkReplicator->PrecariousChunks().contains(chunk);
            case EObjectType::PrecariousVitalChunkMap:
                return chunkReplicator->PrecariousVitalChunks().contains(chunk);
            case EObjectType::OverreplicatedChunkMap:
                return chunkReplicator->OverreplicatedChunks().contains(chunk);
            case EObjectType::UnderreplicatedChunkMap:
                return chunkReplicator->UnderreplicatedChunks().contains(chunk);
            case EObjectType::DataMissingChunkMap:
                return chunkReplicator->DataMissingChunks().contains(chunk);
            case EObjectType::ParityMissingChunkMap:
                return chunkReplicator->ParityMissingChunks().contains(chunk);
            case EObjectType::QuorumMissingChunkMap:
                return chunkReplicator->QuorumMissingChunks().contains(chunk);
            case EObjectType::UnsafelyPlacedChunkMap:
                return chunkReplicator->UnsafelyPlacedChunks().contains(chunk);
            case EObjectType::InconsistentlyPlacedChunkMap:
                return chunkReplicator->InconsistentlyPlacedChunks().contains(chunk);
            case EObjectType::ForeignChunkMap:
                return chunkManager->ForeignChunks().contains(chunk);
            case EObjectType::OldestPartMissingChunkMap:
                return chunkReplicator->OldestPartMissingChunks().contains(chunk);
            default:
                YT_ABORT();
        }
    }

    i64 GetFilteredChunkCount() const
    {
        Bootstrap_->GetHydraFacade()->RequireLeader();
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        const auto& chunkReplicator = chunkManager->GetChunkReplicator();
        switch (Type_) {
            case EObjectType::LostChunkMap:
                return chunkReplicator->LostChunks().size();
            case EObjectType::LostVitalChunkMap:
                return chunkReplicator->LostVitalChunks().size();
            case EObjectType::PrecariousChunkMap:
                return chunkReplicator->PrecariousChunks().size();
            case EObjectType::PrecariousVitalChunkMap:
                return chunkReplicator->PrecariousVitalChunks().size();
            case EObjectType::OverreplicatedChunkMap:
                return chunkReplicator->OverreplicatedChunks().size();
            case EObjectType::UnderreplicatedChunkMap:
                return chunkReplicator->UnderreplicatedChunks().size();
            case EObjectType::DataMissingChunkMap:
                return chunkReplicator->DataMissingChunks().size();
            case EObjectType::ParityMissingChunkMap:
                return chunkReplicator->ParityMissingChunks().size();
            case EObjectType::QuorumMissingChunkMap:
                return chunkReplicator->QuorumMissingChunks().size();
            case EObjectType::UnsafelyPlacedChunkMap:
                return chunkReplicator->UnsafelyPlacedChunks().size();
            case EObjectType::InconsistentlyPlacedChunkMap:
                return chunkReplicator->InconsistentlyPlacedChunks().size();
            case EObjectType::ForeignChunkMap:
                return chunkManager->ForeignChunks().size();
            case EObjectType::OldestPartMissingChunkMap:
                return chunkReplicator->OldestPartMissingChunks().size();
            default:
                YT_ABORT();
        }
    }

    std::vector<TObjectId> GetKeys(i64 sizeLimit) const override
    {
        if (Type_ == EObjectType::ChunkMap) {
            const auto& chunkManager = Bootstrap_->GetChunkManager();
            return ToObjectIds(GetValues(chunkManager->Chunks(), sizeLimit));
        } else {
            return GetFilteredChunkIds(sizeLimit);
        }
    }

    bool IsValid(TObject* object) const override
    {
        auto type = object->GetType();
        if (type != EObjectType::Chunk &&
            type != EObjectType::ErasureChunk &&
            type != EObjectType::JournalChunk &&
            type != EObjectType::ErasureJournalChunk)
        {
            return false;
        }

        if (Type_ == EObjectType::ChunkMap) {
            return true;
        }

        auto* chunk = object->As<TChunk>();
        return FilteredChunksContain(chunk);
    }

    i64 GetSize() const override
    {
        if (Type_ == EObjectType::ChunkMap) {
            const auto& chunkManager = Bootstrap_->GetChunkManager();
            return chunkManager->Chunks().GetSize();
        } else {
            return GetFilteredChunkCount();
        }
    }

    NYPath::TYPath GetWellKnownPath() const override
    {
        switch (Type_) {
            case EObjectType::ChunkMap:
                return "//sys/chunks";
            case EObjectType::LostChunkMap:
                return "//sys/lost_chunks";
            case EObjectType::LostVitalChunkMap:
                return "//sys/lost_vital_chunks";
            case EObjectType::PrecariousChunkMap:
                return "//sys/precarious_chunks";
            case EObjectType::PrecariousVitalChunkMap:
                return "//sys/precarious_vital_chunks";
            case EObjectType::OverreplicatedChunkMap:
                return "//sys/overreplicated_chunks";
            case EObjectType::UnderreplicatedChunkMap:
                return "//sys/underreplicated_chunks";
            case EObjectType::DataMissingChunkMap:
                return "//sys/data_missing_chunks";
            case EObjectType::ParityMissingChunkMap:
                return "//sys/parity_missing_chunks";
            case EObjectType::OldestPartMissingChunkMap:
                return "//sys/oldest_part_missing_chunks";
            case EObjectType::QuorumMissingChunkMap:
                return "//sys/quorum_missing_chunks";
            case EObjectType::UnsafelyPlacedChunkMap:
                return "//sys/unsafely_placed_chunks";
            case EObjectType::InconsistentlyPlacedChunkMap:
                return "//sys/inconsistently_placed_chunks";
            case EObjectType::ForeignChunkMap:
                return "//sys/foreign_chunks";
            default:
                YT_ABORT();
        }
    }
};

INodeTypeHandlerPtr CreateChunkMapTypeHandler(
    TBootstrap* bootstrap,
    EObjectType type)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        type,
        BIND_NO_PROPAGATE([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualChunkMap>(bootstrap, owningNode, type);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualChunkViewMap
    : public TVirtualMulticellMapBase
{
public:
    using TVirtualMulticellMapBase::TVirtualMulticellMapBase;

private:
    std::vector<TObjectId> GetKeys(i64 sizeLimit) const override
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        return ToObjectIds(GetValues(chunkManager->ChunkViews(), sizeLimit));
    }

    bool IsValid(TObject* object) const override
    {
        return object->GetType() == EObjectType::ChunkView;
    }

    i64 GetSize() const override
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        return chunkManager->ChunkViews().GetSize();
    }

    TYPath GetWellKnownPath() const override
    {
        return "//sys/chunk_views";
    }
};

INodeTypeHandlerPtr CreateChunkViewMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::ChunkViewMap,
        BIND_NO_PROPAGATE([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualChunkViewMap>(bootstrap, owningNode);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualChunkListMap
    : public TVirtualMulticellMapBase
{
public:
    using TVirtualMulticellMapBase::TVirtualMulticellMapBase;

private:
    std::vector<TObjectId> GetKeys(i64 sizeLimit) const override
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        return ToObjectIds(GetValues(chunkManager->ChunkLists(), sizeLimit));
    }

    bool IsValid(TObject* object) const override
    {
        return object->GetType() == EObjectType::ChunkList;
    }

    i64 GetSize() const override
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        return chunkManager->ChunkLists().GetSize();
    }

    TYPath GetWellKnownPath() const override
    {
        return "//sys/chunk_lists";
    }
};

INodeTypeHandlerPtr CreateChunkListMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::ChunkListMap,
        BIND_NO_PROPAGATE([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualChunkListMap>(bootstrap, owningNode);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

class TVirtualMediumMap
    : public TVirtualMapBase
{
public:
    TVirtualMediumMap(TBootstrap* bootstrap, INodePtr owningNode)
        : TVirtualMapBase(owningNode)
        , Bootstrap_(bootstrap)
    { }

private:
    TBootstrap* const Bootstrap_;

    std::vector<TString> GetKeys(i64 /*sizeLimit*/) const override
    {
        std::vector<TString> keys;
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        for (auto [mediumId, medium] : chunkManager->Media()) {
            keys.push_back(medium->GetName());
        }
        return keys;
    }

    i64 GetSize() const override
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        return chunkManager->Media().GetSize();
    }

    IYPathServicePtr FindItemService(TStringBuf key) const override
    {
        const auto& chunkManager = Bootstrap_->GetChunkManager();
        auto* medium = chunkManager->FindMediumByName(TString(key));
        if (!IsObjectAlive(medium)) {
            return nullptr;
        }

        const auto& objectManager = Bootstrap_->GetObjectManager();
        return objectManager->GetProxy(medium);
    }
};

INodeTypeHandlerPtr CreateMediumMapTypeHandler(TBootstrap* bootstrap)
{
    YT_VERIFY(bootstrap);

    return CreateVirtualTypeHandler(
        bootstrap,
        EObjectType::MediumMap,
        BIND_NO_PROPAGATE([=] (INodePtr owningNode) -> IYPathServicePtr {
            return New<TVirtualMediumMap>(bootstrap, owningNode);
        }),
        EVirtualNodeOptions::RedirectSelf);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NChunkServer
