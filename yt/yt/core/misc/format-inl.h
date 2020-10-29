#pragma once
#ifndef FORMAT_INL_H_
#error "Direct inclusion of this file is not allowed, include format.h"
// For the sake of sane code completion.
#include "format.h"
#endif

#include "mpl.h"
#include "string.h"
#include "optional.h"
#include "enum.h"
#include "assert.h"

#include <cctype>

namespace NYT {

////////////////////////////////////////////////////////////////////////////////

static const char GenericSpecSymbol = 'v';
static const char Int2Hex[] = "0123456789abcdef";

inline bool IsQuotationSpecSymbol(char symbol)
{
    return symbol == 'Q' || symbol == 'q';
}

// __int128
[[deprecated("Do not use this method")]]
void FormatValue(TStringBuilderBase* builder, __int128 value, TStringBuf format);

// TStringBuf
inline void FormatValue(TStringBuilderBase* builder, TStringBuf value, TStringBuf format)
{
    if (!format) {
        builder->AppendString(value);
        return;
    }

    // Parse alignment.
    bool alignLeft = false;
    const char* current = format.begin();
    if (*current == '-') {
        alignLeft = true;
        ++current;
    }

    bool hasAlign = false;
    int alignSize = 0;
    while (*current >= '0' && *current <= '9') {
        hasAlign = true;
        alignSize = 10 * alignSize + (*current - '0');
        if (alignSize > 1000000) {
            builder->AppendString(TStringBuf("<alignment overflow>"));
            return;
        }
        ++current;
    }

    int padding = 0;
    bool padLeft = false;
    bool padRight = false;
    if (hasAlign) {
        padding = alignSize - value.size();
        if (padding < 0) {
            padding = 0;
        }
        padLeft = !alignLeft;
        padRight = alignLeft;
    }

    bool singleQuotes = false;
    bool doubleQuotes = false;
    while (current < format.end()) {
        if (*current == 'q') {
            singleQuotes = true;
        } else if (*current == 'Q') {
            doubleQuotes = true;
        }
        ++current;
    }

    if (padLeft) {
        builder->AppendChar(' ', padding);
    }

    if (singleQuotes || doubleQuotes) {
        for (const char* valueCurrent = value.begin(); valueCurrent < value.end(); ++valueCurrent) {
            char ch = *valueCurrent;
            if (!std::isprint(ch) && !std::isspace(ch)) {
                builder->AppendString("\\x");
                builder->AppendChar(Int2Hex[static_cast<ui8>(ch) >> 4]);
                builder->AppendChar(Int2Hex[static_cast<ui8>(ch) & 0xf]);
            } else if ((singleQuotes && ch == '\'') || (doubleQuotes && ch == '\"')) {
                builder->AppendChar('\\');
                builder->AppendChar(ch);
            } else {
                builder->AppendChar(ch);
            }
        }
    } else {
        builder->AppendString(value);
    }

    if (padRight) {
        builder->AppendChar(' ', padding);
    }
}

// TString
inline void FormatValue(TStringBuilderBase* builder, const TString& value, TStringBuf format)
{
    FormatValue(builder, TStringBuf(value), format);
}

// const char*
inline void FormatValue(TStringBuilderBase* builder, const char* value, TStringBuf format)
{
    FormatValue(builder, TStringBuf(value), format);
}

// char
inline void FormatValue(TStringBuilderBase* builder, char value, TStringBuf format)
{
    FormatValue(builder, TStringBuf(&value, 1), format);
}

// bool
inline void FormatValue(TStringBuilderBase* builder, bool value, TStringBuf format)
{
    // Parse custom flags.
    bool lowercase = false;
    const char* current = format.begin();
    while (current != format.end()) {
        if (*current == 'l') {
            ++current;
            lowercase = true;
        } else if (IsQuotationSpecSymbol(*current)) {
            ++current;
        } else
            break;
    }

    auto str = lowercase
        ? (value ? TStringBuf("true") : TStringBuf("false"))
        : (value ? TStringBuf("True") : TStringBuf("False"));

    builder->AppendString(str);
}

// Fallback to ToString
struct TToStringFallbackValueFormatterTag
{ };

template <class TValue, class = void>
struct TValueFormatter
{
    static TToStringFallbackValueFormatterTag Do(TStringBuilderBase* builder, const TValue& value, TStringBuf format)
    {
        using ::ToString;
        FormatValue(builder, ToString(value), format);
        return {};
    }
};

// Enum
template <class TEnum>
struct TValueFormatter<TEnum, typename std::enable_if<TEnumTraits<TEnum>::IsEnum>::type>
{
    static void Do(TStringBuilderBase* builder, TEnum value, TStringBuf format)
    {
        // Parse custom flags.
        bool lowercase = false;
        const char* current = format.begin();
        while (current != format.end()) {
            if (*current == 'l') {
                ++current;
                lowercase = true;
            } else if (IsQuotationSpecSymbol(*current)) {
                ++current;
            } else {
                break;
            }
        }

        FormatEnum(builder, value, lowercase);
    }
};

template <class TRange, class TFormatter>
typename TFormattableView<TRange, TFormatter>::TBegin TFormattableView<TRange, TFormatter>::begin() const
{
    return RangeBegin;
}

template <class TRange, class TFormatter>
typename TFormattableView<TRange, TFormatter>::TEnd TFormattableView<TRange, TFormatter>::end() const
{
    return RangeEnd;
}

template <class TRange, class TFormatter>
TFormattableView<TRange, TFormatter> MakeFormattableView(
    const TRange& range,
    TFormatter&& formatter)
{
    return TFormattableView<TRange, std::decay_t<TFormatter>>{range.begin(), range.end(), std::forward<TFormatter>(formatter)};
}

template <class TRange, class TFormatter>
TFormattableView<TRange, TFormatter> MakeShrunkFormattableView(
    const TRange& range,
    TFormatter&& formatter,
    size_t limit)
{
    return TFormattableView<TRange, std::decay_t<TFormatter>>{range.begin(), range.end(), std::forward<TFormatter>(formatter), limit};
}

template <class TRange, class TFormatter>
void FormatRange(TStringBuilderBase* builder, const TRange& range, const TFormatter& formatter, size_t limit = std::numeric_limits<size_t>::max())
{
    builder->AppendChar('[');
    size_t index = 0;
    for (const auto& item : range) {
        if (index > 0) {
            builder->AppendString(DefaultJoinToStringDelimiter);
        }
        if (index == limit) {
            builder->AppendString(DefaultRangeEllipsisFormat);
            break;
        }
        formatter(builder, item);
        ++index;
    }
    builder->AppendChar(']');
}

template <class TRange, class TFormatter>
void FormatKeyValueRange(TStringBuilderBase* builder, const TRange& range, const TFormatter& formatter, size_t limit = std::numeric_limits<size_t>::max())
{
    builder->AppendChar('{');
    size_t index = 0;
    for (const auto& item : range) {
        if (index > 0) {
            builder->AppendString(DefaultJoinToStringDelimiter);
        }
        if (index == limit) {
            builder->AppendString(DefaultRangeEllipsisFormat);
            break;
        }
        formatter(builder, item.first);
        builder->AppendString(DefaultKeyValueDelimiter);
        formatter(builder, item.second);
        ++index;
    }
    builder->AppendChar('}');
}

// TFormattableView
template <class TRange, class TFormatter>
struct TValueFormatter<TFormattableView<TRange, TFormatter>>
{
    static void Do(TStringBuilderBase* builder, const TFormattableView<TRange, TFormatter>& range, TStringBuf /*format*/)
    {
        FormatRange(builder, range, range.Formatter, range.Limit);
    }
};

template <class TFormatter>
TFormatterWrapper<TFormatter> MakeFormatterWrapper(
    TFormatter&& formatter)
{
    return TFormatterWrapper<TFormatter>{
        .Formatter = std::move(formatter)
    };
}

// TFormatterWrapper
template <class TFormatter>
struct TValueFormatter<TFormatterWrapper<TFormatter>>
{
    static void Do(TStringBuilderBase* builder, const TFormatterWrapper<TFormatter>& wrapper, TStringBuf /*format*/)
    {
        wrapper.Formatter(builder);
    }
};

// std::vector
template <class T>
struct TValueFormatter<std::vector<T>>
{
    static void Do(TStringBuilderBase* builder, const std::vector<T>& collection, TStringBuf /*format*/)
    {
        FormatRange(builder, collection, TDefaultFormatter());
    }
};

// SmallVector
template <class T, unsigned N>
struct TValueFormatter<SmallVector<T, N>>
{
    static void Do(TStringBuilderBase* builder, const SmallVector<T, N>& collection, TStringBuf /*format*/)
    {
        FormatRange(builder, collection, TDefaultFormatter());
    }
};

// std::set
template <class T>
struct TValueFormatter<std::set<T>>
{
    static void Do(TStringBuilderBase* builder, const std::set<T>& collection, TStringBuf /*format*/)
    {
        FormatRange(builder, collection, TDefaultFormatter());
    }
};

// std::map
template <class K, class V>
struct TValueFormatter<std::map<K, V>>
{
    static void Do(TStringBuilderBase* builder, const std::map<K, V>& collection, TStringBuf /*format*/)
    {
        FormatKeyValueRange(builder, collection, TDefaultFormatter());
    }
};

// std::multimap
template <class K, class V>
struct TValueFormatter<std::multimap<K, V>>
{
    static void Do(TStringBuilderBase* builder, const std::multimap<K, V>& collection, TStringBuf /*format*/)
    {
        FormatKeyValueRange(builder, collection, TDefaultFormatter());
    }
};

// THashSet
template <class T>
struct TValueFormatter<THashSet<T>>
{
    static void Do(TStringBuilderBase* builder, const THashSet<T>& collection, TStringBuf /*format*/)
    {
        FormatRange(builder, collection, TDefaultFormatter());
    }
};

// THashMultiSet
template <class T>
struct TValueFormatter<THashMultiSet<T>>
{
    static void Do(TStringBuilderBase* builder, const THashMultiSet<T>& collection, TStringBuf /*format*/)
    {
        FormatRange(builder, collection, TDefaultFormatter());
    }
};

// THashMap
template <class K, class V>
struct TValueFormatter<THashMap<K, V>>
{
    static void Do(TStringBuilderBase* builder, const THashMap<K, V>& collection, TStringBuf /*format*/)
    {
        FormatKeyValueRange(builder, collection, TDefaultFormatter());
    }
};

// THashMultiMap
template <class K, class V>
struct TValueFormatter<THashMultiMap<K, V>>
{
    static void Do(TStringBuilderBase* builder, const THashMultiMap<K, V>& collection, TStringBuf /*format*/)
    {
        FormatKeyValueRange(builder, collection, TDefaultFormatter());
    }
};

// TEnumIndexedVector
template <class E, class T>
struct TValueFormatter<TEnumIndexedVector<E, T>>
{
    static void Do(TStringBuilderBase* builder, const TEnumIndexedVector<E, T>& collection, TStringBuf format)
    {
        builder->AppendChar('{');
        bool firstItem = true;
        for (const auto& index : TEnumTraits<E>::GetDomainValues()) {
            if (!firstItem) {
                builder->AppendString(DefaultJoinToStringDelimiter);
            }
            FormatValue(builder, index, format);
            builder->AppendString(": ");
            FormatValue(builder, collection[index], format);
            firstItem = false;
        }
        builder->AppendChar('}');
    }
};

// std::pair
template <class T1, class T2>
struct TValueFormatter<std::pair<T1, T2>>
{
    static void Do(TStringBuilderBase* builder, const std::pair<T1, T2>& value, TStringBuf format)
    {
        builder->AppendChar('{');
        FormatValue(builder, value.first, format);
        builder->AppendString(TStringBuf(", "));
        FormatValue(builder, value.second, format);
        builder->AppendChar('}');
    }
};

// std::optional
inline void FormatValue(TStringBuilderBase* builder, std::nullopt_t, TStringBuf /*format*/)
{
    builder->AppendString(TStringBuf("<null>"));
}

template <class T>
struct TValueFormatter<std::optional<T>>
{
    static void Do(TStringBuilderBase* builder, const std::optional<T>& value, TStringBuf format)
    {
        if (value) {
            FormatValue(builder, *value, format);
        } else {
            FormatValue(builder, std::nullopt, format);
        }
    }
};

template <class TValue>
auto FormatValue(TStringBuilderBase* builder, const TValue& value, TStringBuf format) ->
    decltype(TValueFormatter<TValue>::Do(builder, value, format))
{
    return TValueFormatter<TValue>::Do(builder, value, format);
}

template <class TValue>
void FormatValueViaSprintf(
    TStringBuilderBase* builder,
    TValue value,
    TStringBuf format,
    TStringBuf genericSpec)
{
    constexpr int MaxFormatSize = 64;
    constexpr int SmallResultSize = 64;

    auto copyFormat = [] (char* destination, const char* source, int length) {
        int position = 0;
        for (int index = 0; index < length; ++index) {
            if (IsQuotationSpecSymbol(source[index])) {
                continue;
            }
            destination[position] = source[index];
            ++position;
        }
        return destination + position;
    };

    char formatBuf[MaxFormatSize];
    YT_VERIFY(format.length() >= 1 && format.length() <= MaxFormatSize - 2); // one for %, one for \0
    formatBuf[0] = '%';
    if (format[format.length() - 1] == GenericSpecSymbol) {
        char* formatEnd = copyFormat(formatBuf + 1, format.begin(), format.length() - 1);
        ::memcpy(formatEnd, genericSpec.begin(), genericSpec.length());
        formatEnd[genericSpec.length()] = '\0';
    } else {
        char* formatEnd = copyFormat(formatBuf + 1, format.begin(), format.length());
        *formatEnd = '\0';
    }

    char* result = builder->Preallocate(SmallResultSize);
    size_t resultSize = ::snprintf(result, SmallResultSize, formatBuf, value);
    if (resultSize >= SmallResultSize) {
        result = builder->Preallocate(resultSize + 1);
        YT_VERIFY(::snprintf(result, resultSize + 1, formatBuf, value) == static_cast<int>(resultSize));
    }
    builder->Advance(resultSize);
}

template <class TValue>
char* WriteIntToBufferBackwards(char* buffer, TValue value);

template <class TValue>
void FormatValueViaHelper(TStringBuilderBase* builder, TValue value, TStringBuf format, TStringBuf genericSpec)
{
    if (format == TStringBuf("v")) {
        const int MaxResultSize = 64;
        char buffer[MaxResultSize];
        char* end = buffer + MaxResultSize;
        char* start = WriteIntToBufferBackwards(end, value);
        builder->AppendString(TStringBuf(start, end));
    } else {
        FormatValueViaSprintf(builder, value, format, genericSpec);
    }
}

#define XX(valueType, castType, genericSpec) \
    inline void FormatValue(TStringBuilderBase* builder, valueType value, TStringBuf format) \
    { \
        FormatValueViaHelper(builder, static_cast<castType>(value), format, genericSpec); \
    }

XX(i8,              int,                TStringBuf("d"))
XX(ui8,             unsigned int,       TStringBuf("u"))
XX(i16,             int,                TStringBuf("d"))
XX(ui16,            unsigned int,       TStringBuf("u"))
XX(i32,             int,                TStringBuf("d"))
XX(ui32,            unsigned int,       TStringBuf("u"))
XX(long,            long,               TStringBuf("ld"))
XX(unsigned long,   unsigned long,      TStringBuf("lu"))

#undef XX

#define XX(valueType, castType, genericSpec) \
    inline void FormatValue(TStringBuilderBase* builder, valueType value, TStringBuf format) \
    { \
        FormatValueViaSprintf(builder, static_cast<castType>(value), format, genericSpec); \
    }

XX(double,          double,             TStringBuf("lf"))
XX(float,           float,              TStringBuf("f"))

#undef XX

// Pointer
template <class T>
void FormatValue(TStringBuilderBase* builder, T* value, TStringBuf format)
{
    FormatValueViaSprintf(builder, value, format, TStringBuf("p"));
}

// TDuration (specialize for performance reasons)
inline void FormatValue(TStringBuilderBase* builder, TDuration value, TStringBuf /*format*/)
{
    builder->AppendFormat("%vus", value.MicroSeconds());
}

// TInstant (specialize for TFormatTraits)
inline void FormatValue(TStringBuilderBase* builder, TInstant value, TStringBuf format)
{
    // TODO(babenko): optimize
    builder->AppendFormat("%v", ToString(value), format);
}

////////////////////////////////////////////////////////////////////////////////

template <class TArgFormatter>
void FormatImpl(
    TStringBuilderBase* builder,
    TStringBuf format,
    const TArgFormatter& argFormatter)
{
    size_t argIndex = 0;
    auto current = format.begin();
    while (true) {
        // Scan verbatim part until stop symbol.
        auto verbatimBegin = current;
        auto verbatimEnd = verbatimBegin;
        while (verbatimEnd != format.end() && *verbatimEnd != '%') {
            ++verbatimEnd;
        }

        // Copy verbatim part, if any.
        size_t verbatimSize = verbatimEnd - verbatimBegin;
        if (verbatimSize > 0) {
            builder->AppendString(TStringBuf(verbatimBegin, verbatimSize));
        }

        // Handle stop symbol.
        current = verbatimEnd;
        if (current == format.end()) {
            break;
        }

        YT_ASSERT(*current == '%');
        ++current;

        if (*current == '%') {
            // Verbatim %.
            builder->AppendChar('%');
            ++current;
        } else {
            // Scan format part until stop symbol.
            auto argFormatBegin = current;
            auto argFormatEnd = argFormatBegin;
            bool singleQuotes = false;
            bool doubleQuotes = false;

            while (
                argFormatEnd != format.end() &&
                *argFormatEnd != GenericSpecSymbol &&     // value in generic format
                *argFormatEnd != 'd' &&                   // others are standard specifiers supported by printf
                *argFormatEnd != 'i' &&
                *argFormatEnd != 'u' &&
                *argFormatEnd != 'o' &&
                *argFormatEnd != 'x' &&
                *argFormatEnd != 'X' &&
                *argFormatEnd != 'f' &&
                *argFormatEnd != 'F' &&
                *argFormatEnd != 'e' &&
                *argFormatEnd != 'E' &&
                *argFormatEnd != 'g' &&
                *argFormatEnd != 'G' &&
                *argFormatEnd != 'a' &&
                *argFormatEnd != 'A' &&
                *argFormatEnd != 'c' &&
                *argFormatEnd != 's' &&
                *argFormatEnd != 'p' &&
                *argFormatEnd != 'n')
            {
                if (*argFormatEnd == 'q') {
                    singleQuotes = true;
                } else if (*argFormatEnd == 'Q') {
                    doubleQuotes = true;
                }
                ++argFormatEnd;
            }

            // Handle end of format string.
            if (argFormatEnd != format.end()) {
                ++argFormatEnd;
            }

            // 'n' means 'nothing'; skip the argument.
            if (*argFormatBegin != 'n') {
                // Format argument.
                TStringBuf argFormat(argFormatBegin, argFormatEnd);
                if (singleQuotes) {
                    builder->AppendChar('\'');
                }
                if (doubleQuotes) {
                    builder->AppendChar('"');
                }
                argFormatter(argIndex++, builder, argFormat);
                if (singleQuotes) {
                    builder->AppendChar('\'');
                }
                if (doubleQuotes) {
                    builder->AppendChar('"');
                }
            }

            current = argFormatEnd;
        }
    }
}

////////////////////////////////////////////////////////////////////////////////

template <class T>
struct TFormatTraits
{
    static constexpr bool HasCustomFormatValue = !std::is_same_v<
        decltype(FormatValue(
            static_cast<TStringBuilderBase*>(nullptr),
            *static_cast<const T*>(nullptr),
            TStringBuf())),
        TToStringFallbackValueFormatterTag>;
};

////////////////////////////////////////////////////////////////////////////////

template <size_t IndexBase, class... TArgs>
struct TArgFormatterImpl;

template <size_t IndexBase>
struct TArgFormatterImpl<IndexBase>
{
    void operator() (size_t /*index*/, TStringBuilderBase* builder, TStringBuf /*format*/) const
    {
        builder->AppendString(TStringBuf("<missing argument>"));
    }
};

template <size_t IndexBase, class THeadArg, class... TTailArgs>
struct TArgFormatterImpl<IndexBase, THeadArg, TTailArgs...>
{
    explicit TArgFormatterImpl(const THeadArg& headArg, const TTailArgs&... tailArgs)
        : HeadArg(headArg)
        , TailFormatter(tailArgs...)
    { }

    const THeadArg& HeadArg;
    TArgFormatterImpl<IndexBase + 1, TTailArgs...> TailFormatter;

    void operator() (size_t index, TStringBuilderBase* builder, TStringBuf format) const
    {
        YT_ASSERT(index >= IndexBase);
        if (index == IndexBase) {
            FormatValue(builder, HeadArg, format);
        } else {
            TailFormatter(index, builder, format);
        }
    }
};

////////////////////////////////////////////////////////////////////////////////

template <class... TArgs, size_t FormatLength>
void Format(
    TStringBuilderBase* builder,
    const char (&format)[FormatLength],
    TArgs&&... args)
{
    Format(builder, TStringBuf(format, FormatLength - 1), std::forward<TArgs>(args)...);
}

template <class... TArgs>
void Format(
    TStringBuilderBase* builder,
    TStringBuf format,
    TArgs&&... args)
{
    TArgFormatterImpl<0, TArgs...> argFormatter(args...);
    FormatImpl(builder, format, argFormatter);
}

template <class... TArgs, size_t FormatLength>
TString Format(
    const char (&format)[FormatLength],
    TArgs&&... args)
{
    TStringBuilder builder;
    Format(&builder, format, std::forward<TArgs>(args)...);
    return builder.Flush();
}

template <class... TArgs>
TString Format(
    TStringBuf format,
    TArgs&&... args)
{
    TStringBuilder builder;
    Format(&builder, format, std::forward<TArgs>(args)...);
    return builder.Flush();
}

////////////////////////////////////////////////////////////////////////////////

} // namespace NYT
