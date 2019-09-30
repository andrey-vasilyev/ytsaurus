#include "http_server_mock.h"

#include <yt/ytlib/auth/token_authenticator.h>
#include <yt/ytlib/auth/cookie_authenticator.h>
#include <yt/ytlib/auth/blackbox_service.h>
#include <yt/ytlib/auth/default_blackbox_service.h>
#include <yt/ytlib/auth/config.h>
#include <yt/ytlib/auth/helpers.h>

#include <yt/core/concurrency/thread_pool_poller.h>

#include <yt/core/test_framework/framework.h>

namespace NYT::NAuth {
namespace {

using namespace NConcurrency;
using namespace NTests;
using namespace NYTree;
using namespace NYson;

using ::testing::_;
using ::testing::HasSubstr;
using ::testing::Return;
using ::testing::AllOf;

////////////////////////////////////////////////////////////////////////////////

class TDefaultBlackboxTest
    : public ::testing::Test
{
protected:
    TDefaultBlackboxServiceConfigPtr CreateDefaultBlackboxServiceConfig()
    {
        auto config = New<TDefaultBlackboxServiceConfig>();
        config->Host = ServerMock_.IsStarted() ? ServerMock_.GetHost() : "localhost";
        config->Port = ServerMock_.IsStarted() ? ServerMock_.GetPort() : static_cast<ui16>(0);
        config->Secure = false;
        config->RequestTimeout = TDuration::MilliSeconds(10);
        config->AttemptTimeout = TDuration::MilliSeconds(10);
        config->BackoffTimeout = TDuration::MilliSeconds(10);
        return config;
    }

    IBlackboxServicePtr CreateDefaultBlackboxService(TDefaultBlackboxServiceConfigPtr config = {})
    {
        return NAuth::CreateDefaultBlackboxService(
            config ? config : CreateDefaultBlackboxServiceConfig(),
            CreateThreadPoolPoller(1, "HttpPoller"));
    }

    virtual void SetUp() override
    {
        ServerMock_.Start();
    }

    virtual void TearDown() override
    {
        if (ServerMock_.IsStarted()) {
            ServerMock_.Stop();
        }
    }

    void SetCallback(THttpServerMock::TCallback callback)
    {
        ServerMock_.SetCallback(std::move(callback));
    }

private:
    THttpServerMock ServerMock_;
};

TEST_F(TDefaultBlackboxTest, FailOnBadHost)
{
    auto config = CreateDefaultBlackboxServiceConfig();
    config->Host = "lokalhozd";
    config->Port = 1;
    auto service = CreateDefaultBlackboxService(config);
    auto result = service->Call("hello", {}).Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("DNS resolve failed"));
}

TEST_F(TDefaultBlackboxTest, FailOn5xxResponse)
{
    SetCallback([&] (TClientRequest* request) {
        EXPECT_THAT(request->Input().FirstLine(), HasSubstr("/blackbox?method=hello"));
        request->Output() << HttpResponse(500, "");
    });
    auto service = CreateDefaultBlackboxService();
    auto result = service->Call("hello", {}).Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("Blackbox call returned HTTP status code 500"));
}

TEST_F(TDefaultBlackboxTest, FailOn4xxResponse)
{
    SetCallback([&] (TClientRequest* request) {
        EXPECT_THAT(request->Input().FirstLine(), HasSubstr("/blackbox?method=hello"));
        request->Output() << HttpResponse(404, "");
    });
    auto service = CreateDefaultBlackboxService();
    auto result = service->Call("hello", {}).Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("Blackbox call returned HTTP status code 404"));
}

TEST_F(TDefaultBlackboxTest, FailOnEmptyResponse)
{
    SetCallback([&] (TClientRequest* request) {
        EXPECT_THAT(request->Input().FirstLine(), HasSubstr("/blackbox?method=hello"));
        request->Output() << HttpResponse(200, "");
    });
    auto service = CreateDefaultBlackboxService();
    auto result = service->Call("hello", {}).Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("Error parsing JSON"));
}

TEST_F(TDefaultBlackboxTest, FailOnMalformedResponse)
{
    SetCallback([&] (TClientRequest* request) {
        EXPECT_THAT(request->Input().FirstLine(), HasSubstr("/blackbox?method=hello"));
        request->Output() << HttpResponse(200, "#$&(^$#@(^");
    });
    auto service = CreateDefaultBlackboxService();
    auto result = service->Call("hello", {}).Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("Error parsing JSON"));
}

TEST_F(TDefaultBlackboxTest, FailOnBlackboxException)
{
    SetCallback([&] (TClientRequest* request) {
        EXPECT_THAT(request->Input().FirstLine(), HasSubstr("/blackbox?method=hello"));
        request->Output() << HttpResponse(200, R"jj({"exception":{"id": 666, "value": "bad stuff happened"}})jj");
    });
    auto service = CreateDefaultBlackboxService();
    auto result = service->Call("hello", {}).Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("Blackbox has raised an exception"));
}


TEST_F(TDefaultBlackboxTest, Success)
{
    SetCallback([&] (TClientRequest* request) {
        EXPECT_THAT(request->Input().FirstLine(), HasSubstr("/blackbox?method=hello&foo=bar&spam=ham"));
        request->Output() << HttpResponse(200, R"jj({"status": "ok"})jj");
    });
    auto service = CreateDefaultBlackboxService();
    auto result = service->Call("hello", {{"foo", "bar"}, {"spam", "ham"}}).Get();
    ASSERT_TRUE(result.IsOK());
    EXPECT_TRUE(AreNodesEqual(result.ValueOrThrow(), ConvertTo<INodePtr>(TYsonString("{status=ok}"))));
}

TEST_F(TDefaultBlackboxTest, RetriesErrors)
{
    std::atomic<int> counter = {0};
    SetCallback([&] (TClientRequest* request) {
        switch (counter) {
            case 0:  request->Output() << HttpResponse(500, ""); break;
            case 1:  request->Output() << HttpResponse(404, ""); break;
            case 2:  request->Output() << HttpResponse(200, ""); break;
            case 3:  request->Output() << HttpResponse(200, "#$&(^$#@(^"); break;
            case 4:  request->Output() << HttpResponse(200, R"jj({"exception":{"id": 9, "value": "DB_FETCHFAILED"}})jj"); break;
            case 5:  request->Output() << HttpResponse(200, R"jj({"exception":{"id": 10, "value": "DB_EXCEPTION"}})jj"); break;
            default: request->Output() << HttpResponse(200, R"jj({"exception":{"id": 0, "value": "OK"}})jj"); break;
        }
        ++counter;
    });

    auto config = CreateDefaultBlackboxServiceConfig();
    config->BackoffTimeout = TDuration::MilliSeconds(0);
    config->AttemptTimeout = TDuration::Seconds(30);
    config->RequestTimeout = TDuration::Seconds(30);
    auto service = CreateDefaultBlackboxService(config);
    auto result = service->Call("hello", {}).Get();
    ASSERT_TRUE(result.IsOK());
    EXPECT_EQ(7, counter.load());
}

////////////////////////////////////////////////////////////////////////////////

class TMockBlackboxService
    : public IBlackboxService
{
public:
    MOCK_METHOD3(Call, TFuture<INodePtr>(
        const TString&,
        const THashMap<TString, TString>&,
        const THashMap<TString, TString>&));

    virtual TErrorOr<TString> GetLogin(const NYTree::INodePtr& reply) const override
    {
        return GetByYPath<TString>(reply, "/login");
    }
};

////////////////////////////////////////////////////////////////////////////////

class TTokenAuthenticatorTest
    : public ::testing::Test
{
protected:
    TTokenAuthenticatorTest()
        : Config_(New<TBlackboxTokenAuthenticatorConfig>())
        , Blackbox_(New<TMockBlackboxService>())
        , Authenticator_(CreateBlackboxTokenAuthenticator(Config_, Blackbox_))
    { }

    void MockCall(const TString& yson)
    {
        EXPECT_CALL(*Blackbox_, Call("oauth", _, _))
            .WillOnce(Return(MakeFuture<INodePtr>(ConvertTo<INodePtr>(TYsonString(yson)))));
    }

    TFuture<TAuthenticationResult> Invoke(const TString& token, const TString& userIP)
    {
        return Authenticator_->Authenticate(TTokenCredentials{
            token,
            NNet::TNetworkAddress::Parse(userIP)
        });
    }

    TBlackboxTokenAuthenticatorConfigPtr Config_;
    TIntrusivePtr<TMockBlackboxService> Blackbox_;
    TIntrusivePtr<ITokenAuthenticator> Authenticator_;
};

TEST_F(TTokenAuthenticatorTest, FailOnUnderlyingFailure)
{
    EXPECT_CALL(*Blackbox_, Call("oauth", _, _))
        .WillOnce(Return(MakeFuture<INodePtr>(TError("Underlying failure"))));
    auto result = Invoke("mytoken", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("Underlying failure"));
}

TEST_F(TTokenAuthenticatorTest, FailOnInvalidResponse1)
{
    MockCall("{}");
    auto result = Invoke("mytoken", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("invalid response"));
}

TEST_F(TTokenAuthenticatorTest, FailOnInvalidResponse2)
{
    MockCall("{status={id=0}}");
    auto result = Invoke("mytoken", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), AllOf(
        HasSubstr("invalid response"),
        HasSubstr("/login"),
        HasSubstr("/oauth/client_id"),
        HasSubstr("/oauth/scope")));
}

TEST_F(TTokenAuthenticatorTest, FailOnRejection)
{
    MockCall("{status={id=5}}");
    auto result = Invoke("mytoken", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("rejected token"));
}

TEST_F(TTokenAuthenticatorTest, FailOnInvalidScope)
{
    Config_->Scope = "yt:api";
    MockCall(R"yy({status={id=0};oauth={scope="i-am-hacker";client_id="i-am-hacker";client_name="yes-i-am"};login=hacker})yy");
    auto result = Invoke("mytoken", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("does not provide a valid scope"));
}

TEST_F(TTokenAuthenticatorTest, Success)
{
    Config_->Scope = "yt:api";
    MockCall(R"yy({status={id=0};oauth={scope="x:1 yt:api x:2";client_id="cid";client_name="nm"};login=sandello})yy");
    auto result = Invoke("mytoken", "127.0.0.1").Get();
    ASSERT_TRUE(result.IsOK());
    EXPECT_EQ("sandello", result.Value().Login);
    EXPECT_EQ("blackbox:token:cid:nm", result.Value().Realm);
}

////////////////////////////////////////////////////////////////////////////////

class TCookieAuthenticatorTest
    : public ::testing::Test
{
protected:
    TCookieAuthenticatorTest()
        : Config_(CreateBlackboxCookieAuthenticatorConfig())
        , Blackbox_(New<TMockBlackboxService>())
        , Authenticator_(CreateBlackboxCookieAuthenticator(Config_, Blackbox_))
    { }

    void MockCall(const TString& yson)
    {
        EXPECT_CALL(*Blackbox_, Call("sessionid", _, _))
            .WillOnce(Return(MakeFuture<INodePtr>(ConvertTo<INodePtr>(TYsonString(yson)))));
    }

    TFuture<TAuthenticationResult> Authenticate(
        const TString& sessionId,
        const TString& sslSessionId,
        const TString& userIP)
    {
        TCookieCredentials credentials;
        credentials.SessionId = sessionId;
        credentials.SslSessionId = sslSessionId;
        credentials.UserIP = NNet::TNetworkAddress::Parse(userIP);
        return Authenticator_->Authenticate(credentials);
    }

protected:
    TBlackboxCookieAuthenticatorConfigPtr Config_;
    TIntrusivePtr<TMockBlackboxService> Blackbox_;
    TIntrusivePtr<ICookieAuthenticator> Authenticator_;

    static TBlackboxCookieAuthenticatorConfigPtr CreateBlackboxCookieAuthenticatorConfig()
    {
        auto config = New<TBlackboxCookieAuthenticatorConfig>();
        config->Domain = "myhost";
        return config;
    }
};

TEST_F(TCookieAuthenticatorTest, FailOnUnderlyingFailure)
{
    EXPECT_CALL(*Blackbox_, Call("sessionid", _, _))
        .WillOnce(Return(MakeFuture<INodePtr>(TError("Underlying failure"))));
    auto result = Authenticate("mysessionid", "mysslsessionid", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("Underlying failure"));
}

TEST_F(TCookieAuthenticatorTest, FailOnInvalidResponse1)
{
    MockCall("{}");
    auto result = Authenticate("mysessionid", "mysslsessionid", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("invalid response"));
}

TEST_F(TCookieAuthenticatorTest, FailOnInvalidResponse2)
{
    MockCall("{status={id=0}}");
    auto result = Authenticate("mysessionid", "mysslsessionid", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), AllOf(
        HasSubstr("invalid response"),
        HasSubstr("/login")));
}

TEST_F(TCookieAuthenticatorTest, FailOnRejection)
{
    MockCall("{status={id=5}}");
    auto result = Authenticate("mysessionid", "mysslsessionid", "127.0.0.1").Get();
    ASSERT_TRUE(!result.IsOK());
    EXPECT_THAT(CollectMessages(result), HasSubstr("rejected session cookie"));
}

TEST_F(TCookieAuthenticatorTest, Success)
{
    MockCall("{status={id=0};login=sandello}");
    auto result = Authenticate("mysessionid", "mysslsessionid", "127.0.0.1").Get();
    ASSERT_TRUE(result.IsOK());
    EXPECT_EQ("sandello", result.Value().Login);
    EXPECT_EQ("blackbox:cookie", result.Value().Realm);
}

////////////////////////////////////////////////////////////////////////////////

} // namespace
} // namespace NYT::NAuth
