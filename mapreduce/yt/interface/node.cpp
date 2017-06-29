#include "node.h"

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

bool TNode::TEntity::operator==(const TEntity&) const {
    return true;
}

////////////////////////////////////////////////////////////////////////////////

bool TNode::TUndefined::operator==(const TUndefined&) const {
    return true;
}

////////////////////////////////////////////////////////////////////////////////

TNode::TNode()
    : Value_(TVariantTypeTag<TUndefined>())
{ }

TNode::TNode(const char* s)
    : Value_(TVariantTypeTag<TString>(), TString(s))
{ }

TNode::TNode(const TStringBuf& s)
    : Value_(TVariantTypeTag<TString>(), TString(s))
{ }

TNode::TNode(TString s)
    : Value_(TVariantTypeTag<TString>(), std::move(s))
{ }

TNode::TNode(int i)
    : Value_(TVariantTypeTag<i64>(), i)
{ }


TNode::TNode(unsigned int ui)
    : Value_(TVariantTypeTag<ui64>(), ui)
{ }

TNode::TNode(long i)
    : Value_(TVariantTypeTag<i64>(), i)
{ }

TNode::TNode(unsigned long ui)
    : Value_(TVariantTypeTag<ui64>(), ui)
{ }

TNode::TNode(long long i)
    : Value_(TVariantTypeTag<i64>(), i)
{ }

TNode::TNode(unsigned long long ui)
    : Value_(TVariantTypeTag<ui64>(), ui)
{ }

TNode::TNode(double d)
    : Value_(TVariantTypeTag<double>(), d)
{ }

TNode::TNode(bool b)
    : Value_(TVariantTypeTag<bool>(), b)
{ }

TNode::TNode(const TNode& rhs)
    : TNode()
{
    Copy(rhs);
}

TNode& TNode::operator=(const TNode& rhs)
{
    if (this != &rhs) {
        Clear();
        Copy(rhs);
    }
    return *this;
}

TNode::TNode(TNode&& rhs)
    : TNode()
{
    if (this != &rhs) {
        Move(std::move(rhs));
    }
}

TNode& TNode::operator=(TNode&& rhs)
{
    if (this != &rhs) {
        Move(std::move(rhs));
    }
    return *this;
}

TNode::~TNode() = default;

void TNode::Clear()
{
    ClearAttributes();
    Value_ = TUndefined();
}

bool TNode::IsString() const
{
    return Value_.Is<TString>();
}

bool TNode::IsInt64() const
{
    return Value_.Is<i64>();
}

bool TNode::IsUint64() const
{
    return Value_.Is<ui64>();
}

bool TNode::IsDouble() const
{
    return Value_.Is<double>();
}

bool TNode::IsBool() const
{
    return Value_.Is<bool>();
}

bool TNode::IsList() const
{
    return Value_.Is<TList>();
}

bool TNode::IsMap() const
{
    return Value_.Is<TMap>();
}

bool TNode::IsEntity() const
{
    return Value_.Is<TEntity>();
}

bool TNode::IsUndefined() const
{
    return Value_.Is<TUndefined>();
}

bool TNode::Empty() const
{
    switch (GetType()) {
        case STRING:
            return Value_.As<TString>().empty();
        case LIST:
            return Value_.As<TList>().empty();
        case MAP:
            return Value_.As<TMap>().empty();
        default:
            ythrow TTypeError() << "Empty() called for type " << TypeToString(GetType());
    }
}

size_t TNode::Size() const
{
    switch (GetType()) {
        case STRING:
            return Value_.As<TString>().size();
        case LIST:
            return Value_.As<TList>().size();
        case MAP:
            return Value_.As<TMap>().size();
        default:
            ythrow TTypeError() << "Size() called for type " << TypeToString(GetType());
    }
}

TNode::EType TNode::GetType() const
{
    switch (Value_.Tag()) {
        case TValue::TagOf<TUndefined>():
            return UNDEFINED;
        case TValue::TagOf<TString>():
            return STRING;
        case TValue::TagOf<i64>():
            return INT64;
        case TValue::TagOf<ui64>():
            return UINT64;
        case TValue::TagOf<double>():
            return DOUBLE;
        case TValue::TagOf<bool>():
            return BOOL;
        case TValue::TagOf<TList>():
            return LIST;
        case TValue::TagOf<TMap>():
            return MAP;
        case TValue::TagOf<TEntity>():
            return ENTITY;
    }
    Y_UNREACHABLE();
}

const TString& TNode::AsString() const
{
    CheckType(STRING);
    return Value_.As<TString>();
}

i64 TNode::AsInt64() const
{
    CheckType(INT64);
    return Value_.As<i64>();
}

ui64 TNode::AsUint64() const
{
    CheckType(UINT64);
    return Value_.As<ui64>();
}

double TNode::AsDouble() const
{
    CheckType(DOUBLE);
    return Value_.As<double>();
}

bool TNode::AsBool() const
{
    CheckType(BOOL);
    return Value_.As<bool>();
}

const TNode::TList& TNode::AsList() const
{
    CheckType(LIST);
    return Value_.As<TList>();
}

const TNode::TMap& TNode::AsMap() const
{
    CheckType(MAP);
    return Value_.As<TMap>();
}

TNode::TList& TNode::AsList()
{
    CheckType(LIST);
    return Value_.As<TList>();
}

TNode::TMap& TNode::AsMap()
{
    CheckType(MAP);
    return Value_.As<TMap>();
}

const TString& TNode::UncheckedAsString() const noexcept
{
    return Value_.As<TString>();
}

i64 TNode::UncheckedAsInt64() const noexcept
{
    return Value_.As<i64>();
}

ui64 TNode::UncheckedAsUint64() const noexcept
{
    return Value_.As<ui64>();
}

double TNode::UncheckedAsDouble() const noexcept
{
    return Value_.As<double>();
}

bool TNode::UncheckedAsBool() const noexcept
{
    return Value_.As<bool>();
}

const TNode::TList& TNode::UncheckedAsList() const noexcept
{
    return Value_.As<TList>();
}

const TNode::TMap& TNode::UncheckedAsMap() const noexcept
{
    return Value_.As<TMap>();
}

TNode::TList& TNode::UncheckedAsList() noexcept
{
    return Value_.As<TList>();
}

TNode::TMap& TNode::UncheckedAsMap() noexcept
{
    return Value_.As<TMap>();
}

TNode TNode::CreateList()
{
    TNode node;
    node.Value_ = TValue(TVariantTypeTag<TList>());
    return node;
}

TNode TNode::CreateMap()
{
    TNode node;
    node.Value_ = TValue(TVariantTypeTag<TMap>());
    return node;
}

TNode TNode::CreateEntity()
{
    TNode node;
    node.Value_ = TValue(TVariantTypeTag<TEntity>());
    return node;
}

const TNode& TNode::operator[](size_t index) const
{
    CheckType(LIST);
    return Value_.As<TList>()[index];
}

TNode& TNode::operator[](size_t index)
{
    CheckType(LIST);
    return Value_.As<TList>()[index];
}

TNode& TNode::Add()
{
    AssureList();
    return Value_.As<TList>().emplace_back();
}

TNode& TNode::Add(const TNode& node)
{
    AssureList();
    Value_.As<TList>().emplace_back(node);
    return *this;
}

TNode& TNode::Add(TNode&& node)
{
    AssureList();
    Value_.As<TList>().emplace_back(std::move(node));
    return *this;
}

bool TNode::HasKey(const TStringBuf key) const
{
    CheckType(MAP);
    return Value_.As<TMap>().has(key);
}

TNode& TNode::operator()(const TString& key, const TNode& value)
{
    AssureMap();
    Value_.As<TMap>()[key] = value;
    return *this;
}

TNode& TNode::operator()(const TString& key, TNode&& value)
{
    AssureMap();
    Value_.As<TMap>()[key] = std::move(value);
    return *this;
}

const TNode& TNode::operator[](const TStringBuf key) const
{
    CheckType(MAP);
    static TNode notFound;
    const auto& map = Value_.As<TMap>();
    TMap::const_iterator i = map.find(key);
    if (i == map.end()) {
        return notFound;
    } else {
        return i->second;
    }
}

TNode& TNode::operator[](const TStringBuf key)
{
    AssureMap();
    return Value_.As<TMap>()[key];
}

bool TNode::HasAttributes() const
{
    return Attributes_ && !Attributes_->Empty();
}

void TNode::ClearAttributes()
{
    if (Attributes_) {
        Attributes_.Destroy();
    }
}

const TNode& TNode::GetAttributes() const
{
    static TNode notFound = TNode::CreateMap();
    if (!Attributes_) {
        return notFound;
    }
    return *Attributes_;
}

TNode& TNode::Attributes()
{
    if (!Attributes_) {
        CreateAttributes();
    }
    return *Attributes_;
}

void TNode::MoveWithoutAttributes(TNode&& rhs)
{
    Value_ = std::move(rhs.Value_);
    rhs.Clear();
}

const TString& TNode::TypeToString(EType type)
{
    static TString typeNames[] = {
        "UNDEFINED",
        "STRING",
        "INT64",
        "UINT64",
        "DOUBLE",
        "BOOL",
        "LIST",
        "MAP",
        "ENTITY"
    };
    return typeNames[type];
}

void TNode::Copy(const TNode& rhs)
{
    if (rhs.Attributes_) {
        if (!Attributes_) {
            CreateAttributes();
        }
        *Attributes_ = *rhs.Attributes_;
    }

    Value_ = rhs.Value_;
}

void TNode::Move(TNode&& rhs)
{
    Value_ = std::move(rhs.Value_);
    Attributes_ = std::move(rhs.Attributes_);
}

void TNode::CheckType(EType type) const
{
    Y_ENSURE_EX(GetType() == type,
        TTypeError() << "TNode type " << TypeToString(type) <<  " expected, actual type " << TypeToString(GetType());
    );
}

void TNode::AssureMap()
{
    if (Value_.Is<TUndefined>()) {
        Value_ = TMap();
    } else {
        CheckType(MAP);
    }
}

void TNode::AssureList()
{
    if (Value_.Is<TUndefined>()) {
        Value_ = TList();
    } else {
        CheckType(LIST);
    }
}

void TNode::CreateAttributes()
{
    Attributes_ = new TNode;
    Attributes_->Value_ = TMap();
}

////////////////////////////////////////////////////////////////////////////////

bool operator==(const TNode& lhs, const TNode& rhs)
{
    if (lhs.Value_.Is<TNode::TUndefined>() ||
        rhs.Value_.Is<TNode::TUndefined>())
    {
        // TODO: should try to remove this behaviour if nobody uses it.
        return false;
    }

    if (lhs.GetType() != rhs.GetType()) {
        return false;
    }

    if (lhs.Attributes_) {
        if (rhs.Attributes_) {
            if (*lhs.Attributes_ != *rhs.Attributes_) {
                return false;
            }
        } else {
            return false;
        }
    } else {
        if (rhs.Attributes_) {
            return false;
        }
    }

    return rhs.Value_ == lhs.Value_;
}

bool operator!=(const TNode& lhs, const TNode& rhs)
{
    return !(lhs == rhs);
}

bool GetBool(const TNode& node)
{
    if (node.IsBool()) {
        return node.AsBool();
    } else if (node.IsString()) {
        return node.AsString() == "true";
    } else {
        ythrow TNode::TTypeError()
            << "GetBool(): not a boolean or string type";
    }
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
