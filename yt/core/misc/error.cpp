#include "stdafx.h"
#include "error.h"
#include "address.h"
#include "serialize.h"

#include <core/misc/error.pb.h>

#include <core/ytree/convert.h>
#include <core/ytree/fluent.h>

#include <core/yson/tokenizer.h>

#include <core/tracing/trace_context.h>

#include <core/concurrency/scheduler.h>

#include <util/system/error.h>

namespace NYT {

using namespace NYTree;
using namespace NYson;

////////////////////////////////////////////////////////////////////////////////

void TErrorCode::Save(TStreamSaveContext& context) const
{
    NYT::Save(context, Value_);
}

void TErrorCode::Load(TStreamLoadContext& context)
{
    NYT::Load(context, Value_);
}

////////////////////////////////////////////////////////////////////////////////

TError::TErrorOr()
    : Code_(NYT::EErrorCode::OK)
{ }

TError::TErrorOr(const TError& other)
    : Code_(other.Code_)
    , Message_(other.Message_)
    , Attributes_(other.Attributes_ ? other.Attributes_->Clone() : nullptr)
    , InnerErrors_(other.InnerErrors_)
{ }

TError::TErrorOr(TError&& other) noexcept
    : Code_(other.Code_)
    , Message_(std::move(other.Message_))
    , Attributes_(std::move(other.Attributes_))
    , InnerErrors_(std::move(other.InnerErrors_))
{ }

TError::TErrorOr(const std::exception& ex)
{
    const auto* errorEx = dynamic_cast<const TErrorException*>(&ex);
    if (errorEx) {
        *this = errorEx->Error();
    } else {
        Code_ = NYT::EErrorCode::Generic;
        Message_ = ex.what();
    }
}

TError::TErrorOr(const Stroka& message)
    : Code_(NYT::EErrorCode::Generic)
    , Message_(message)
{
    CaptureOriginAttributes();
}

TError::TErrorOr(TErrorCode code, const Stroka& message)
    : Code_(code)
    , Message_(message)
{
    if (!IsOK()) {
        CaptureOriginAttributes();
    }
}

TError& TError::operator = (const TError& other)
{
    if (this != &other) {
        Code_ = other.Code_;
        Message_ = other.Message_;
        Attributes_ = other.Attributes_ ? other.Attributes_->Clone() : nullptr;
        InnerErrors_ = other.InnerErrors_;
    }
    return *this;
}

TError& TError::operator = (TError&& other) noexcept
{
    Code_ = other.Code_;
    Message_ = std::move(other.Message_);
    Attributes_ = std::move(other.Attributes_);
    InnerErrors_ = std::move(other.InnerErrors_);
    return *this;
}

TError TError::FromSystem()
{
    return FromSystem(LastSystemError());
}

TError TError::FromSystem(int error)
{
    return TError("%v", LastSystemErrorText(error)) <<
        TErrorAttribute("errno", error);
}

TErrorCode TError::GetCode() const
{
    return Code_;
}

TError& TError::SetCode(TErrorCode code)
{
    Code_ = code;
    return *this;
}

const Stroka& TError::GetMessage() const
{
    return Message_;
}

TError& TError::SetMessage(const Stroka& message)
{
    Message_ = message;
    return *this;
}

const IAttributeDictionary& TError::Attributes() const
{
    return Attributes_ ? *Attributes_ : EmptyAttributes();
}

IAttributeDictionary& TError::Attributes()
{
    if (!Attributes_) {
        Attributes_ = CreateEphemeralAttributes();
    }
    return *Attributes_;
}

const std::vector<TError>& TError::InnerErrors() const
{
    return InnerErrors_;
}

std::vector<TError>& TError::InnerErrors()
{
    return InnerErrors_;
}

TError TError::Sanitize() const
{
    TError result;
    result.Code_ = Code_;
    result.Message_ = Message_;
    if (Attributes_) {
        // Cf. CaptureOriginAttributes.
        auto attributes = Attributes_->Clone();
        attributes->Remove("host");
        attributes->Remove("datetime");
        attributes->Remove("pid");
        attributes->Remove("tid");
        attributes->Remove("fid");
        attributes->Remove("trace_id");
        attributes->Remove("span_id");
        result.Attributes_ = std::move(attributes);
    }

    for (auto& innerError : InnerErrors_) {
        result.InnerErrors_.push_back(innerError.Sanitize());
    }

    return result;
}

bool TError::IsOK() const
{
    return Code_ == NYT::EErrorCode::OK;
}

void TError::ThrowOnError() const
{
    if (!IsOK()) {
        THROW_ERROR *this;
    }
}

TError TError::Wrap() const
{
    return *this;
}

void TErrorOr<void>::Save(TStreamSaveContext& context) const
{
    using NYT::Save;
    Save(context, Code_);
    Save(context, Message_);
    Save(context, Attributes_);
    Save(context, InnerErrors_);
}

void TErrorOr<void>::Load(TStreamLoadContext& context)
{
    using NYT::Load;
    Load(context, Code_);
    Load(context, Message_);
    Load(context, Attributes_);
    Load(context, InnerErrors_);
}

void TError::CaptureOriginAttributes()
{
    Attributes().Set("host", TAddressResolver::Get()->GetLocalHostName());
    Attributes().Set("datetime", TInstant::Now());
    Attributes().Set("pid", ::getpid());
    Attributes().Set("tid", NConcurrency::GetCurrentThreadId());
    Attributes().Set("fid", NConcurrency::GetCurrentFiberId());
    auto traceContext = NTracing::GetCurrentTraceContext();
    if (traceContext.IsEnabled()) {
        Attributes().SetYson("trace_id", ConvertToYsonString(traceContext.GetTraceId()));
        Attributes().SetYson("span_id", ConvertToYsonString(traceContext.GetSpanId()));
    }
}

TNullable<TError> TError::FindMatching(TErrorCode code) const
{
    if (Code_ == code) {
        return *this;
    }

    for (const auto& innerError : InnerErrors_) {
        auto innerResult = innerError.FindMatching(code);
        if (innerResult) {
            return std::move(innerResult);
        }
    }

    return Null;
}

////////////////////////////////////////////////////////////////////////////////

namespace {

void AppendIndent(TStringBuilder* builer, int indent)
{
    builer->AppendChar(' ', indent);
}

void AppendAttribute(TStringBuilder* builder, const Stroka& key, const Stroka& value, int indent)
{
    AppendIndent(builder, indent + 4);
    builder->AppendFormat("%-15s %s", key, value);
    builder->AppendChar('\n');
}

void AppendError(TStringBuilder* builder, const TError& error, int indent)
{
    if (error.IsOK()) {
        builder->AppendString("OK");
        return;
    }

    AppendIndent(builder, indent);
    builder->AppendString(error.GetMessage());
    builder->AppendChar('\n');

    if (error.GetCode() != NYT::EErrorCode::Generic) {
        AppendAttribute(builder, "code", ToString(static_cast<int>(error.GetCode())), indent);
    }

    // Pretty-print origin.
    auto host = error.Attributes().Find<Stroka>("host");
    auto datetime = error.Attributes().Find<Stroka>("datetime");
    auto pid = error.Attributes().Find<pid_t>("pid");
    auto tid = error.Attributes().Find<NConcurrency::TThreadId>("tid");
    auto fid = error.Attributes().Find<NConcurrency::TFiberId>("fid");
    if (host && datetime && pid && tid && fid) {
        AppendAttribute(
            builder,
            "origin",
            Format("%v on %v (pid %v, tid %x, fid %x)",
                *host,
                *datetime,
                *pid,
                *tid,
                *fid),
            indent);
    }

    auto keys = error.Attributes().List();
    for (const auto& key : keys) {
        if (key == "host" ||
            key == "datetime" ||
            key == "pid" ||
            key == "tid" ||
            key == "fid")
            continue;

        auto value = error.Attributes().GetYson(key);
        TTokenizer tokenizer(value.Data());
        YCHECK(tokenizer.ParseNext());
        switch (tokenizer.GetCurrentType()) {
            case ETokenType::String:
                AppendAttribute(builder, key, Stroka(tokenizer.CurrentToken().GetStringValue()), indent);
                break;
            case ETokenType::Int64:
                AppendAttribute(builder, key, ToString(tokenizer.CurrentToken().GetInt64Value()), indent);
                break;
            case ETokenType::Uint64:
                AppendAttribute(builder, key, ToString(tokenizer.CurrentToken().GetUint64Value()), indent);
                break;
            case ETokenType::Double:
                AppendAttribute(builder, key, ToString(tokenizer.CurrentToken().GetDoubleValue()), indent);
                break;
            case ETokenType::Boolean:
                AppendAttribute(builder, key, Stroka(FormatBool(tokenizer.CurrentToken().GetBooleanValue())), indent);
                break;
            default:
                AppendAttribute(builder, key, ConvertToYsonString(value, EYsonFormat::Text).Data(), indent);
                break;
        }
    }

    for (const auto& innerError : error.InnerErrors()) {
        builder->AppendChar('\n');
        AppendError(builder, innerError, indent + 2);
    }
}

} // namespace

Stroka ToString(const TError& error)
{
    TStringBuilder builder;
    AppendError(&builder, error, 0);
    return builder.Flush();
}

void ToProto(NYT::NProto::TError* protoError, const TError& error)
{
    protoError->set_code(error.GetCode());

    if (!error.GetMessage().empty()) {
        protoError->set_message(error.GetMessage());
    } else {
        protoError->clear_message();
    }

    if (!error.Attributes().List().empty()) {
        ToProto(protoError->mutable_attributes(), error.Attributes());
    } else {
        protoError->clear_attributes();
    }

    protoError->clear_inner_errors();
    for (const auto& innerError : error.InnerErrors()) {
        ToProto(protoError->add_inner_errors(), innerError);
    }
}

void FromProto(TError* error, const NYT::NProto::TError& protoError)
{
    *error = TError(
        protoError.code(),
        protoError.has_message() ? protoError.message() : "");

    if (protoError.has_attributes()) {
        error->Attributes().MergeFrom(*FromProto(protoError.attributes()));
    }

    error->InnerErrors() = FromProto<TError>(protoError.inner_errors());
}

void Serialize(const TError& error, NYson::IYsonConsumer* consumer, const std::function<void(NYson::IYsonConsumer*)>* valueProducer)
{
    BuildYsonFluently(consumer)
        .BeginMap()
            .Item("code").Value(error.GetCode())
            .Item("message").Value(error.GetMessage())
            .Item("attributes").DoMapFor(error.Attributes().List(), [&] (TFluentMap fluent, const Stroka& key) {
                fluent
                    .Item(key).Value(error.Attributes().GetYson(key));
            })
            .DoIf(!error.InnerErrors().empty(), [&] (TFluentMap fluent) {
                fluent
                    .Item("inner_errors").DoListFor(error.InnerErrors(), [=] (TFluentList fluent, const TError& innerError) {
                        fluent
                            .Item().Value(innerError);
                    });
            })
            .DoIf(valueProducer != nullptr, [=] (TFluentMap fluent) {
                fluent
                    .Item("value").Do(BIND(*valueProducer));
            })
        .EndMap();
}

void Deserialize(TError& error, NYTree::INodePtr node)
{
    auto mapNode = node->AsMap();

    error = TError(
        mapNode->GetChild("code")->GetValue<i64>(),
        mapNode->GetChild("message")->GetValue<Stroka>());

    error.Attributes().Clear();
    auto attributesNode = mapNode->FindChild("attributes");
    if (attributesNode) {
        error.Attributes().MergeFrom(attributesNode->AsMap());
    }

    error.InnerErrors().clear();
    auto innerErrorsNode = mapNode->FindChild("inner_errors");
    if (innerErrorsNode) {
        for (auto innerErrorNode : innerErrorsNode->AsList()->GetChildren()) {
            auto innerError = ConvertTo<TError>(innerErrorNode);
            error.InnerErrors().push_back(innerError);
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

TError operator << (TError error, const TErrorAttribute& attribute)
{
    error.Attributes().SetYson(attribute.Key, attribute.Value);
    return error;
}

TError operator << (TError error, const TError& innerError)
{
    error.InnerErrors().push_back(innerError);
    return error;
}

TError operator << (TError error, const std::vector<TError>& innerErrors)
{
    error.InnerErrors().insert(
        error.InnerErrors().end(),
        innerErrors.begin(),
        innerErrors.end());
    return error;
}

TError operator << (TError error, std::unique_ptr<NYTree::IAttributeDictionary> attributes)
{
    for (const auto& key : attributes->List()) {
        error.Attributes().SetYson(key, attributes->GetYson(key));
    }
    return error;
}

////////////////////////////////////////////////////////////////////////////////

const char* TErrorException::what() const throw()
{
    if (CachedWhat_.empty()) {
        CachedWhat_ = ToString(Error_);
    }
    return ~CachedWhat_;
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
