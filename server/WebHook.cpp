#include <sstream>
#include <unordered_map>
#include <mutex>
#include "System.h"
#include "jsoncpp/json.h"
#include "Util/logger.h"
#include "Util/util.h"
#include "Util/onceToken.h"
#include "Util/NoticeCenter.h"
#include "Common/config.h"
#include "Common/MediaSource.h"
#include "Http/HttpRequester.h"
#include "Network/TcpSession.h"
#include "Rtsp/RtspSession.h"

using namespace Json;
using namespace toolkit;
using namespace mediakit;

//支持json或urlencoded方式传输参数
#define JSON_ARGS

#ifdef JSON_ARGS
typedef Value ArgsType;
#else
typedef HttpArgs ArgsType;
#endif

namespace Hook {
#define HOOK_FIELD "hook."

	const char kEnable[]	   = HOOK_FIELD "enable";
	const char kTimeoutSec[]       = HOOK_FIELD "timeoutSec";
	const char kOnPublish[]	= HOOK_FIELD "on_publish";
	const char kOnPlay[]	   = HOOK_FIELD "on_play";
	const char kOnFlowReport[]     = HOOK_FIELD "on_flow_report";
	const char kOnRtspRealm[]      = HOOK_FIELD "on_rtsp_realm";
	const char kOnRtspAuth[]       = HOOK_FIELD "on_rtsp_auth";
	const char kOnStreamChanged[]  = HOOK_FIELD "on_stream_changed";
	const char kOnStreamNotFound[] = HOOK_FIELD "on_stream_not_found";
	const char kOnRecordMp4[]      = HOOK_FIELD "on_record_mp4";
	const char kAdminParams[]      = HOOK_FIELD "admin_params";

	onceToken token(
		[]() {
			mINI::Instance()[kEnable]	   = false;
			mINI::Instance()[kTimeoutSec]       = 10;
			mINI::Instance()[kOnPublish]	= "http://127.0.0.1/index/hook/on_publish";
			mINI::Instance()[kOnPlay]	   = "http://127.0.0.1/index/hook/on_play";
			mINI::Instance()[kOnFlowReport]     = "http://127.0.0.1/index/hook/on_flow_report";
			mINI::Instance()[kOnRtspRealm]      = "http://127.0.0.1/index/hook/on_rtsp_realm";
			mINI::Instance()[kOnRtspAuth]       = "http://127.0.0.1/index/hook/on_rtsp_auth";
			mINI::Instance()[kOnStreamChanged]  = "http://127.0.0.1/index/hook/on_stream_changed";
			mINI::Instance()[kOnStreamNotFound] = "http://127.0.0.1/index/hook/on_stream_not_found";
			mINI::Instance()[kOnRecordMp4]      = "http://127.0.0.1/index/hook/on_record_mp4";
			mINI::Instance()[kAdminParams]      = "secret=035c73f7-bb6b-4889-a715-d9eb2d1925cc";
		},
		nullptr);
} // namespace Hook

static void parse_http_response(const SockException& ex, const string& status, const HttpClient::HttpHeader& header, const string& strRecvBody, const function<void(const Value&, const string&)>& fun)
{
	if (ex) {
		auto errStr = StrPrinter << "[network err]:" << ex.what() << endl;
		fun(Json::nullValue, errStr);
		return;
	}
	if (status != "200") {
		auto errStr = StrPrinter << "[bad http status code]:" << status << endl;
		fun(Json::nullValue, errStr);
		return;
	}
	try {
		stringstream ss(strRecvBody);
		Value	result;
		ss >> result;
		if (result["code"].asInt() != 0) {
			auto errStr = StrPrinter << "[json code]:"
						 << "code=" << result["code"] << ",msg=" << result["msg"] << endl;
			fun(Json::nullValue, errStr);
			return;
		}
		fun(result, "");
	} catch (std::exception& ex) {
		auto errStr = StrPrinter << "[parse json failed]:" << ex.what() << endl;
		fun(Json::nullValue, errStr);
	}
}

string to_string(const Value& value)
{
	return value.toStyledString();
}

string to_string(const HttpArgs& value)
{
	return value.make();
}

const char* getContentType(const Value& value)
{
	return "application/json";
}

const char* getContentType(const HttpArgs& value)
{
	return "application/x-www-form-urlencoded";
}

static void do_http_hook(const string& url, const ArgsType& body, const function<void(const Value&, const string&)>& fun)
{
	GET_CONFIG_AND_REGISTER(float, hook_timeoutSec, Hook::kTimeoutSec);
	HttpRequester::Ptr requester(new HttpRequester);
	requester->setMethod("POST");
	auto bodyStr = to_string(body);
	requester->setBody(bodyStr);
	requester->addHeader("Content-Type", getContentType(body));
	std::shared_ptr<Ticker> pTicker(new Ticker);
	requester->startRequester(
		url,
		[url, fun, bodyStr, requester, pTicker](const SockException& ex, const string& status, const HttpClient::HttpHeader& header, const string& strRecvBody) {
			onceToken token(nullptr, [&]() { const_cast<HttpRequester::Ptr&>(requester).reset(); });
			parse_http_response(ex, status, header, strRecvBody, [&](const Value& obj, const string& err) {
				if (fun) {
					fun(obj, err);
				}
				if (!err.empty()) {
					WarnL << "hook " << url << " " << pTicker->elapsedTime() << "ms,failed" << err << ":" << bodyStr;
				}
				else if (pTicker->elapsedTime() > 500) {
					DebugL << "hook " << url << " " << pTicker->elapsedTime() << "ms,success:" << bodyStr;
				}
			});
		},
		hook_timeoutSec);
}

static ArgsType make_json(const MediaInfo& args)
{
	ArgsType body;
	body["schema"] = args._schema;
	body["vhost"]  = args._vhost;
	body["app"]    = args._app;
	body["stream"] = args._streamid;
	body["params"] = args._param_strs;
	return std::move(body);
}

void installWebHook()
{
	GET_CONFIG_AND_REGISTER(bool, hook_enable, Hook::kEnable);
	GET_CONFIG_AND_REGISTER(string, hook_publish, Hook::kOnPublish);
	GET_CONFIG_AND_REGISTER(string, hook_play, Hook::kOnPlay);
	GET_CONFIG_AND_REGISTER(string, hook_flowreport, Hook::kOnFlowReport);
	GET_CONFIG_AND_REGISTER(string, hook_adminparams, Hook::kAdminParams);
	GET_CONFIG_AND_REGISTER(string, hook_rtsp_realm, Hook::kOnRtspRealm);
	GET_CONFIG_AND_REGISTER(string, hook_rtsp_auth, Hook::kOnRtspAuth);
	GET_CONFIG_AND_REGISTER(string, hook_stream_chaned, Hook::kOnStreamChanged);
	GET_CONFIG_AND_REGISTER(string, hook_stream_not_found, Hook::kOnStreamNotFound);
	GET_CONFIG_AND_REGISTER(string, hook_record_mp4, Hook::kOnRecordMp4);

	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaPublish, [](BroadcastMediaPublishArgs) {
		if (!hook_enable || args._param_strs == hook_adminparams || hook_publish.empty()) {
			invoker("");
			return;
		}
		//异步执行该hook api，防止阻塞NoticeCenter
		auto body    = make_json(args);
		body["ip"]   = sender.get_peer_ip();
		body["port"] = sender.get_peer_port();
		body["id"]   = sender.getIdentifier();
		//执行hook
		do_http_hook(hook_publish, body, [invoker](const Value& obj, const string& err) { invoker(err); });
	});

	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaPlayed, [](BroadcastMediaPlayedArgs) {
		if (!hook_enable || args._param_strs == hook_adminparams || hook_play.empty()) {
			invoker("");
			return;
		}
		auto body    = make_json(args);
		body["ip"]   = sender.get_peer_ip();
		body["port"] = sender.get_peer_port();
		body["id"]   = sender.getIdentifier();
		//执行hook
		do_http_hook(hook_play, body, [invoker](const Value& obj, const string& err) { invoker(err); });
	});

	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastFlowReport, [](BroadcastFlowReportArgs) {
		if (!hook_enable || args._param_strs == hook_adminparams || hook_flowreport.empty()) {
			return;
		}
		auto body	  = make_json(args);
		body["ip"]	 = sender.get_peer_ip();
		body["port"]       = sender.get_peer_port();
		body["id"]	 = sender.getIdentifier();
		body["totalBytes"] = (Json::UInt64)totalBytes;
		body["duration"]   = (Json::UInt64)totalDuration;
		body["player"]     = isPlayer;
		//执行hook
		do_http_hook(hook_flowreport, body, nullptr);
	});

	static const string unAuthedRealm = "unAuthedRealm";

	//监听kBroadcastOnGetRtspRealm事件决定rtsp链接是否需要鉴权(传统的rtsp鉴权方案)才能访问
	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastOnGetRtspRealm, [](BroadcastOnGetRtspRealmArgs) {
		if (!hook_enable || args._param_strs == hook_adminparams || hook_rtsp_realm.empty()) {
			//无需认证
			invoker("");
			return;
		}
		auto body    = make_json(args);
		body["ip"]   = sender.get_peer_ip();
		body["port"] = sender.get_peer_port();
		body["id"]   = sender.getIdentifier();
		//执行hook
		do_http_hook(hook_rtsp_realm, body, [invoker](const Value& obj, const string& err) {
			if (!err.empty()) {
				//如果接口访问失败，那么该rtsp流认证失败
				invoker(unAuthedRealm);
				return;
			}
			invoker(obj["realm"].asString());
		});
	});

	//监听kBroadcastOnRtspAuth事件返回正确的rtsp鉴权用户密码
	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastOnRtspAuth, [](BroadcastOnRtspAuthArgs) {
		if (unAuthedRealm == realm || !hook_enable || hook_rtsp_auth.empty()) {
			//认证失败
			invoker(false, makeRandStr(12));
			return;
		}
		auto body		= make_json(args);
		body["ip"]		= sender.get_peer_ip();
		body["port"]		= sender.get_peer_port();
		body["id"]		= sender.getIdentifier();
		body["user_name"]       = user_name;
		body["must_no_encrypt"] = must_no_encrypt;
		body["realm"]		= realm;
		//执行hook
		do_http_hook(hook_rtsp_auth, body, [invoker](const Value& obj, const string& err) {
			if (!err.empty()) {
				//认证失败
				invoker(false, makeRandStr(12));
				return;
			}
			invoker(obj["encrypted"].asBool(), obj["passwd"].asString());
		});
	});

	//监听rtsp、rtmp源注册或注销事件
	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastMediaChanged, [](BroadcastMediaChangedArgs) {
		if (!hook_enable || hook_stream_chaned.empty()) {
			return;
		}
		ArgsType body;
		body["regist"] = bRegist;
		body["schema"] = schema;
		body["vhost"]  = vhost;
		body["app"]    = app;
		body["stream"] = stream;
		//执行hook
		do_http_hook(hook_stream_chaned, body, nullptr);
	});

	//监听播放失败(未找到特定的流)事件
	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastNotFoundStream, [](BroadcastNotFoundStreamArgs) {
		if (!hook_enable || hook_stream_not_found.empty()) {
			return;
		}
		auto body    = make_json(args);
		body["ip"]   = sender.get_peer_ip();
		body["port"] = sender.get_peer_port();
		body["id"]   = sender.getIdentifier();
		//执行hook
		do_http_hook(hook_stream_not_found, body, nullptr);
	});

#ifdef ENABLE_MP4V2
	//录制mp4文件成功后广播
	NoticeCenter::Instance().addListener(nullptr, Broadcast::kBroadcastRecordMP4, [](BroadcastRecordMP4Args) {
		if (!hook_enable || hook_record_mp4.empty()) {
			return;
		}
		ArgsType body;
		body["start_time"] = (Json::UInt64)info.ui64StartedTime;
		body["time_len"]   = (Json::UInt64)info.ui64TimeLen;
		body["file_size"]  = (Json::UInt64)info.ui64FileSize;
		body["file_path"]  = info.strFilePath;
		body["file_name"]  = info.strFileName;
		body["folder"]     = info.strFolder;
		body["url"]	= info.strUrl;
		body["app"]	= info.strAppName;
		body["stream"]     = info.strStreamId;
		body["vhost"]      = info.strVhost;
		//执行hook
		do_http_hook(hook_record_mp4, body, nullptr);
	});
#endif // ENABLE_MP4V2
}

void unInstallWebHook() {}