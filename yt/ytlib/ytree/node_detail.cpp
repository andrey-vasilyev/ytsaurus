#include "stdafx.h"
#include "node_detail.h"
#include "ypath_detail.h"
#include "ypath.h"
#include "tree_visitor.h"
#include "tree_builder.h"
#include "yson_writer.h"

namespace NYT {
namespace NYTree {

using namespace NRpc;

////////////////////////////////////////////////////////////////////////////////

IYPathService2::TNavigateResult2 TNodeBase::Navigate2(TYPath path)
{
    if (path.empty()) {
        return TNavigateResult2::Here();
    }

    if (path[0] == '@') {
        auto attributes = GetAttributes();
        if (~attributes == NULL) {
            throw TYTreeException() << "Node has no custom attributes";
        }

        // TODO: virtual attributes

        return TNavigateResult2::There(
            ~IYPathService2::FromNode(~attributes),
            TYPath(path.begin() + 1, path.end()));
    }

    return NavigateRecursive2(path);
}

IYPathService2::TNavigateResult2 TNodeBase::NavigateRecursive2(TYPath path)
{
    UNUSED(path);
    throw TYTreeException() << "Navigation is not supported";
}

void TNodeBase::Invoke2(NRpc::IServiceContext* context)
{
    Stroka verb = context->GetVerb();
    if (verb == "Get") {
        Get2Thunk(context);
    } else if (verb == "Set") {
        Set2Thunk(context);
    } else if (verb == "Remove") {
        Remove2Thunk(context);
    } else {
        context->Reply(TError(EErrorCode::NoSuchMethod));
    }
}

RPC_SERVICE_METHOD_IMPL(TNodeBase, Get2)
{
    Stroka path = context->GetPath();
    if (path.empty()) {
        GetSelf2(request, response, context);
    } else {
        GetRecursive2(path, request, response, context);
    }
}

void TNodeBase::GetSelf2(TReqGet2* request, TRspGet2* response, TCtxGet2::TPtr context)
{
    UNUSED(request);

    TStringStream stream;
    TYsonWriter writer(&stream, TYsonWriter::EFormat::Binary);
    TTreeVisitor visitor(&writer, false);
    visitor.Visit(this);

    response->SetValue(stream.Str());
    context->Reply();
}

void TNodeBase::GetRecursive2(TYPath path, TReqGet2* request, TRspGet2* response, TCtxGet2::TPtr context)
{
    UNUSED(path);
    UNUSED(request);
    UNUSED(response);
    UNUSED(context);

    // TODO:
    ythrow yexception() << "Child is not found";
}

RPC_SERVICE_METHOD_IMPL(TNodeBase, Set2)
{
    Stroka path = context->GetPath();
    if (path.empty()) {
        SetSelf2(request, response, context);
    } else {
        SetRecursive2(path, request, response, context);
    }
}

void TNodeBase::SetSelf2(TReqSet2* request, TRspSet2* response, TCtxSet2::TPtr context)
{
    UNUSED(request);
    UNUSED(response);
    UNUSED(context);

    ythrow yexception() << "Cannot modify the node";
}

void TNodeBase::SetRecursive2(TYPath path, TReqSet2* request, TRspSet2* response, TCtxSet2::TPtr context)
{
    UNUSED(path);
    UNUSED(request);
    UNUSED(response);
    UNUSED(context);

    // TODO:
    ythrow yexception() << "Child is not found";
}

RPC_SERVICE_METHOD_IMPL(TNodeBase, Remove2)
{
    Stroka path = context->GetPath();
    if (path.empty()) {
        RemoveSelf2(request, response, context);
    } else {
        RemoveRecursive2(path, request, response, context);
    }
}

void TNodeBase::RemoveSelf2(TReqRemove2* request, TRspRemove2* response, TCtxRemove2::TPtr context)
{
    UNUSED(request);
    UNUSED(response);

    auto parent = GetParent();

    if (~parent == NULL) {
        throw TYTreeException() << "Cannot remove the root";
    }

    parent->AsComposite()->RemoveChild(this);
    context->Reply();
}

void TNodeBase::RemoveRecursive2(TYPath path, TReqRemove2* request, TRspRemove2* response, TCtxRemove2::TPtr context)
{
    UNUSED(path);
    UNUSED(request);
    UNUSED(response);
    UNUSED(context);

    // TODO:
    ythrow yexception() << "Child is not found";
}




IYPathService::TGetResult TNodeBase::Get(
    TYPath path,
    IYsonConsumer* consumer)
{
    if (path.empty()) {
        return GetSelf(consumer);
    }

    if (path[0] == '@') {
        auto attributes = GetAttributes();

        if (path == "@") {
            // TODO: use fluent API

            consumer->OnBeginMap();
            auto names = GetVirtualAttributeNames();
            FOREACH (const auto& name, names) {
                consumer->OnMapItem(name);
                YVERIFY(GetVirtualAttribute(name, consumer));
            }
            
            if (~attributes != NULL) {
                auto children = attributes->GetChildren();
                FOREACH (const auto& pair, children) {
                    consumer->OnMapItem(pair.First());
                    TTreeVisitor visitor(consumer);
                    visitor.Visit(pair.Second());
                }
            }

            consumer->OnEndMap(false);

            return TGetResult::CreateDone();
        } else {
            Stroka prefix;
            TYPath tailPath;
            ChopYPathPrefix(TYPath(path.begin() + 1, path.end()), &prefix, &tailPath);

            if (GetVirtualAttribute(prefix, consumer))
                return TGetResult::CreateDone();

            if (~attributes == NULL) {
                throw TYTreeException() << "Node has no custom attributes";
            }

            auto child = attributes->FindChild(prefix);
            if (~child == NULL) {
                throw TYTreeException() << Sprintf("Attribute %s is not found",
                    ~prefix.Quote());
            }

            TTreeVisitor visitor(consumer);
            visitor.Visit(child);
            return TGetResult::CreateDone();
        }
    } else {
        return GetRecursive(path, consumer);
    }
}

IYPathService::TGetResult TNodeBase::GetSelf(IYsonConsumer* consumer)
{
    TTreeVisitor visitor(consumer, false);
    visitor.Visit(this);
    return TGetResult::CreateDone();
}

IYPathService::TGetResult TNodeBase::GetRecursive(TYPath path, IYsonConsumer* consumer)
{
    UNUSED(consumer);
    return Navigate(path);
}

IYPathService::TNavigateResult TNodeBase::Navigate(TYPath path)
{
    if (path.empty()) {
        return TNavigateResult::CreateDone(this);
    }

    if (path[0] == '@') {
        auto attributes = GetAttributes();
        if (~attributes == NULL) {
            throw TYTreeException() << "Node has no custom attributes";
        }

        return TNavigateResult::CreateRecurse(
            IYPathService::FromNode(~attributes),
            TYPath(path.begin() + 1, path.end()));
    }

    return NavigateRecursive(path);
}

IYPathService::TNavigateResult TNodeBase::NavigateRecursive(TYPath path)
{
    UNUSED(path);
    throw TYTreeException() << "Navigation is not supported";
}

IYPathService::TSetResult TNodeBase::Set(
    TYPath path, 
    TYsonProducer::TPtr producer)
{
    if (path.empty()) {
        return SetSelf(producer);
    }

    if (path[0] == '@') {
        auto attributes = GetAttributes();
        if (~attributes == NULL) {
            attributes = ~GetFactory()->CreateMap();
            SetAttributes(attributes);
        }

        // TODO: should not be able to override a virtual attribute

        return IYPathService::TSetResult::CreateRecurse(
            IYPathService::FromNode(~attributes),
            TYPath(path.begin() + 1, path.end()));
    } else {
        return SetRecursive(path, producer);
    }
}

IYPathService::TSetResult TNodeBase::SetSelf(TYsonProducer::TPtr producer)
{
    UNUSED(producer);
    throw TYTreeException() << "Cannot modify the node";
}

IYPathService::TSetResult TNodeBase::SetRecursive(TYPath path, TYsonProducer::TPtr producer)
{
    UNUSED(producer);
    return Navigate(path);
}

IYPathService::TRemoveResult TNodeBase::Remove(TYPath path)
{
    if (path.empty()) {
        return RemoveSelf();
    }

    if (path[0] == '@') {
        auto attributes = GetAttributes();
        if (~attributes == NULL) {
            throw TYTreeException() << "Node has no custom attributes";
        }

        return IYPathService::TRemoveResult::CreateRecurse(
            IYPathService::FromNode(~attributes),
            TYPath(path.begin() + 1, path.end()));
    } else {
        return RemoveRecursive(path);
    }
}

IYPathService::TRemoveResult TNodeBase::RemoveSelf()
{
    auto parent = GetParent();

    if (~parent == NULL) {
        throw TYTreeException() << "Cannot remove the root";
    }

    parent->AsComposite()->RemoveChild(this);
    return TRemoveResult::CreateDone();
}

IYPathService::TRemoveResult TNodeBase::RemoveRecursive(TYPath path)
{
    return Navigate(path);
}

IYPathService::TLockResult TNodeBase::Lock(TYPath path)
{
    if (path.empty()) {
        return LockSelf();
    }
    
    if (path[0] == '@') {
        throw TYTreeException() << "Locking attributes is not supported";
    }

    return LockRecursive(path);
}

IYPathService::TLockResult TNodeBase::LockSelf()
{
    throw TYTreeException() << "Locking is not supported";
}

IYPathService::TLockResult TNodeBase::LockRecursive(TYPath path)
{
    return Navigate(path);
}

yvector<Stroka> TNodeBase::GetVirtualAttributeNames()
{
    return yvector<Stroka>();
}

bool TNodeBase::GetVirtualAttribute(const Stroka& name, IYsonConsumer* consumer)
{
    UNUSED(name);
    UNUSED(consumer);
    return false;
}

////////////////////////////////////////////////////////////////////////////////

IYPathService2::TNavigateResult2 TMapNodeMixin::NavigateRecursive2(TYPath path)
{
    Stroka prefix;
    TYPath tailPath;
    ChopYPathPrefix(path, &prefix, &tailPath);

    auto child = FindChild(prefix);
    if (~child == NULL) {
        return IYPathService2::TNavigateResult2::Here();
    } else {
        return IYPathService2::TNavigateResult2::There(~IYPathService2::FromNode(~child), tailPath);
    }
}

void TMapNodeMixin::SetRecursive2(TYPath path, const TYson& value, ITreeBuilder* builder)
{
    IMapNode::TPtr currentNode = this;
    TYPath currentPath = path;

    while (true) {
        Stroka prefix;
        TYPath tailPath;
        ChopYPathPrefix(currentPath, &prefix, &tailPath);

        if (tailPath.empty()) {
            builder->BeginTree();
            TStringInput input(value);
            TYsonReader reader(builder);
            reader.Read(&input);
            auto newChild = builder->EndTree();
            currentNode->AddChild(newChild, prefix);
            break;
        }

        auto newChild = GetFactory()->CreateMap();
        currentNode->AddChild(newChild, prefix);
        currentNode = newChild;
        currentPath = tailPath;
    }
}


IYPathService::TNavigateResult TMapNodeMixin::NavigateRecursive(TYPath path)
{
    Stroka prefix;
    TYPath tailPath;
    ChopYPathPrefix(path, &prefix, &tailPath);

    auto child = FindChild(prefix);
    if (~child == NULL) {
        throw TYTreeException() << Sprintf("Key %s is not found",
            ~prefix.Quote());
    }

    return IYPathService::TNavigateResult::CreateRecurse(IYPathService::FromNode(~child), tailPath);
}

IYPathService::TSetResult TMapNodeMixin::SetRecursive(
    TYPath path,
    TYsonProducer* producer,
    ITreeBuilder* builder)
{
    Stroka prefix;
    TYPath tailPath;
    ChopYPathPrefix(path, &prefix, &tailPath);

    auto child = FindChild(prefix);
    if (~child != NULL) {
        return IYPathService::TSetResult::CreateRecurse(IYPathService::FromNode(~child), tailPath);
    }

    if (tailPath.empty()) {
        builder->BeginTree();
        producer->Do(builder);
        auto newChild = builder->EndTree();
        AddChild(newChild, prefix);
        return IYPathService::TSetResult::CreateDone(newChild);
    } else {
        auto newChild = GetFactory()->CreateMap();
        AddChild(newChild, prefix);
        return IYPathService::TSetResult::CreateRecurse(IYPathService::FromNode(~newChild), tailPath);
    }
}

////////////////////////////////////////////////////////////////////////////////

IYPathService2::TNavigateResult2 TListNodeMixin::NavigateRecursive2(TYPath path)
{
    Stroka prefix;
    TYPath tailPath;
    ChopYPathPrefix(path, &prefix, &tailPath);

    try {
        int index = FromString<int>(prefix);
        return GetYPathChild2(index, tailPath);
    } catch (...) {
        return IYPathService2::TNavigateResult2::Here();
    }
}

void TListNodeMixin::SetRecursive2(TYPath path, const TYson& value, ITreeBuilder* builder)
{
    YUNREACHABLE();

    //IMapNode::TPtr currentNode = this;
    //TYPath currentPath = path;

    //while (true) {
    //    Stroka prefix;
    //    TYPath tailPath;
    //    ChopYPathPrefix(currentPath, &prefix, &tailPath);

    //    if (prefix.empty()) {
    //        throw TYTreeException() << "Empty child index";
    //    }

    //    if (prefix == "+") {
    //        return CreateYPathChild(GetChildCount(), tailPath, producer, builder);
    //    } else if (prefix == "-") {
    //        return CreateYPathChild(0, tailPath, producer, builder);
    //    }

    //    char lastPrefixCh = prefix[prefix.length() - 1];
    //    TStringBuf indexString =
    //        lastPrefixCh == '+' || lastPrefixCh == '-'
    //        ? TStringBuf(prefix.begin() + 1, prefix.end())
    //        : prefix;

    //    int index;
    //    try {
    //        index = FromString<int>(indexString);
    //    } catch (...) {
    //        throw TYTreeException() << Sprintf("Failed to parse child index %s",
    //            ~Stroka(indexString).Quote());
    //    }

    //    if (lastPrefixCh == '+') {
    //        return CreateYPathChild(index + 1, tailPath, producer, builder);
    //    } else if (lastPrefixCh == '-') {
    //        return CreateYPathChild(index, tailPath, producer, builder);
    //    } else {
    //        auto navigateResult = GetYPathChild(index, tailPath);
    //        YASSERT(navigateResult.Code == IYPathService::ECode::Recurse);
    //        return IYPathService::TSetResult::CreateRecurse(navigateResult.RecurseService, navigateResult.RecursePath);
    //    }

    //    auto newChild = GetFactory()->CreateMap();
    //    currentNode->AddChild(newChild, prefix);
    //    currentNode = newChild;
    //    currentPath = tailPath;
    //}
}

IYPathService2::TNavigateResult2 TListNodeMixin::GetYPathChild2(
    int index,
    TYPath tailPath) const
{
    int count = GetChildCount();
    if (count == 0) {
        throw TYTreeException() << "List is empty";
    }

    if (index < 0 || index >= count) {
        throw TYTreeException() << Sprintf("Invalid child index %d, expecting value in range 0..%d",
            index,
            count - 1);
    }

    auto child = FindChild(index);
    return IYPathService2::TNavigateResult2::There(~IYPathService2::FromNode(~child), tailPath);
}


IYPathService::TNavigateResult TListNodeMixin::NavigateRecursive(TYPath path)
{
    Stroka prefix;
    TYPath tailPath;
    ChopYPathPrefix(path, &prefix, &tailPath);

    int index;
    try {
        index = FromString<int>(prefix);
    } catch (...) {
        throw TYTreeException() << Sprintf("Failed to parse child index %s",
            ~prefix.Quote());
    }

    return GetYPathChild(index, tailPath);
}

IYPathService::TSetResult TListNodeMixin::SetRecursive(
    TYPath path,
    TYsonProducer* producer,
    ITreeBuilder* builder)
{
    Stroka prefix;
    TYPath tailPath;
    ChopYPathPrefix(path, &prefix, &tailPath);

    if (prefix.empty()) {
        throw TYTreeException() << "Empty child index";
    }

    if (prefix == "+") {
        return CreateYPathChild(GetChildCount(), tailPath, producer, builder);
    } else if (prefix == "-") {
        return CreateYPathChild(0, tailPath, producer, builder);
    }

    char lastPrefixCh = prefix[prefix.length() - 1];
    TStringBuf indexString =
        lastPrefixCh == '+' || lastPrefixCh == '-'
        ? TStringBuf(prefix.begin() + 1, prefix.end())
        : prefix;

    int index;
    try {
        index = FromString<int>(indexString);
    } catch (...) {
        throw TYTreeException() << Sprintf("Failed to parse child index %s",
            ~Stroka(indexString).Quote());
    }

    if (lastPrefixCh == '+') {
        return CreateYPathChild(index + 1, tailPath, producer, builder);
    } else if (lastPrefixCh == '-') {
        return CreateYPathChild(index, tailPath, producer, builder);
    } else {
        auto navigateResult = GetYPathChild(index, tailPath);
        YASSERT(navigateResult.Code == IYPathService::ECode::Recurse);
        return IYPathService::TSetResult::CreateRecurse(navigateResult.RecurseService, navigateResult.RecursePath);
    }
}

IYPathService::TSetResult TListNodeMixin::CreateYPathChild(
    int beforeIndex,
    TYPath tailPath,
    TYsonProducer* producer,
    ITreeBuilder* builder)
{
    if (tailPath.empty()) {
        builder->BeginTree();
        producer->Do(builder);
        auto newChild = builder->EndTree();
        AddChild(newChild, beforeIndex);
        return IYPathService::TSetResult::CreateDone(newChild);
    } else {
        auto newChild = GetFactory()->CreateMap();
        AddChild(newChild, beforeIndex);
        return IYPathService::TSetResult::CreateRecurse(IYPathService::FromNode(~newChild), tailPath);
    }
}

IYPathService::TNavigateResult TListNodeMixin::GetYPathChild(
    int index,
    TYPath tailPath) const
{
    int count = GetChildCount();
    if (count == 0) {
        throw TYTreeException() << "List is empty";
    }

    if (index < 0 || index >= count) {
        throw TYTreeException() << Sprintf("Invalid child index %d, expecting value in range 0..%d",
            index,
            count - 1);
    }

    auto child = FindChild(index);
    return IYPathService::TNavigateResult::CreateRecurse(IYPathService::FromNode(~child), tailPath);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYTree
} // namespace NYT

