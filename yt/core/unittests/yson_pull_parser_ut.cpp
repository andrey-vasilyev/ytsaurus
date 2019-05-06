#include <yt/core/test_framework/framework.h>
#include <yt/core/test_framework/yson_consumer_mock.h>

#include <yt/core/yson/pull_parser.h>

#include <yt/core/misc/error.h>

#include <util/stream/format.h>
#include <util/stream/mem.h>

namespace NYT::NYson {

////////////////////////////////////////////////////////////////////////////////

void PrintTo(const TYsonItem& ysonItem, ::std::ostream* out)
{
    auto type = ysonItem.GetType();
    (*out) << ToString(type) << " {";
    switch (type) {
        case EYsonItemType::Int64Value:
            (*out) << ysonItem.UncheckedAsInt64();
            break;
        case EYsonItemType::Uint64Value:
            (*out) << ysonItem.UncheckedAsUint64();
            break;
        case EYsonItemType::DoubleValue:
            (*out) << ysonItem.UncheckedAsDouble();
            break;
        case EYsonItemType::BooleanValue:
            (*out) << ysonItem.UncheckedAsBoolean();
            break;
        case EYsonItemType::StringValue:
            (*out) << ysonItem.UncheckedAsString();
            break;
        default:
            (*out) << ' ';
            break;
    }
    (*out) << "}";
}

////////////////////////////////////////////////////////////////////////////////

namespace {

////////////////////////////////////////////////////////////////////////////////

TString GetYsonPullSignature(TYsonPullParser* parser, std::optional<size_t> levelToMark=std::nullopt)
{
    TString result;
    {
        TStringOutput out(result);
        bool exhausted = false;
        while (!exhausted) {
            if (levelToMark && parser->IsOnValueBoundary(*levelToMark)) {
                out << "! ";
            }

            auto item = parser->Next();
            switch (item.GetType()) {
                case EYsonItemType::BeginMap:
                    out << "{";
                    break;
                case EYsonItemType::EndMap:
                    out << "}";
                    break;
                case EYsonItemType::BeginAttributes:
                    out << "<";
                    break;
                case EYsonItemType::EndAttributes:
                    out << ">";
                    break;
                case EYsonItemType::BeginList:
                    out << "[";
                    break;
                case EYsonItemType::EndList:
                    out << "]";
                    break;
                case EYsonItemType::EntityValue:
                    out << "#";
                    break;
                case EYsonItemType::BooleanValue:
                    out << (item.UncheckedAsBoolean() ? "%true" : "%false");
                    break;
                case EYsonItemType::Int64Value:
                    out << item.UncheckedAsInt64();
                    break;
                case EYsonItemType::Uint64Value:
                    out << item.UncheckedAsUint64() << 'u';
                    break;
                case EYsonItemType::DoubleValue: {
                    auto value = item.UncheckedAsDouble();
                    if (std::isnan(value)) {
                        out << "%nan";
                    } else if (std::isinf(value)) {
                        if (value > 0) {
                            out << "%+inf";
                        } else {
                            out << "%-inf";
                        }
                    } else {
                        out << Prec(value, PREC_POINT_DIGITS, 2);
                    }
                    break;
                }
                case EYsonItemType::StringValue:
                    out << '\'' << EscapeC(item.UncheckedAsString()) << '\'';
                    break;
                case EYsonItemType::EndOfStream:
                    exhausted = true;
                    continue;
            }
            out << ' ';
        }
    }
    if (result) {
        // Strip trailing space.
        result.resize(result.size() - 1);
    }
    return result;
}

TString GetYsonPullSignature(
    TStringBuf input,
    EYsonType type = EYsonType::Node,
    std::optional<size_t> levelToMark = std::nullopt)
{
    TMemoryInput in(input.data(), input.size());
    TYsonPullParser parser(&in, type);
    return GetYsonPullSignature(&parser, levelToMark);
}

////////////////////////////////////////////////////////////////////////////////

class TStringBufVectorReader
    : public IZeroCopyInput
{
public:
    TStringBufVectorReader(const std::vector<TStringBuf>& data)
        : Data_(data.rbegin(), data.rend())
    {
        NextBuffer();
    }

private:
    void NextBuffer() {
        if (Data_.empty()) {
            CurrentInput_.reset();
        } else {
            CurrentInput_.emplace(Data_.back());
            Data_.pop_back();
        }
    }

    size_t DoNext(const void** ptr, size_t len)
    {
        if (!len) {
            return 0;
        }
        while (CurrentInput_) {
            auto result = CurrentInput_->Next(ptr, len);
            if (result) {
                return result;
            }
            NextBuffer();
        }
        return 0;
    }

private:
    std::vector<TStringBuf> Data_;
    std::optional<TMemoryInput> CurrentInput_;
};

struct TStringBufCursorHelper
{
    TMemoryInput MemoryInput;
    TYsonPullParser PullParser;

    TStringBufCursorHelper(TStringBuf input, EYsonType ysonType)
        : MemoryInput(input)
        , PullParser(&MemoryInput, ysonType)
    { }
};

class TStringBufCursor
    : private TStringBufCursorHelper
    , public TYsonPullParserCursor
{
public:
    explicit TStringBufCursor(TStringBuf input, EYsonType ysonType = EYsonType::Node)
        : TStringBufCursorHelper(input, ysonType)
        , TYsonPullParserCursor(&PullParser)
    { }
};

////////////////////////////////////////////////////////////////////////////////

TEST(TYsonPullParserTest, Int)
{
    EXPECT_EQ(GetYsonPullSignature(" 100500 "), "100500");
    EXPECT_EQ(GetYsonPullSignature("\x02\xa7\xa2\x0c"), "-100500");
}

TEST(TYsonPullParserTest, Uint)
{
    EXPECT_EQ(GetYsonPullSignature(" 42u "), "42u");
    EXPECT_EQ(GetYsonPullSignature("\x06\x94\x91\x06"), "100500u");
}

TEST(TYsonPullParserTest, Double)
{
    EXPECT_EQ(GetYsonPullSignature(" 31415926e-7 "), "3.14");
    EXPECT_EQ(GetYsonPullSignature("\x03iW\x14\x8B\n\xBF\5@"), "2.72");
    EXPECT_EQ(GetYsonPullSignature(" %nan "), "%nan");
    EXPECT_ANY_THROW(GetYsonPullSignature(" %+nan "));
    EXPECT_ANY_THROW(GetYsonPullSignature(" %-nan "));
    EXPECT_ANY_THROW(GetYsonPullSignature(" %nany "));
    EXPECT_ANY_THROW(GetYsonPullSignature(" +nan "));
    EXPECT_EQ(GetYsonPullSignature(" %inf "), "%+inf");
    EXPECT_EQ(GetYsonPullSignature(" %+inf "), "%+inf");
    EXPECT_EQ(GetYsonPullSignature(" %-inf "), "%-inf");
    EXPECT_ANY_THROW(GetYsonPullSignature(" +inf "));
    EXPECT_ANY_THROW(GetYsonPullSignature(" %infinity "));
}

TEST(TYsonPullParserTest, String)
{
    EXPECT_EQ(GetYsonPullSignature(" nan "), "'nan'");
    EXPECT_EQ(GetYsonPullSignature("\x01\x06" "bar"), "'bar'");
    EXPECT_EQ(GetYsonPullSignature("\x01\x80\x01" + TString(64, 'a')), TString("'") + TString(64, 'a') + "'");
    EXPECT_EQ(GetYsonPullSignature(AsStringBuf("\x01\x00")), "''");
    EXPECT_EQ(GetYsonPullSignature(" Hello_789_World_123 "), "'Hello_789_World_123'");
    EXPECT_EQ(GetYsonPullSignature(" Hello_789_World_123 "), "'Hello_789_World_123'");
    EXPECT_EQ(
        GetYsonPullSignature("\" abcdeABCDE <1234567> + (10_000) - = 900   \""),
        "' abcdeABCDE <1234567> + (10_000) - = 900   '"
    );
}

TEST(TYsonPullParserTest, StringEscaping)
{
    TString expected;
    for (int i = 0; i < 256; ++i) {
        expected.push_back(char(i));
    }

    auto inputData = AsStringBuf(
        "\"\\0\\1\\2\\3\\4\\5\\6\\7\\x08\\t\\n\\x0B\\x0C\\r\\x0E\\x0F"
        "\\x10\\x11\\x12\\x13\\x14\\x15\\x16\\x17\\x18\\x19\\x1A\\x1B"
        "\\x1C\\x1D\\x1E\\x1F !\\\"#$%&'()*+,-./0123456789:;<=>?@ABCD"
        "EFGHIJKLMNOPQRSTUVWXYZ[\\\\]^_`abcdefghijklmnopqrstuvwxyz{|}~"
        "\\x7F\\x80\\x81\\x82\\x83\\x84\\x85\\x86\\x87\\x88\\x89\\x8A"
        "\\x8B\\x8C\\x8D\\x8E\\x8F\\x90\\x91\\x92\\x93\\x94\\x95\\x96"
        "\\x97\\x98\\x99\\x9A\\x9B\\x9C\\x9D\\x9E\\x9F\\xA0\\xA1\\xA2"
        "\\xA3\\xA4\\xA5\\xA6\\xA7\\xA8\\xA9\\xAA\\xAB\\xAC\\xAD\\xAE"
        "\\xAF\\xB0\\xB1\\xB2\\xB3\\xB4\\xB5\\xB6\\xB7\\xB8\\xB9\\xBA"
        "\\xBB\\xBC\\xBD\\xBE\\xBF\\xC0\\xC1\\xC2\\xC3\\xC4\\xC5\\xC6"
        "\\xC7\\xC8\\xC9\\xCA\\xCB\\xCC\\xCD\\xCE\\xCF\\xD0\\xD1\\xD2"
        "\\xD3\\xD4\\xD5\\xD6\\xD7\\xD8\\xD9\\xDA\\xDB\\xDC\\xDD\\xDE"
        "\\xDF\\xE0\\xE1\\xE2\\xE3\\xE4\\xE5\\xE6\\xE7\\xE8\\xE9\\xEA"
        "\\xEB\\xEC\\xED\\xEE\\xEF\\xF0\\xF1\\xF2\\xF3\\xF4\\xF5\\xF6"
        "\\xF7\\xF8\\xF9\\xFA\\xFB\\xFC\\xFD\\xFE\\xFF\"");

    TMemoryInput inputStream(inputData);
    TYsonPullParser parser(&inputStream, EYsonType::Node);
    auto item1 = parser.Next();
    EXPECT_EQ(item1.GetType(), EYsonItemType::StringValue);
    EXPECT_EQ(item1.UncheckedAsString(), expected);

    auto item2 = parser.Next();
    EXPECT_TRUE(item2.IsEndOfStream());
}

TEST(TYsonPullParserTest, TrailingSlashes)
{
    char inputData[] = {'"', '\\', '"', '\\', '\\', '"'};
    TMemoryInput inputStream(inputData, sizeof(inputData));
    TYsonPullParser parser(&inputStream, EYsonType::Node);
    auto item1 = parser.Next();
    EXPECT_EQ(item1.GetType(), EYsonItemType::StringValue);
    EXPECT_EQ(item1.UncheckedAsString(), "\"\\");

    auto item2 = parser.Next();
    EXPECT_TRUE(item2.IsEndOfStream());
}

TEST(TYsonPullParserTest, Entity)
{
    EXPECT_EQ(GetYsonPullSignature(" # "), "#");
}

TEST(TYsonPullParserTest, Boolean)
{
    EXPECT_EQ(GetYsonPullSignature(" %true "), "%true");
    EXPECT_EQ(GetYsonPullSignature(" %false "), "%false");
    EXPECT_EQ(GetYsonPullSignature("\x04"), "%false");
    EXPECT_EQ(GetYsonPullSignature("\x05"), "%true");
    EXPECT_ANY_THROW(GetYsonPullSignature(" %falsee "));
}

TEST(TYsonPullParserTest, List)
{
    EXPECT_EQ(GetYsonPullSignature("[]"), "[ ]");
    EXPECT_EQ(GetYsonPullSignature("[[]]"), "[ [ ] ]");
    EXPECT_EQ(GetYsonPullSignature("[[] ; ]"), "[ [ ] ]");
    EXPECT_EQ(GetYsonPullSignature("[[] ; [[]] ]"), "[ [ ] [ [ ] ] ]");
}

TEST(TYsonPullParserTest, Map)
{
    EXPECT_EQ(GetYsonPullSignature("{}"), "{ }");
    EXPECT_EQ(GetYsonPullSignature("{k=v}"), "{ 'k' 'v' }");
    EXPECT_EQ(GetYsonPullSignature("{k=v;}"), "{ 'k' 'v' }");
    EXPECT_EQ(GetYsonPullSignature("{k1=v; k2={} }"), "{ 'k1' 'v' 'k2' { } }");
}

TEST(TYsonPullParserTest, Attributes)
{
    EXPECT_EQ(GetYsonPullSignature("<>#"), "< > #");
    EXPECT_EQ(GetYsonPullSignature("<a=v> #"), "< 'a' 'v' > #");
    EXPECT_EQ(GetYsonPullSignature("<a=v;> #"), "< 'a' 'v' > #");
    EXPECT_EQ(GetYsonPullSignature("<a1=v; a2={}; a3=<># > #"), "< 'a1' 'v' 'a2' { } 'a3' < > # > #");
}

TEST(TYsonPullParserTest, ListFragment)
{
    EXPECT_EQ(GetYsonPullSignature("", EYsonType::ListFragment), "");
    EXPECT_EQ(GetYsonPullSignature("#", EYsonType::ListFragment), "#");
    EXPECT_EQ(GetYsonPullSignature("#;", EYsonType::ListFragment), "#");
    EXPECT_EQ(GetYsonPullSignature("#; #", EYsonType::ListFragment), "# #");
    EXPECT_EQ(GetYsonPullSignature("[];{};<>#;", EYsonType::ListFragment), "[ ] { } < > #");
}

TEST(TYsonPullParserTest, MapFragment)
{
    EXPECT_EQ(GetYsonPullSignature("", EYsonType::MapFragment), "");
    EXPECT_EQ(GetYsonPullSignature("k=v ", EYsonType::MapFragment), "'k' 'v'");
    EXPECT_EQ(GetYsonPullSignature("k=v ; ", EYsonType::MapFragment), "'k' 'v'");
    EXPECT_EQ(GetYsonPullSignature("k1=v; k2={}; k3=[]; k4=<>#", EYsonType::MapFragment), "'k1' 'v' 'k2' { } 'k3' [ ] 'k4' < > #");
}

TEST(TYsonPullParserTest, Complex1)
{
    EXPECT_EQ(
        GetYsonPullSignature(
            "<acl = { read = [ \"*\" ]; write = [ sandello ] } ;  \n"
            "  lock_scope = mytables> \n"
            "{ path = \"/home/sandello\" ; mode = 0755 }"),

            "< 'acl' { 'read' [ '*' ] 'write' [ 'sandello' ] }"
            " 'lock_scope' 'mytables' >"
            " { 'path' '/home/sandello' 'mode' 755 }");
}

TEST(TYsonPullParserTest, TestBadYson)
{
    EXPECT_ANY_THROW(GetYsonPullSignature("foo bar "));
    EXPECT_ANY_THROW(GetYsonPullSignature("foo bar ", EYsonType::ListFragment));
    EXPECT_ANY_THROW(GetYsonPullSignature("foo bar ", EYsonType::MapFragment));
    EXPECT_ANY_THROW(GetYsonPullSignature("foo; bar"));
    EXPECT_ANY_THROW(GetYsonPullSignature("foo bar ", EYsonType::MapFragment));
    EXPECT_ANY_THROW(GetYsonPullSignature("foo=bar "));
    EXPECT_ANY_THROW(GetYsonPullSignature("foo=bar ", EYsonType::ListFragment));
}

TEST(TYsonPullParserTest, Capture)
{
    EXPECT_EQ(GetYsonPullSignature(" foo ", EYsonType::Node, 0), "! 'foo' !");
    EXPECT_EQ(GetYsonPullSignature(" <foo=bar>foo ", EYsonType::Node, 0), "! < 'foo' 'bar' > 'foo' !");
    EXPECT_EQ(GetYsonPullSignature(" <foo=bar>[ 42 ] ", EYsonType::Node, 1), "< 'foo' ! 'bar' ! > [ ! 42 ! ]");
    EXPECT_EQ(GetYsonPullSignature(
        " <foo=[bar]; bar=2; baz=[[1;2;3]]>[ 1; []; {foo=[]}] ", EYsonType::Node, 2),
        "< 'foo' [ ! 'bar' ! ] 'bar' 2 'baz' [ ! [ 1 2 3 ] ! ] > [ 1 [ ! ] { 'foo' ! [ ] ! } ]");
}

TEST(TYsonPullParserTest, DepthLimitExceeded)
{
    EXPECT_NO_THROW(GetYsonPullSignature(TString(63, '[') + TString(63, ']')));
    EXPECT_ANY_THROW(GetYsonPullSignature(TString(64, '[') + TString(64, ']')));
}

TEST(TYsonPullParserTest, ContextInExceptions)
{
    try {
        GetYsonPullSignature("{foo bar = 580}");
    } catch (const std::exception& ex) {
        EXPECT_THAT(ex.what(), testing::HasSubstr("bar = 580}"));
        return;
    }
    GTEST_FAIL() << "Expected exception to be thrown";
}

TEST(TYsonPullParserTest, ContextInExceptions_ManyBlocks)
{
    try {
        auto manyO = TString(100, 'o');
        TStringBufVectorReader input(
            {
                "{fo",
                manyO, // try to overflow 64 byte context
                "o bar = 580}",
            }
        );
        TYsonPullParser parser(&input, EYsonType::Node);
        GetYsonPullSignature(&parser);
    } catch (const std::exception& ex) {
        EXPECT_THAT(ex.what(), testing::HasSubstr("bar = 580}"));
        return;
    }
    GTEST_FAIL() << "Expected exception to be thrown";
}

TEST(TYsonPullParserTest, ContextInExceptions_ContextAtTheVeryBeginning)
{
    try {
        GetYsonPullSignature("! foo bar baz");
    } catch (const std::exception& ex) {
        EXPECT_THAT(ex.what(), testing::HasSubstr("! foo bar"));
        return;
    }
    GTEST_FAIL() << "Expected exception to be thrown";
}

TEST(TYsonPullTest, ContextInExceptions_Margin)
{
    try {
        auto manyO = TString(100, 'o');
        TStringBufVectorReader input(
            {
                "{fo",
                manyO, // try to overflow 64 byte context
                "a",
                "b",
                "c",
                "d bar = 580}",
            }
        );
        TYsonPullParser parser(&input, EYsonType::Node);
        GetYsonPullSignature(&parser);
    } catch (const std::exception& ex) {
        EXPECT_THAT(ex.what(), testing::HasSubstr("oabcd bar = 580}"));
        return;
    }
}

TEST(TYsonPullParserCursorTest, TestTransferValueBasicCases)
{
    auto input = AsStringBuf("[ [ {foo=<attr=value>bar; qux=[-1; 2u; %false; 3.14; lol; # ]} ] ; 6 ]");
    auto cursor = TStringBufCursor(input);
    EXPECT_EQ(cursor.GetCurrent(), TYsonItem::Simple(EYsonItemType::BeginList));
    cursor.Next();
    {
        ::testing::StrictMock<TMockYsonConsumer> mock;
        {
            ::testing::InSequence g;
            EXPECT_CALL(mock, OnBeginList());
            EXPECT_CALL(mock, OnListItem());
            EXPECT_CALL(mock, OnBeginMap());
            EXPECT_CALL(mock, OnKeyedItem("foo"));
            EXPECT_CALL(mock, OnBeginAttributes());
            EXPECT_CALL(mock, OnKeyedItem("attr"));
            EXPECT_CALL(mock, OnStringScalar("value"));
            EXPECT_CALL(mock, OnEndAttributes());
            EXPECT_CALL(mock, OnStringScalar("bar"));
            EXPECT_CALL(mock, OnKeyedItem("qux"));
            EXPECT_CALL(mock, OnBeginList());
            EXPECT_CALL(mock, OnListItem());
            EXPECT_CALL(mock, OnInt64Scalar(-1));
            EXPECT_CALL(mock, OnListItem());
            EXPECT_CALL(mock, OnUint64Scalar(2));
            EXPECT_CALL(mock, OnListItem());
            EXPECT_CALL(mock, OnBooleanScalar(false));
            EXPECT_CALL(mock, OnListItem());
            EXPECT_CALL(mock, OnDoubleScalar(::testing::DoubleEq(3.14)));
            EXPECT_CALL(mock, OnListItem());
            EXPECT_CALL(mock, OnStringScalar("lol"));
            EXPECT_CALL(mock, OnListItem());
            EXPECT_CALL(mock, OnEntity());
            EXPECT_CALL(mock, OnEndList());
            EXPECT_CALL(mock, OnEndMap());
            EXPECT_CALL(mock, OnEndList());
        }
        cursor.TransferComplexValue(&mock);
    }
    EXPECT_EQ(cursor.GetCurrent(), TYsonItem::Int64(6));
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NYTree
