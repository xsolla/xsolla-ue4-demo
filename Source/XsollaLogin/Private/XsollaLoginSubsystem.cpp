// Copyright 2019 Xsolla Inc. All Rights Reserved.
// @author Vladimir Alyamkin <ufna@ufna.ru>

#include "XsollaLoginSubsystem.h"

#include "XsollaLogin.h"
#include "XsollaLoginDefines.h"
#include "XsollaLoginLibrary.h"
#include "XsollaLoginSave.h"
#include "XsollaLoginSettings.h"

#include "Developer/Settings/Public/ISettingsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/Engine.h"
#include "JsonObjectConverter.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/Base64.h"
#include "OnlineSubsystem.h"
#include "Runtime/Launch/Resources/Version.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/ConstructorHelpers.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "FXsollaLoginModule"

const FString UXsollaLoginSubsystem::RegistrationEndpoint(TEXT("https://login.xsolla.com/api/user"));
const FString UXsollaLoginSubsystem::LoginEndpoint(TEXT("https://login.xsolla.com/api/login"));
const FString UXsollaLoginSubsystem::LoginSocialEndpoint(TEXT("https://login.xsolla.com/api/social"));
const FString UXsollaLoginSubsystem::ResetPasswordEndpoint(TEXT("https://login.xsolla.com/api/password/reset/request"));

const FString UXsollaLoginSubsystem::ProxyRegistrationEndpoint(TEXT("https://login.xsolla.com/api/proxy/registration"));
const FString UXsollaLoginSubsystem::ProxyLoginEndpoint(TEXT("https://login.xsolla.com/api/proxy/login"));
const FString UXsollaLoginSubsystem::ProxyResetPasswordEndpoint(TEXT("https://login.xsolla.com/api/proxy/password/reset"));

const FString UXsollaLoginSubsystem::ValidateTokenEndpoint(TEXT("https://login.xsolla.com/api/users/me"));

const FString UXsollaLoginSubsystem::UserAttributesEndpoint(TEXT("https://login.xsolla.com/api/attributes"));

const FString UXsollaLoginSubsystem::CrossAuthEndpoint(TEXT("https://livedemo.xsolla.com/sdk/token"));

const FString UXsollaLoginSubsystem::AccountLinkingCodeEndpoint(TEXT("https://login.xsolla.com/api/users/account/code"));

const FString UXsollaLoginSubsystem::LoginEndpointOAuth(TEXT("https://login.xsolla.com/api/oauth2"));

const FString UXsollaLoginSubsystem::BlankRedirectEndpoint(TEXT("https://login.xsolla.com/api/blank"));

UXsollaLoginSubsystem::UXsollaLoginSubsystem()
	: UGameInstanceSubsystem()
{
	static ConstructorHelpers::FClassFinder<UUserWidget> BrowserWidgetFinder(TEXT("/Xsolla/Browser/W_LoginBrowser.W_LoginBrowser_C"));
	DefaultBrowserWidgetClass = BrowserWidgetFinder.Class;
}

void UXsollaLoginSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	LoadSavedData();

	// Initialize subsystem with project identifiers provided by user
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	Initialize(Settings->ProjectID, Settings->LoginID);

	UE_LOG(LogXsollaLogin, Log, TEXT("%s: XsollaLogin subsystem initialized"), *VA_FUNC_LINE);
}

void UXsollaLoginSubsystem::Deinitialize()
{
	// Do nothing for now
	Super::Deinitialize();
}

void UXsollaLoginSubsystem::Initialize(const FString& InProjectId, const FString& InLoginId)
{
	ProjectID = InProjectId;
	LoginID = InLoginId;

	// Check token override from Xsolla Launcher
	FString LauncherLoginJwt = UXsollaLoginLibrary::GetStringCommandLineParam(TEXT("xsolla-login-jwt"));
	if (!LauncherLoginJwt.IsEmpty())
	{
		UE_LOG(LogXsollaLogin, Warning, TEXT("%s: Xsolla Launcher login token is used"), *VA_FUNC_LINE);
		LoginData.AuthToken.JWT = LauncherLoginJwt;
	}
}

void UXsollaLoginSubsystem::RegistrateUser(const FString& Username, const FString& Password, const FString& Email, const FString& State, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	if (IOnlineSubsystem::IsEnabled(STEAM_SUBSYSTEM))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: User registration should be handled via Steam"), *VA_FUNC_LINE);
		ErrorCallback.ExecuteIfBound(TEXT("Registration failed"), TEXT("User registration should be handled via Steam"));
		return;
	}

	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();

	if (Settings->UseOAuth2)
	{
		RegistrateUserOAuth(Username, Password, Email, State, SuccessCallback, ErrorCallback);
	}
	else
	{
		RegistrateUserJWT(Username, Password, Email, SuccessCallback, ErrorCallback);
	}
}

void UXsollaLoginSubsystem::AuthenticateUser(const FString& Username, const FString& Password, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback, bool bRememberMe)
{
	if (IOnlineSubsystem::IsEnabled(STEAM_SUBSYSTEM))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: User authentication should be handled via Steam"), *VA_FUNC_LINE);
		ErrorCallback.ExecuteIfBound(TEXT("Authentication failed"), TEXT("User authentication should be handled via Steam"));
		return;
	}

	// Be sure we've dropped any saved info
	LoginData = FXsollaLoginData();
	LoginData.Username = Username;
	LoginData.Password = Password;
	LoginData.bRememberMe = bRememberMe;
	SaveData();

	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();

	if (Settings->UseOAuth2)
	{
		AuthenticateUserOAuth(Username, Password, SuccessCallback, ErrorCallback);
	}
	else
	{
		AuthenticateUserJWT(Username, Password, bRememberMe, SuccessCallback, ErrorCallback);
	}
}

void UXsollaLoginSubsystem::ResetUserPassword(const FString& User, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	if (IOnlineSubsystem::IsEnabled(STEAM_SUBSYSTEM))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: User password reset should be handled via Steam"), *VA_FUNC_LINE);
		ErrorCallback.ExecuteIfBound(TEXT("Password reset failed"), TEXT("User password reset should be handled via Steam"));
		return;
	}

	// Prepare request payload
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetStringField((Settings->UserDataStorage == EUserDataStorage::Xsolla) ? TEXT("username") : TEXT("email"), User);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	// Generate endpoint url
	const FString Endpoint = (Settings->UserDataStorage == EUserDataStorage::Xsolla) ? ResetPasswordEndpoint : ProxyResetPasswordEndpoint;
	const FString Url = FString::Printf(TEXT("%s?projectId=%s&login_url=%s"),
		*Endpoint,
		*LoginID,
		*FGenericPlatformHttp::UrlEncode(Settings->CallbackURL));

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::Default_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::ValidateToken(const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Generate endpoint url
	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(ValidateTokenEndpoint, EXsollaLoginRequestVerb::GET, TEXT(""), LoginData.AuthToken.JWT);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::TokenVerify_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::GetSocialAuthenticationUrl(const FString& ProviderName, const FString& State, const FOnSocialUrlReceived& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();

	if (Settings->UseOAuth2)
	{
		GetSocialAuthenticationUrlOAuth(ProviderName, State, SuccessCallback, ErrorCallback);
	}
	else
	{
		GetSocialAuthenticationUrlJWT(ProviderName, SuccessCallback, ErrorCallback);
	}
}

void UXsollaLoginSubsystem::LaunchSocialAuthentication(const FString& SocialAuthenticationUrl, UUserWidget*& BrowserWidget, bool bRememberMe)
{
	PendingSocialAuthenticationUrl = SocialAuthenticationUrl;

	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();

	// Check for user browser widget override
	auto BrowserWidgetClass = (Settings->OverrideBrowserWidgetClass) ? Settings->OverrideBrowserWidgetClass : DefaultBrowserWidgetClass;

	auto MyBrowser = CreateWidget<UUserWidget>(GEngine->GameViewport->GetWorld(), BrowserWidgetClass);
	MyBrowser->AddToViewport(MAX_int32);

	BrowserWidget = MyBrowser;

	// Be sure we've dropped any saved info
	LoginData = FXsollaLoginData();
	LoginData.bRememberMe = bRememberMe;
	SaveData();
}

void UXsollaLoginSubsystem::SetToken(const FString& Token)
{
	LoginData.AuthToken.JWT = Token;
	SaveData();
}

void UXsollaLoginSubsystem::RefreshToken(const FString& RefreshToken, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();

	// Prepare request payload
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetStringField(TEXT("client_id"), Settings->ClientID);
	RequestDataJson->SetStringField(TEXT("grant_type"), TEXT("refresh_token"));
	RequestDataJson->SetStringField(TEXT("refresh_token"), RefreshToken);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	// Generate endpoint url
	const FString Url = FString::Printf(TEXT("%s/token"), *LoginEndpointOAuth);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST);
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	HttpRequest->SetContentAsString(EncodeFormData(RequestDataJson));
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::RefreshTokenOAuth_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::ExchangeAuthenticationCodeToToken(const FString& AuthenticationCode, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();

	// Prepare request payload
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetStringField(TEXT("client_id"), Settings->ClientID);
	RequestDataJson->SetStringField(TEXT("grant_type"), TEXT("authorization_code"));
	RequestDataJson->SetStringField(TEXT("code"), AuthenticationCode);
	RequestDataJson->SetStringField(TEXT("redirect_uri"), BlankRedirectEndpoint);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	// Generate endpoint url
	const FString Url = FString::Printf(TEXT("%s/token"), *LoginEndpointOAuth);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST);
	HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/x-www-form-urlencoded"));
	HttpRequest->SetContentAsString(EncodeFormData(RequestDataJson));
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::RefreshTokenOAuth_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::AuthenticateWithSessionTicket(const FString& ProviderName, const FString& SessionTicket, const FString& AppId, const FString& State, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();

	if (Settings->UseOAuth2)
	{
		AuthenticateWithSessionTicketOAuth(ProviderName, AppId, SessionTicket, State, SuccessCallback, ErrorCallback);
	}
	else
	{
		AuthenticateWithSessionTicketJWT(ProviderName, AppId, SessionTicket, SuccessCallback, ErrorCallback);
	}
}

void UXsollaLoginSubsystem::UpdateUserAttributes(const FString& AuthToken, const FString& UserId, const TArray<FString>& AttributeKeys, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Prepare request body
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetNumberField(TEXT("publisher_project_id"), FCString::Atoi(*ProjectID));
	if (!UserId.IsEmpty())
	{
		RequestDataJson->SetStringField(TEXT("user_id"), UserId);
	}

	TArray<TSharedPtr<FJsonValue>> KeysJsonArray;
	for (auto Key : AttributeKeys)
	{
		KeysJsonArray.Push(MakeShareable(new FJsonValueString(Key)));
	}

	RequestDataJson->SetArrayField(TEXT("keys"), KeysJsonArray);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	const FString Url = FString::Printf(TEXT("%s/users/me/get"), *UserAttributesEndpoint);
	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent, AuthToken);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::UpdateUserAttributes_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::ModifyUserAttributes(const FString& AuthToken, const TArray<FXsollaUserAttribute>& AttributesToModify, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Prepare request body
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());

	TArray<TSharedPtr<FJsonValue>> AttributesJsonArray;
	for (auto Attribute : AttributesToModify)
	{
		TSharedRef<FJsonObject> AttributeJson = MakeShareable(new FJsonObject());
		if (FJsonObjectConverter::UStructToJsonObject(FXsollaUserAttribute::StaticStruct(), &Attribute, AttributeJson, 0, 0))
		{
			AttributesJsonArray.Push(MakeShareable(new FJsonValueObject(AttributeJson)));
		}
	}

	RequestDataJson->SetArrayField(TEXT("attributes"), AttributesJsonArray);
	RequestDataJson->SetNumberField(TEXT("publisher_project_id"), FCString::Atoi(*ProjectID));

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	const FString Url = FString::Printf(TEXT("%s/users/me/update"), *UserAttributesEndpoint);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent, AuthToken);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::Default_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::RemoveUserAttributes(const FString& AuthToken, const TArray<FString>& AttributesToRemove, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Prepare request body
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetNumberField(TEXT("publisher_project_id"), FCString::Atoi(*ProjectID));
	SetStringArrayField(RequestDataJson, TEXT("removing_keys"), AttributesToRemove);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	const FString Url = FString::Printf(TEXT("%s/users/me/update"), *UserAttributesEndpoint);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent, AuthToken);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::Default_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::CreateAccountLinkingCode(const FString& AuthToken, const FOnCodeReceived& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(AccountLinkingCodeEndpoint, EXsollaLoginRequestVerb::POST, TEXT(""), AuthToken);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::AccountLinkingCode_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::LinkAccount(const FString& UserId, const EXsollaTargetPlatform Platform, const FString& Code, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString PlatformName = GetTargetPlatformName(Platform);
	const FString Url = FString::Printf(TEXT("%s?user_id=%s&platform=%s&code=%s"), *Settings->AccountLinkingURL, *UserId, *PlatformName, *Code);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::Default_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::AuthenticatePlatformAccountUser(const FString& UserId, const EXsollaTargetPlatform Platform, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString PlatformName = GetTargetPlatformName(Platform);
	const FString Url = FString::Printf(TEXT("%s?user_id=%s&platform=%s"), *Settings->PlatformAuthenticationURL, *UserId, *PlatformName);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::GET);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::AuthConsoleAccountUser_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::RegistrateUserJWT(const FString& Username, const FString& Password, const FString& Email, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Prepare request payload
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetStringField(TEXT("username"), Username);
	RequestDataJson->SetStringField(TEXT("password"), Password);
	RequestDataJson->SetStringField(TEXT("email"), Email);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	// Generate endpoint url
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString Endpoint = (Settings->UserDataStorage == EUserDataStorage::Xsolla) ? RegistrationEndpoint : ProxyRegistrationEndpoint;
	const FString Url = FString::Printf(TEXT("%s?projectId=%s&login_url=%s"),
		*Endpoint,
		*LoginID,
		*FGenericPlatformHttp::UrlEncode(Settings->CallbackURL));

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::Default_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::RegistrateUserOAuth(const FString& Username, const FString& Password, const FString& Email, const FString& State, const FOnRequestSuccess& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Prepare request payload
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetStringField(TEXT("username"), Username);
	RequestDataJson->SetStringField(TEXT("password"), Password);
	RequestDataJson->SetStringField(TEXT("email"), Email);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	// Generate endpoint url
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString Endpoint = (Settings->UserDataStorage == EUserDataStorage::Xsolla) ? RegistrationEndpoint : ProxyRegistrationEndpoint;
	const FString Url = FString::Printf(TEXT("%s/user?response_type=code&client_id=%s&state=%s&redirect_uri=%s"),
		*LoginEndpointOAuth,
		*Settings->ClientID,
		*State,
		*BlankRedirectEndpoint);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::Default_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::AuthenticateUserJWT(const FString& Username, const FString& Password, bool bRememberMe, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Prepare request payload
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetStringField(TEXT("username"), Username);
	RequestDataJson->SetStringField(TEXT("password"), Password);
	RequestDataJson->SetBoolField(TEXT("remember_me"), bRememberMe);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	// Generate endpoint url
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString Endpoint = (Settings->UserDataStorage == EUserDataStorage::Xsolla) ? LoginEndpoint : ProxyLoginEndpoint;
	const FString Url = FString::Printf(TEXT("%s?projectId=%s&login_url=%s"),
		*Endpoint,
		*LoginID,
		*FGenericPlatformHttp::UrlEncode(Settings->CallbackURL));

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::UserLogin_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::AuthenticateUserOAuth(const FString& Username, const FString& Password, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Prepare request payload
	TSharedPtr<FJsonObject> RequestDataJson = MakeShareable(new FJsonObject());
	RequestDataJson->SetStringField(TEXT("username"), Username);
	RequestDataJson->SetStringField(TEXT("password"), Password);

	FString PostContent;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&PostContent);
	FJsonSerializer::Serialize(RequestDataJson.ToSharedRef(), Writer);

	// Generate endpoint url
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString Url = FString::Printf(TEXT("%s/login/token?client_id=%s&scope=offline"),
		*LoginEndpointOAuth,
		*Settings->ClientID);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::POST, PostContent);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::UserLoginOAuth_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::GetSocialAuthenticationUrlJWT(const FString& ProviderName, const FOnSocialUrlReceived& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Generate endpoint url
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString Url = FString::Printf(TEXT("%s/%s/login_url?projectId=%s&login_url=%s"),
		*LoginSocialEndpoint,
		*ProviderName,
		*LoginID,
		*FGenericPlatformHttp::UrlEncode(Settings->CallbackURL));

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::GET);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::SocialAuthUrl_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::GetSocialAuthenticationUrlOAuth(const FString& ProviderName, const FString& State, const FOnSocialUrlReceived& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Generate endpoint url
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	const FString Url = FString::Printf(TEXT("%s/social/%s/login_url?client_id=%s&redirect_uri=%s&response_type=code&state=%s&scope=offline"),
		*LoginEndpointOAuth,
		*ProviderName,
		*Settings->ClientID,
		*BlankRedirectEndpoint,
		*State);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url, EXsollaLoginRequestVerb::GET);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::SocialAuthUrl_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::AuthenticateWithSessionTicketJWT(const FString& ProviderName, const FString& AppId, const FString& SessionTicket, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Generate endpoint url
	FString Url = FString::Printf(TEXT("%s/%s?projectId=%s&app_id=%s&session_ticket=%s"),
		*CrossAuthEndpoint,
		*ProviderName,
		*LoginID,
		*AppId,
		*SessionTicket);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::CrossAuth_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::AuthenticateWithSessionTicketOAuth(const FString& ProviderName, const FString& AppId, const FString& SessionTicket, const FString& State, const FOnAuthUpdate& SuccessCallback, const FOnAuthError& ErrorCallback)
{
	// Generate endpoint url
	const UXsollaLoginSettings* Settings = FXsollaLoginModule::Get().GetSettings();
	FString Url = FString::Printf(TEXT("%s/social/%s/cross_auth?client_id=%s&response_type=code&redirect_uri=%s&state=%s&app_id=%s&scope=offline&session_ticket=%s&is_redirect=false"),
		*LoginEndpointOAuth,
		*ProviderName,
		*Settings->ClientID,
		*BlankRedirectEndpoint,
		*State,
		*AppId,
		*SessionTicket);

	TSharedRef<IHttpRequest> HttpRequest = CreateHttpRequest(Url);
	HttpRequest->OnProcessRequestComplete().BindUObject(this, &UXsollaLoginSubsystem::SessionTicketOAuth_HttpRequestComplete, SuccessCallback, ErrorCallback);
	HttpRequest->ProcessRequest();
}

void UXsollaLoginSubsystem::Default_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnRequestSuccess SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	SuccessCallback.ExecuteIfBound();
}

void UXsollaLoginSubsystem::UserLogin_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthUpdate SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	FString ErrorStr;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		static const FString LoginUrlFieldName = TEXT("login_url");
		if (JsonObject->HasTypedField<EJson::String>(LoginUrlFieldName))
		{
			FString LoginUrl = JsonObject.Get()->GetStringField(LoginUrlFieldName);
			FString UrlOptions = LoginUrl.RightChop(LoginUrl.Find(TEXT("?"))).Replace(TEXT("&"), TEXT("?"));

			LoginData.AuthToken.JWT = UGameplayStatics::ParseOption(UrlOptions, TEXT("token"));

			SaveData();

			UE_LOG(LogXsollaLogin, Log, TEXT("%s: Received token: %s"), *VA_FUNC_LINE, *LoginData.AuthToken.JWT);

			SuccessCallback.ExecuteIfBound(LoginData);

			return;
		}
		else
		{
			ErrorStr = FString::Printf(TEXT("Can't process response json: no field '%s' found"), *LoginUrlFieldName);
		}
	}
	else
	{
		ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	}

	// No success before so call the error callback
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

void UXsollaLoginSubsystem::UserLoginOAuth_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthUpdate SuccessCallback, FOnAuthError ErrorCallback)
{
	HandleOAuthTokenRequest(HttpRequest, HttpResponse, bSucceeded, ErrorCallback, SuccessCallback);
}

void UXsollaLoginSubsystem::TokenVerify_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthUpdate SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	// If no error happend so token is verified now
	LoginData.AuthToken.bIsVerified = true;
	SaveData();

	SuccessCallback.ExecuteIfBound(LoginData);
}

void UXsollaLoginSubsystem::SocialAuthUrl_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnSocialUrlReceived SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	FString ErrorStr;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		static const FString SocialUrlFieldName = TEXT("url");
		if (JsonObject->HasTypedField<EJson::String>(SocialUrlFieldName))
		{
			FString SocialnUrl = JsonObject.Get()->GetStringField(SocialUrlFieldName);
			SuccessCallback.ExecuteIfBound(SocialnUrl);
			return;
		}
		else
		{
			ErrorStr = FString::Printf(TEXT("Can't process response json: no field '%s' found"), *SocialUrlFieldName);
		}
	}
	else
	{
		ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	}

	// No success before so call the error callback
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

void UXsollaLoginSubsystem::CrossAuth_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthUpdate SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	FString ErrorStr;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		static const FString TokenFieldName = TEXT("token");
		if (JsonObject->HasTypedField<EJson::String>(TokenFieldName))
		{
			FString Token = JsonObject.Get()->GetStringField(TokenFieldName);

			LoginData.AuthToken.JWT = Token;

			SaveData();

			UE_LOG(LogXsollaLogin, Log, TEXT("%s: Received token: %s"), *VA_FUNC_LINE, *LoginData.AuthToken.JWT);

			SuccessCallback.ExecuteIfBound(LoginData);

			return;
		}
		else
		{
			ErrorStr = FString::Printf(TEXT("Can't process response json: no field '%s' found"), *TokenFieldName);
		}
	}
	else
	{
		ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	}

	// No success before so call the error callback
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

void UXsollaLoginSubsystem::UpdateUserAttributes_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnRequestSuccess SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	TArray<FXsollaUserAttribute> userAttributesData;
	if (FJsonObjectConverter::JsonArrayStringToUStruct(ResponseStr, &userAttributesData, 0, 0))
	{
		UserAttributes = userAttributesData;
		SuccessCallback.ExecuteIfBound();
		return;
	}

	// No success before so call the error callback
	FString ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

void UXsollaLoginSubsystem::AccountLinkingCode_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnCodeReceived SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	FString ErrorStr;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		static const FString AccountLinkingCode = TEXT("code");
		if (JsonObject->HasTypedField<EJson::String>(AccountLinkingCode))
		{
			FString Code = JsonObject.Get()->GetStringField(AccountLinkingCode);
			SuccessCallback.ExecuteIfBound(Code);
			return;
		}
		else
		{
			ErrorStr = FString::Printf(TEXT("Can't process response json: no field '%s' found"), *AccountLinkingCode);
		}
	}
	else
	{
		ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	}

	// No success before so call the error callback
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

void UXsollaLoginSubsystem::AuthConsoleAccountUser_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthUpdate SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	FString ErrorStr;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		static const FString TokenFieldName = TEXT("token");
		if (JsonObject->HasTypedField<EJson::String>(TokenFieldName))
		{
			FString Token = JsonObject.Get()->GetStringField(TokenFieldName);

			LoginData.AuthToken.JWT = Token;

			SaveData();

			UE_LOG(LogXsollaLogin, Log, TEXT("%s: Received token: %s"), *VA_FUNC_LINE, *LoginData.AuthToken.JWT);

			SuccessCallback.ExecuteIfBound(LoginData);

			return;
		}
		else
		{
			ErrorStr = FString::Printf(TEXT("Can't process response json: no field '%s' found"), *TokenFieldName);
		}
	}
	else
	{
		ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	}

	// No success before so call the error callback
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

void UXsollaLoginSubsystem::RefreshTokenOAuth_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthUpdate SuccessCallback, FOnAuthError ErrorCallback)
{
	HandleOAuthTokenRequest(HttpRequest, HttpResponse, bSucceeded, ErrorCallback, SuccessCallback);
}

void UXsollaLoginSubsystem::SessionTicketOAuth_HttpRequestComplete(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthUpdate SuccessCallback, FOnAuthError ErrorCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	FString ErrorStr;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		static const FString LoginUrlFieldName = TEXT("login_url");
		if (JsonObject->HasTypedField<EJson::String>(LoginUrlFieldName))
		{
			FString LoginUrlRaw = JsonObject.Get()->GetStringField(LoginUrlFieldName);
			FString LoginUrl = FGenericPlatformHttp::UrlDecode(LoginUrlRaw);
			FString UrlOptions = LoginUrl.RightChop(LoginUrl.Find(TEXT("?"))).Replace(TEXT("&"), TEXT("?"));

			FString Code = UGameplayStatics::ParseOption(UrlOptions, TEXT("code"));

			UE_LOG(LogXsollaLogin, Log, TEXT("%s: Received code: %s"), *VA_FUNC_LINE, *Code);

			ExchangeAuthenticationCodeToToken(Code, SuccessCallback, ErrorCallback);

			return;
		}
		else
		{
			ErrorStr = FString::Printf(TEXT("Can't process response json: no field '%s' found"), *LoginUrlFieldName);
		}
	}
	else
	{
		ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	}

	// No success before so call the error callback
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

void UXsollaLoginSubsystem::HandleOAuthTokenRequest(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthError& ErrorCallback, FOnAuthUpdate& SuccessCallback)
{
	if (HandleRequestError(HttpRequest, HttpResponse, bSucceeded, ErrorCallback))
	{
		return;
	}

	FString ResponseStr = HttpResponse->GetContentAsString();
	UE_LOG(LogXsollaLogin, Verbose, TEXT("%s: Response: %s"), *VA_FUNC_LINE, *ResponseStr);

	FString ErrorStr;

	TSharedPtr<FJsonObject> JsonObject;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
	if (FJsonSerializer::Deserialize(Reader, JsonObject))
	{
		static const FString AccessTokenFieldName = TEXT("access_token");
		if (JsonObject->HasTypedField<EJson::String>(AccessTokenFieldName))
		{
			LoginData.AuthToken.JWT = JsonObject->GetStringField(AccessTokenFieldName);
			LoginData.AuthToken.RefreshToken = JsonObject->GetStringField(TEXT("refresh_token"));

			SaveData();

			UE_LOG(LogXsollaLogin, Log, TEXT("%s: Received token: %s"), *VA_FUNC_LINE, *LoginData.AuthToken.JWT);

			SuccessCallback.ExecuteIfBound(LoginData);

			return;
		}
		else
		{
			ErrorStr = FString::Printf(TEXT("Can't process response json: no field '%s' found"), *AccessTokenFieldName);
		}
	}
	else
	{
		ErrorStr = FString::Printf(TEXT("Can't deserialize response json: "), *ResponseStr);
	}

	// No success before so call the error callback
	ErrorCallback.ExecuteIfBound(TEXT("204"), ErrorStr);
}

bool UXsollaLoginSubsystem::HandleRequestError(FHttpRequestPtr HttpRequest, FHttpResponsePtr HttpResponse, bool bSucceeded, FOnAuthError ErrorCallback)
{
	FString ErrorStr;
	FString ErrorCode = TEXT("204");
	FString ResponseStr = TEXT("invalid");

	if (bSucceeded && HttpResponse.IsValid())
	{
		ResponseStr = HttpResponse->GetContentAsString();

		if (!EHttpResponseCodes::IsOk(HttpResponse->GetResponseCode()))
		{
			ErrorCode = FString::Printf(TEXT("%d"), HttpResponse->GetResponseCode());
			ErrorStr = FString::Printf(TEXT("Invalid response. code=%d error=%s"), HttpResponse->GetResponseCode(), *ResponseStr);

			// Example: {"error":{"code":"003-003","description":"The username is already taken"}}
			TSharedPtr<FJsonObject> JsonObject;
			TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(*ResponseStr);
			if (FJsonSerializer::Deserialize(Reader, JsonObject))
			{
				static const FString ErrorFieldName = TEXT("error");
				if (JsonObject->HasTypedField<EJson::Object>(ErrorFieldName))
				{
					TSharedPtr<FJsonObject> ErrorObject = JsonObject.Get()->GetObjectField(ErrorFieldName);
					ErrorCode = ErrorObject.Get()->GetStringField(TEXT("code"));
					ErrorStr = ErrorObject.Get()->GetStringField(TEXT("description"));
				}
				else
				{
					ErrorStr = FString::Printf(TEXT("Can't deserialize error json: no field '%s' found"), *ErrorFieldName);
				}
			}
			else
			{
				ErrorStr = TEXT("Can't deserialize error json");
			}
		}
	}
	else
	{
		ErrorStr = TEXT("No response");
	}

	if (!ErrorStr.IsEmpty())
	{
		UE_LOG(LogXsollaLogin, Warning, TEXT("%s: request failed (%s): %s"), *VA_FUNC_LINE, *ErrorStr, *ResponseStr);
		ErrorCallback.ExecuteIfBound(ErrorCode, ErrorStr);
		return true;
	}

	return false;
}

TSharedRef<IHttpRequest> UXsollaLoginSubsystem::CreateHttpRequest(const FString& Url, const EXsollaLoginRequestVerb Verb, const FString& Content, const FString& AuthToken)
{
	TSharedRef<IHttpRequest> HttpRequest = FHttpModule::Get().CreateRequest();

	// Temporal solution with headers processing on server-side #37
	const FString MetaUrl = FString::Printf(TEXT("%sengine=ue4&engine_v=%s&sdk=login&sdk_v=%s"),
		Url.Contains(TEXT("?")) ? TEXT("&") : TEXT("?"),
		ENGINE_VERSION_STRING,
		XSOLLA_LOGIN_VERSION);
	HttpRequest->SetURL(Url + MetaUrl);

	switch (Verb)
	{
	case EXsollaLoginRequestVerb::GET:
		HttpRequest->SetVerb(TEXT("GET"));

		// Check that we doen't provide content with GET request
		if (!Content.IsEmpty())
		{
			UE_LOG(LogXsollaLogin, Warning, TEXT("%s: Request content is not empty for GET request. Maybe you should use POST one?"), *VA_FUNC_LINE);
		}
		break;

	case EXsollaLoginRequestVerb::POST:
		HttpRequest->SetVerb(TEXT("POST"));
		break;

	case EXsollaLoginRequestVerb::PUT:
		HttpRequest->SetVerb(TEXT("PUT"));
		break;

	case EXsollaLoginRequestVerb::DELETE:
		HttpRequest->SetVerb(TEXT("DELETE"));
		break;

	default:
		unimplemented();
	}

	if (!Content.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
		HttpRequest->SetContentAsString(Content);
	}

	if (!AuthToken.IsEmpty())
	{
		HttpRequest->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AuthToken));
	}

	// Xsolla meta
	HttpRequest->SetHeader(TEXT("X-ENGINE"), TEXT("UE4"));
	HttpRequest->SetHeader(TEXT("X-ENGINE-V"), ENGINE_VERSION_STRING);
	HttpRequest->SetHeader(TEXT("X-SDK"), TEXT("LOGIN"));
	HttpRequest->SetHeader(TEXT("X-SDK-V"), XSOLLA_LOGIN_VERSION);

	return HttpRequest;
}

FString UXsollaLoginSubsystem::EncodeFormData(TSharedPtr<FJsonObject> FormDataJson)
{
	FString EncodedFormData = "";
	uint16 ParamIndex = 0;

	for (auto FormDataIt = FormDataJson->Values.CreateIterator(); FormDataIt; ++FormDataIt)
	{
		FString Key = FormDataIt.Key();
		FString Value = FormDataIt.Value().Get()->AsString();

		if (!Key.IsEmpty() && !Value.IsEmpty())
		{
			EncodedFormData += ParamIndex == 0 ? "" : "&";
			EncodedFormData += FGenericPlatformHttp::UrlEncode(Key) + "=" + FGenericPlatformHttp::UrlEncode(Value);
		}

		ParamIndex++;
	}

	return EncodedFormData;
}

void UXsollaLoginSubsystem::SetStringArrayField(TSharedPtr<FJsonObject> Object, const FString& FieldName, const TArray<FString>& Array) const
{
	TArray<TSharedPtr<FJsonValue>> StringJsonArray;
	for (auto Item : Array)
	{
		StringJsonArray.Push(MakeShareable(new FJsonValueString(Item)));
	}

	Object->SetArrayField(FieldName, StringJsonArray);
}

bool UXsollaLoginSubsystem::ParseTokenPayload(const FString& Token, TSharedPtr<FJsonObject>& PayloadJsonObject) const
{
	TArray<FString> TokenParts;
	Token.ParseIntoArray(TokenParts, TEXT("."));

	FString PayloadStr;
	if (!FBase64::Decode(TokenParts[1], PayloadStr))
	{
		return false;
	}

	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PayloadStr);
	if (!FJsonSerializer::Deserialize(Reader, PayloadJsonObject))
	{
		return false;
	}

	return true;
}

FString UXsollaLoginSubsystem::GetTargetPlatformName(EXsollaTargetPlatform Platform)
{
	FString platform;

	switch (Platform)
	{
	case EXsollaTargetPlatform::PlaystationNetwork:
		platform = TEXT("playstation_network");
		break;

	case EXsollaTargetPlatform::XboxLive:
		platform = TEXT("xbox_live");
		break;

	case EXsollaTargetPlatform::Xsolla:
		platform = TEXT("xsolla");
		break;

	case EXsollaTargetPlatform::PcStandalone:
		platform = TEXT("pc_standalone");
		break;

	case EXsollaTargetPlatform::NintendoShop:
		platform = TEXT("nintendo_shop");
		break;

	case EXsollaTargetPlatform::GooglePlay:
		platform = TEXT("google_play");
		break;

	case EXsollaTargetPlatform::AppStoreIos:
		platform = TEXT("app_store_ios");
		break;

	case EXsollaTargetPlatform::AndroidStandalone:
		platform = TEXT("android_standalone");
		break;

	case EXsollaTargetPlatform::IosStandalone:
		platform = TEXT("ios_standalone");
		break;

	case EXsollaTargetPlatform::AndroidOther:
		platform = TEXT("android_other");
		break;

	case EXsollaTargetPlatform::IosOther:
		platform = TEXT("ios_other");
		break;

	case EXsollaTargetPlatform::PcOther:
		platform = TEXT("pc_other");
		break;

	default:
		platform = TEXT("");
	}

	return platform;
}

FXsollaLoginData UXsollaLoginSubsystem::GetLoginData()
{
	return LoginData;
}

void UXsollaLoginSubsystem::DropLoginData(bool ClearCache)
{
	// Drop saved data in memory
	LoginData = FXsollaLoginData();

	if (ClearCache)
	{
		// Drop saved data in cache
		UXsollaLoginSave::Save(LoginData);
	}
}

FString UXsollaLoginSubsystem::GetUserId(const FString& Token)
{
	TSharedPtr<FJsonObject> PayloadJsonObject;
	if (!ParseTokenPayload(Token, PayloadJsonObject))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: Can't parse token payload"), *VA_FUNC_LINE);
		return FString();
	}

	FString UserId;
	if (!PayloadJsonObject->TryGetStringField(TEXT("sub"), UserId))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: Can't find user ID in token payload"), *VA_FUNC_LINE);
		return FString();
	}

	return UserId;
}

FString UXsollaLoginSubsystem::GetTokenProvider(const FString& Token)
{
	TSharedPtr<FJsonObject> PayloadJsonObject;
	if (!ParseTokenPayload(Token, PayloadJsonObject))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: Can't parse token payload"), *VA_FUNC_LINE);
		return FString();
	}

	FString Provider;
	if (!PayloadJsonObject->TryGetStringField(TEXT("provider"), Provider))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: Can't find provider in token payload"), *VA_FUNC_LINE);
		return FString();
	}

	return Provider;
}

FString UXsollaLoginSubsystem::GetTokenParameter(const FString& Token, const FString& Parameter)
{
	TSharedPtr<FJsonObject> PayloadJsonObject;
	if (!ParseTokenPayload(Token, PayloadJsonObject))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: Can't parse token payload"), *VA_FUNC_LINE);
		return FString();
	}

	FString ParameterValue;
	if (!PayloadJsonObject->TryGetStringField(Parameter, ParameterValue))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: Can't find parameter %s in token payload"), *VA_FUNC_LINE, *Parameter);
		return FString();
	}

	return ParameterValue;
}

bool UXsollaLoginSubsystem::IsMasterAccount(const FString& Token)
{
	TSharedPtr<FJsonObject> PayloadJsonObject;
	if (!ParseTokenPayload(Token, PayloadJsonObject))
	{
		UE_LOG(LogXsollaLogin, Error, TEXT("%s: Can't parse token payload"), *VA_FUNC_LINE);
		return false;
	}

	bool ParameterValue = false;
	if (!PayloadJsonObject->TryGetBoolField(TEXT("is_master"), ParameterValue))
	{
		return false;
	}

	return ParameterValue;
}

void UXsollaLoginSubsystem::LoadSavedData()
{
	LoginData = UXsollaLoginSave::Load();
}

void UXsollaLoginSubsystem::SaveData()
{
	if (LoginData.bRememberMe)
	{
		UXsollaLoginSave::Save(LoginData);
	}
	else
	{
		// Don't drop cache in memory but reset save file
		UXsollaLoginSave::Save(FXsollaLoginData());
	}
}

FString UXsollaLoginSubsystem::GetPendingSocialAuthenticationUrl() const
{
	return PendingSocialAuthenticationUrl;
}

TArray<FXsollaUserAttribute> UXsollaLoginSubsystem::GetUserAttributes()
{
	return UserAttributes;
}

#undef LOCTEXT_NAMESPACE