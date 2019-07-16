#pragma once

#include "public.h"

#include <yt/server/master/cell_master/public.h>

#include <yt/server/master/transaction_server/public.h>

#include <yt/server/master/object_server/public.h>

#include <yt/core/yson/string.h>

namespace NYT::NCypressServer {

////////////////////////////////////////////////////////////////////////////////

const THashMap<TString, TCypressNodeBase*>& GetMapNodeChildMap(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction,
    THashMap<TString, TCypressNodeBase*>* storage);

std::vector<TCypressNodeBase*> GetMapNodeChildList(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

const std::vector<TCypressNodeBase*>& GetListNodeChildList(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

std::vector<std::pair<TString, TCypressNodeBase*>> SortKeyToChild(
    const THashMap<TString, TCypressNodeBase*>& keyToChildMap);

TCypressNodeBase* FindMapNodeChild(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction,
    TStringBuf key);

TStringBuf FindMapNodeChildKey(
    TMapNode* parentNode,
    TCypressNodeBase* trunkChildNode);

int FindListNodeChildIndex(
    TListNode* parentNode,
    TCypressNodeBase* trunkChildNode);

THashMap<TString, NYson::TYsonString> GetNodeAttributes(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

THashSet<TString> ListNodeAttributes(
    const TCypressManagerPtr& cypressManager,
    TCypressNodeBase* trunkNode,
    NTransactionServer::TTransaction* transaction);

void AttachChild(
    const NObjectServer::TObjectManagerPtr& objectManager,
    TCypressNodeBase* trunkParent,
    TCypressNodeBase* child);

void DetachChild(
    const NObjectServer::TObjectManagerPtr& objectManager,
    TCypressNodeBase* trunkParent,
    TCypressNodeBase* child,
    bool unref);

bool NodeHasKey(const TCypressNodeBase* node);

bool IsAncestorOf(
    const TCypressNodeBase* trunkAncestor,
    const TCypressNodeBase* trunkDescendant);

TNodeId MakePortalExitNodeId(
    TNodeId entranceNodeId,
    NObjectClient::TCellTag exitCellTag);

TNodeId MakePortalEntranceNodeId(
    TNodeId exitNodeId,
    NObjectClient::TCellTag entranceCellTag);

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT::NCypressServer
