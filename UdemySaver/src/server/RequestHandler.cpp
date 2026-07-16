#include "RequestHandler.h"

#ifdef _WIN32
#define NOMINMAX
#include <Windows.h>
#endif

#include <nlohmann/json.hpp>
#include <boost/beast/http/status.hpp>

#include <curl/curl.h>

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <unordered_map>
#include <algorithm>
#include <stdexcept>
#include <thread>
#include <vector>
#include <filesystem>
#include <map>
#include <functional>
#include <cstdio>
#include <atomic>
#include <cerrno>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

// utils/helper
#include "FFmpegHelper.h"
#include "Helper.h"

using boost::beast::http::status;
using json = nlohmann::json;

namespace {
	constexpr const char* kDefaultUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";
	constexpr const char* kDrmProtectedMessage = "DRM-protected content cannot be downloaded.";

	bool is_drm_protected_asset(const nlohmann::json& asset) {
		if (!asset.is_object()) return false;
		auto license_token = asset.find("media_license_token");
		return asset.value("course_is_drmed", false) ||
			asset.value("is_drmed", false) ||
			asset.value("drm_protected", false) ||
			(license_token != asset.end() && license_token->is_string() && !license_token->get<std::string>().empty());
	}

	bool is_drm_protected_playlist(const std::string& playlist) {
		std::string value = playlist;
		std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return (value.find("#ext-x-key") != std::string::npos && value.find("method=none") == std::string::npos) ||
			value.find("#ext-x-session-key") != std::string::npos ||
			value.find("widevine") != std::string::npos ||
			value.find("playready") != std::string::npos ||
			value.find("fairplay") != std::string::npos;
	}
}

std::vector<std::string> get_browser_headers() {
	return {
		"sec-ch-ua: \"Chromium\";v=\"122\", \"Not(A:Brand\";v=\"24\", \"Google Chrome\";v=\"122\"",
		"sec-ch-ua-mobile: ?0",
		"sec-ch-ua-platform: \"Windows\"",
		"sec-fetch-dest: empty",
		"sec-fetch-mode: cors",
		"sec-fetch-site: same-origin",
		"Upgrade-Insecure-Requests: 1",
		"Pragma: no-cache",
		"Cache-Control: no-cache",
		"Accept-Language: en-US,en;q=0.9,tr;q=0.8"
	};
}

struct CurlHandle { CURL* h = nullptr; CurlHandle() { h = curl_easy_init(); } ~CurlHandle() { if (h) curl_easy_cleanup(h); } };

size_t RequestHandler::header_probe_cb(char* buffer, size_t size, size_t nitems, void* userdata) {
	size_t total = size * nitems;
	HeaderProbe* hp = reinterpret_cast<HeaderProbe*>(userdata);
	std::string line(buffer, buffer + total);

	std::string low = line; std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return (char)std::tolower(c); });

	const std::string cl = "content-length:";
	if (low.rfind(cl, 0) == 0)
	{
		std::string num = line.substr((int)cl.size());
		try { hp->content_length = std::stoll(Helper::trim(num)); }
		catch (...) {}
	}

	// content-range: bytes 0-0/12345
	const std::string cr = "content-range:";
	if (low.rfind(cr, 0) == 0)
	{
		auto slash = line.find('/');
		if (slash != std::string::npos)
		{
			std::string total_s = line.substr(slash + 1);
			try { hp->content_range_total = std::stoll(Helper::trim(total_s)); }
			catch (...) {}
		}
	}
	return total;
}


bool RequestHandler::probe_content_length(const std::string& url,
	const std::vector<std::string>& extra_headers,
	long long& out_bytes, std::string& err) {
	out_bytes = -1; err.clear();
	CurlHandle ch; if (!ch.h) { err = "curl init failed"; return false; }

	// header list
	struct curl_slist* hdr = nullptr;
	for (auto& h : extra_headers) hdr = curl_slist_append(hdr, h.c_str());

	HeaderProbe hp;
	curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(ch.h, CURLOPT_USERAGENT, kDefaultUserAgent);
	curl_easy_setopt(ch.h, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(ch.h, CURLOPT_NOBODY, 1L);                       // HEAD
	curl_easy_setopt(ch.h, CURLOPT_HEADERFUNCTION, header_probe_cb);
	curl_easy_setopt(ch.h, CURLOPT_HEADERDATA, &hp);
	curl_easy_setopt(ch.h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
	curl_easy_setopt(ch.h, CURLOPT_TIMEOUT_MS, 20000L);
	if (!proxy_.empty()) {
		curl_easy_setopt(ch.h, CURLOPT_PROXY, proxy_.c_str());
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	else {
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 2L);
	}

	CURLcode rc = curl_easy_perform(ch.h);

	if (rc == CURLE_OK && hp.content_length >= 0)
	{
		out_bytes = hp.content_length;
		if (hdr) curl_slist_free_all(hdr);
		return true;
	}

	hp = HeaderProbe{};
	curl_easy_setopt(ch.h, CURLOPT_NOBODY, 0L);
	curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, Helper::write_discard);
	curl_easy_setopt(ch.h, CURLOPT_RANGE, "0-0");
	rc = curl_easy_perform(ch.h);

	if (hdr) curl_slist_free_all(hdr);

	if (rc == CURLE_OK && hp.content_range_total >= 0)
	{
		out_bytes = hp.content_range_total;
		return true;
	}

	err = curl_easy_strerror(rc);
	return false;
}

std::string RequestHandler::pick_from_asset_for_size(const json& a, const std::string& pref) {
	if (!a.is_object()) return {};

	std::map<int, std::string> mp4_by_quality;
	std::map<int, std::string> hls_by_quality;
	std::string fallback_mp4;
	std::string hls_candidate;

	if (a.contains("download_urls") && a["download_urls"].is_object()) {
		auto it = a["download_urls"].find("Video");
		if (it != a["download_urls"].end() && it->is_array()) {
			for (auto& v : *it) {
				std::string file = v.value("file", "");
				int q = Helper::extract_quality_value(v.value("label", ""));
				if (!file.empty()) {
					if (fallback_mp4.empty()) fallback_mp4 = file;
					if (q > 0) mp4_by_quality[q] = file;
				}
			}
		}
	}

	if (a.contains("stream_urls") && a["stream_urls"].is_object()) {
		auto it = a["stream_urls"].find("Video");
		if (it != a["stream_urls"].end() && it->is_array()) {
			for (auto& v : *it) {
				if (v.value("type", "") == "video/mp4") {
					std::string file = v.value("file", "");
					int q = Helper::extract_quality_value(v.value("label", ""));
					if (!file.empty() && q > 0) mp4_by_quality[q] = file;
				}
				else if (v.value("type", "") == "application/x-mpegURL") {
					hls_candidate = v.value("file", "");
				}
			}
		}
	}

	int wanted = Helper::extract_quality_value(pref);
	if (!mp4_by_quality.empty()) {
		if (pref == "Highest" || wanted >= 1080) return mp4_by_quality.rbegin()->second;
		if (pref == "Lowest") return mp4_by_quality.begin()->second;

		auto it = mp4_by_quality.find(wanted);
		if (it != mp4_by_quality.end()) return it->second;

		auto it_up = mp4_by_quality.lower_bound(wanted);
		if (it_up != mp4_by_quality.end()) return it_up->second;

		return mp4_by_quality.rbegin()->second;
	}

	if (!hls_candidate.empty()) return hls_candidate;
	if (a.contains("hls_url") && a["hls_url"].is_string()) return a["hls_url"].get<std::string>();

	return fallback_mp4;
}

// ---------------- ctor / dtor ----------------
RequestHandler::RequestHandler(std::string webroot)
	: webroot_(std::move(webroot)), api_base_("https://www.udemy.com") {
	curl_global_init(CURL_GLOBAL_DEFAULT);
	avformat_network_init();
	load_settings();

	worker_ = std::thread([this] { worker_loop(); });
}

RequestHandler::~RequestHandler() {
	{
		std::lock_guard<std::mutex> lk(mtx_);
		stop_ = true;
	}
	cv_.notify_all();
	if (worker_.joinable()) worker_.join();

	curl_global_cleanup();
	avformat_network_deinit();
}

// ---------------- settings.ini ----------------
void RequestHandler::load_settings() {
	std::unordered_map<std::string, std::string> kv;

	std::string ini = Helper::read_file_utf8("settings.ini");
	if (ini.empty())
	{
		std::ofstream f("settings.ini");
		if (f)
		{
			f << "# UdemySaver settings\n";
			f << "udemy_access_token=\n";
			f << "udemy_client_id=\n";
			f << "udemy_api_base=https://www.udemy.com\n";
			f << "# http_proxy=\n";
			f << "download_subtitles=true\n";
			f << "download_assets=true\n";
		}

		token_.clear();
		client_id_.clear();
		api_base_ = "https://www.udemy.com";
		proxy_.clear();
		download_subtitles_ = true;
		download_assets_ = true;
		return;
	}

	std::istringstream is(ini);
	std::string line;
	while (std::getline(is, line))
	{
		line = Helper::trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';') continue;
		auto pos = line.find('=');
		if (pos == std::string::npos) continue;
		auto key = Helper::trim(line.substr(0, pos));
		auto val = Helper::trim(line.substr(pos + 1));
		if (!val.empty() && (val.front() == '"' || val.front() == '\''))
		{
			if (val.size() >= 2 && val.back() == val.front())
				val = val.substr(1, val.size() - 2);
		}
		std::transform(key.begin(), key.end(), key.begin(), ::tolower);
		kv[key] = val;
	}

	if (kv.count("udemy_access_token")) token_ = kv["udemy_access_token"];
	else if (kv.count("access_token"))  token_ = kv["access_token"];

	if (kv.count("udemy_client_id")) client_id_ = kv["udemy_client_id"];
	else if (kv.count("client_id"))  client_id_ = kv["client_id"];

	if (kv.count("udemy_api_base")) api_base_ = kv["udemy_api_base"];
	else if (kv.count("api_base"))   api_base_ = kv["api_base"];

	api_host_ = Helper::extract_host(api_base_);

	if (kv.count("http_proxy")) proxy_ = kv["http_proxy"];
	else if (kv.count("proxy")) proxy_ = kv["proxy"];

	if (kv.count("download_subtitles"))
	{
		std::string v = kv["download_subtitles"];
		std::transform(v.begin(), v.end(), v.begin(), ::tolower);
		download_subtitles_ = (v == "1" || v == "true" || v == "yes" || v == "on");
	}

	if (kv.count("download_assets"))
	{
		std::string v = kv["download_assets"];
		std::transform(v.begin(), v.end(), v.begin(), ::tolower);
		download_assets_ = (v == "1" || v == "true" || v == "yes" || v == "on");
	}
}

// ---------------- Udemy GET ----------------
std::string RequestHandler::udemy_get(const std::string& url, long timeout_ms) {
	if (token_.empty()) throw std::runtime_error("no_token");

	CURL* curl = curl_easy_init();
	if (!curl) throw std::runtime_error("curl_init_failed");

	struct CurlHeaders { curl_slist* list = nullptr; ~CurlHeaders() { if (list) curl_slist_free_all(list); } } hdr;

	std::string body;
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, kDefaultUserAgent);
	curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); // enable gzip/deflate
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, timeout_ms);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, Helper::write_to_string);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
	// When using a proxy (like mitmproxy), disable SSL verification
	// since the proxy will intercept and re-sign certificates
	if (!proxy_.empty())
	{
		curl_easy_setopt(curl, CURLOPT_PROXY, proxy_.c_str());
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	else
	{
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
	}

	// headers
	std::string auth = "Authorization: Bearer " + token_;
	std::string cookie_header = "Cookie: access_token=" + token_;
	if (!client_id_.empty()) {
		cookie_header += "; client_id=" + client_id_;
	}
	hdr.list = curl_slist_append(hdr.list, auth.c_str());
	hdr.list = curl_slist_append(hdr.list, "Accept: application/json, text/plain, */*");
	hdr.list = curl_slist_append(hdr.list, cookie_header.c_str());
	hdr.list = curl_slist_append(hdr.list, "Referer: https://www.udemy.com/");
	hdr.list = curl_slist_append(hdr.list, "Origin: https://www.udemy.com");
	auto browser_headers = get_browser_headers();
	for (const auto& h : browser_headers) {
		hdr.list = curl_slist_append(hdr.list, h.c_str());
	}

	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdr.list);

	CURLcode rc = curl_easy_perform(curl);
	if (rc != CURLE_OK)
	{
		std::string err = curl_easy_strerror(rc);
		curl_easy_cleanup(curl);
		throw std::runtime_error("curl: " + err);
	}
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);
	curl_easy_cleanup(curl);

	if (code < 200 || code >= 300)
	{
		throw std::runtime_error("http " + std::to_string(code));
	}
	return body;
}

// ---------------- /session ----------------
std::pair<status, std::string> RequestHandler::handleSession() {
	json out;
	out["ok"] = true;
	out["source"] = "settings";
	out["auth"] = !token_.empty();
	out["opts"] = { {"subs", download_subtitles_}, {"assets", download_assets_} };

	if (token_.empty())
	{
		// not authenticated, but not an error
		return { status::ok, out.dump() };
	}

	try
	{
		// GET /api-2.0/users/me/?fields[user]=@default
		std::string url = api_base_ + "/api-2.0/users/me/?fields[user]=@default";
		auto body = udemy_get(url, 15000);
		json me = json::parse(body);
		out["user"] = me;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		out["auth"] = false;
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleSettingsUpdate(const std::string& body) {
	using status = boost::beast::http::status;
	nlohmann::json out; out["ok"] = false;

	try
	{
		auto in = nlohmann::json::parse(body);

		std::string new_token = token_;
		std::string new_api = api_base_;
		std::string new_proxy = proxy_;
		bool new_subs = download_subtitles_;
		bool new_assets = download_assets_;

		if (in.contains("udemy_access_token")) new_token = in.value("udemy_access_token", std::string{});
		if (in.contains("udemy_api_base"))    new_api = in.value("udemy_api_base", std::string{});
		if (in.contains("http_proxy"))        new_proxy = in.value("http_proxy", std::string{});
		if (in.contains("download_subtitles")) new_subs = in.value("download_subtitles", false);
		if (in.contains("download_assets"))    new_assets = in.value("download_assets", false);

		auto trim2 = [](std::string s)
			{
				auto ws = [](int c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
				while (!s.empty() && ws((unsigned char)s.front())) s.erase(s.begin());
				while (!s.empty() && ws((unsigned char)s.back()))  s.pop_back();
				return s;
			};
		new_token = trim2(new_token);
		new_api = trim2(new_api);
		new_proxy = trim2(new_proxy);
		if (new_api.empty()) new_api = "https://www.udemy.com";

		{
			std::ofstream f("settings.ini", std::ios::binary);
			if (!f)
			{
				out["error"] = "cannot write settings.ini";
				return { status::internal_server_error, out.dump() };
			}
			f << "# UdemySaver settings\n";
			f << "udemy_access_token=" << new_token << "\n";
			f << "udemy_api_base=" << new_api << "\n";
			if (!new_proxy.empty()) f << "http_proxy=" << new_proxy << "\n";
			f << "download_subtitles=" << (new_subs ? "true" : "false") << "\n";
			f << "download_assets=" << (new_assets ? "true" : "false") << "\n";
			f.flush();
		}

		token_ = new_token;
		api_base_ = new_api;
		proxy_ = new_proxy;
		download_subtitles_ = new_subs;
		download_assets_ = new_assets;

		out["ok"] = true;
		out["auth"] = !token_.empty();
		out["opts"] = { {"subs", download_subtitles_}, {"assets", download_assets_} };
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		out["auth"] = false;
		out["courses"] = json::array();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string>
RequestHandler::handleCourses(int page, int page_size, const std::string& query) {
	using boost::beast::http::status;
	json out;
	out["ok"] = true;

	if (page <= 0) page = 1;
	if (page_size <= 0 || page_size > 100) page_size = 12;

	if (token_.empty())
	{
		out["auth"] = false;
		out["page"] = page;
		out["page_size"] = page_size;
		out["courses"] = json::array();
		return { status::ok, out.dump() };
	}

	auto fix_proto = [](std::string s)->std::string
		{
			if (!s.empty() && s.rfind("//", 0) == 0) s = "https:" + s;
			return s;
		};

	auto make_abs = [&](std::string s)->std::string
		{
			if (!s.empty() && s[0] == '/') return api_base_ + s;
			return s;
		};

	try
	{
		std::ostringstream url;
		url << api_base_
			<< "/api-2.0/users/me/subscribed-courses/?page=" << page
			<< "&page_size=" << page_size
			<< "&fields[course]=@min,title,headline,url,image_480x270,image_480x270@2x,image_240x135,image_240x135@2x,image_125_H,image_200_H,visible_instructors";

		if (!query.empty()) {
			url << "&search=" << query;
		}
		else {
			url << "&ordering=-last_accessed";
		}

		auto body = udemy_get(url.str(), 15000);
		json raw = json::parse(body);

		json courses = json::array();
		if (raw.contains("results") && raw["results"].is_array())
		{
			for (auto& c : raw["results"])
			{
				auto pick = [&](const char* key)->std::string
					{
						if (c.contains(key) && c[key].is_string())
							return c[key].get<std::string>();
						return {};
					};

				std::string img = pick("image_480x270");
				if (img.empty()) img = pick("image_480x270@2x");
				if (img.empty()) img = pick("image_240x135");
				if (img.empty()) img = pick("image_240x135@2x");
				if (img.empty()) img = pick("image_200_H");
				if (img.empty()) img = pick("image_125_H");

				img = fix_proto(img);
				std::string rel = c.value("url", "");
				std::string abs = make_abs(rel);

				json j;
				j["id"] = c.value("id", 0);
				j["title"] = c.value("title", "");
				j["headline"] = c.value("headline", "");
				j["url"] = abs;
				j["image"] = img;
				j["image_raw"] = c.value("image_480x270", "");

				if (c.contains("visible_instructors") &&
					c["visible_instructors"].is_array() &&
					!c["visible_instructors"].empty())
				{
					j["instructor"] = c["visible_instructors"][0].value("title", "");
				}
				else
				{
					j["instructor"] = "";
				}
				courses.push_back(j);
			}
		}

		out["auth"] = true;
		out["page"] = page;
		out["page_size"] = page_size;
		out["total"] = raw.value("count", 0);
		out["courses"] = courses;

		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}


std::pair<boost::beast::http::status, std::string>
RequestHandler::handleLectures(int course_id, int page, int page_size) {
	using boost::beast::http::status;
	if (page <= 0) page = 1;
	if (page_size <= 0 || page_size > 200) page_size = 100;

	nlohmann::json out;
	try
	{
		if (token_.empty())
		{
			out["count"] = 0;
			out["next"] = nullptr;
			out["previous"] = nullptr;
			out["results"] = nlohmann::json::array();
			return { status::ok, out.dump() };
		}

		// subscriber-curriculum-items => chapter + lecture + asset
		std::ostringstream url;
		url << api_base_
			<< "/api-2.0/courses/" << course_id
			<< "/subscriber-curriculum-items/?page=" << page
			<< "&page_size=" << page_size
			<< "&fields[lecture]=asset,title,object_index,asset_type,supplementary_assets,download_url"
			<< "&fields[asset]=stream_urls,download_urls,download_url,filename,asset_type,hls_url"
			<< "&fields[chapter]=title,object_index"
			<< "&fields[supplementary_asset]=id,title,asset_type,download_urls,download_url,external_url,filename";

		auto body = udemy_get(url.str(), 20000);
		nlohmann::json raw = nlohmann::json::parse(body);

		out["count"] = raw.value("count", 0);
		out["next"] = raw.contains("next") ? raw["next"] : nullptr;
		out["previous"] = raw.contains("previous") ? raw["previous"] : nullptr;

		out["results"] = nlohmann::json::array();
		int cur_section = 0;
		std::string cur_section_title;

		if (raw.contains("results") && raw["results"].is_array())
		{
			for (auto& it : raw["results"])
			{
				const std::string klass = it.value("_class", it.value("type", ""));
				if (klass == "chapter")
				{
					cur_section += 1;
					cur_section_title = it.value("title", "");
					continue;
				}
				if (klass == "lecture")
				{
					nlohmann::json lec;
					lec["id"] = it.value("id", 0);
					lec["title"] = it.value("title", "");
					if (it.contains("asset")) lec["asset"] = it["asset"];
					if (it.contains("supplementary_assets"))
						lec["supplementary_assets"] = it["supplementary_assets"];
					lec["section_index"] = cur_section;
					lec["section_title"] = cur_section_title;
					lec["object_index"] = it.value("object_index", 0);
					out["results"].push_back(std::move(lec));
				}
			}
		}
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string>
RequestHandler::handleQueueAdd(const std::string& body) {
	using status = boost::beast::http::status;
	json out;

	try
	{
		json in = json::parse(body);

		int lecture_id = in.value("lecture_id", 0);
		int asset_id = in.value("asset_id", 0);
		int course_id_val = in.value("course_id", 0);

		std::string course_title_val = in.value("course_title", std::string{});
		std::string section_title_val = in.value("section_title", std::string{});

		bool is_video = in.value("is_video", false);
		std::string pref = in.value("quality", "1080");
		std::string in_url = in.value("url", std::string{});

		if (is_video && course_id_val > 0 && lecture_id > 0) {
			// Never trust a client-provided video URL: resolving here guarantees DRM
			// metadata is checked before the job enters the queue.
			in_url = resolve_lecture_stream(course_id_val, lecture_id, pref);
		}
		else if (in_url.empty())
		{
			if (asset_id > 0 && course_id_val > 0 && lecture_id > 0) {
				try { in_url = resolve_supplementary_asset(asset_id); }
				catch (...) { if (in_url.empty()) throw; }
			}
		}

		Job j;
		j.id = next_id_++;
		j.url = in_url;
		j.filename = in.value("filename", "");

		j.course_id = course_id_val;
		j.course_title = course_title_val;

		j.section_index = in.value("section_index", 0);
		j.section_title = section_title_val;
		j.lecture_index = in.value("lecture_index", 0);
		j.lecture_title = in.value("lecture_title", std::string{});

		if (j.course_id && !j.course_title.empty())
		{
			j.out_path_dir = Helper::course_dir(j.course_id, j.course_title);
			if (!j.section_title.empty() && j.section_index > 0)
			{
				j.out_path_dir += "/" + Helper::section_dir(j.section_index, j.section_title);
			}
		}
		else
		{
			j.out_path_dir = "downloads/misc";
		}

		if (!j.filename.empty()) {
			for (char& c : j.filename) {
				if (c == '<' || c == '>' || c == ':' || c == '"' ||
					c == '/' || c == '\\' || c == '|' || c == '?' || c == '*') {
					c = '-';
				}
			}

			auto abs_dir = std::filesystem::absolute(std::filesystem::u8path(j.out_path_dir));
			std::string abs_dir_str = Helper::path_to_utf8(abs_dir);

			int max_filename_len = 250 - static_cast<int>(abs_dir_str.length()) - 6;

			if (max_filename_len < 10) max_filename_len = 10;

			if (j.filename.size() > max_filename_len) {
				auto ext = std::filesystem::path(j.filename).extension().string();
				int base_len = max_filename_len - static_cast<int>(ext.length());

				if (base_len > 0) {
					j.filename = j.filename.substr(0, base_len) + ext;
				}
				else {
					j.filename = j.filename.substr(0, max_filename_len);
				}
			}
		}

		{
			std::error_code fec;
			auto out_dir = std::filesystem::u8path(j.out_path_dir);
			std::filesystem::create_directories(out_dir, fec);
			auto final_path = out_dir / std::filesystem::u8path(j.filename);

			bool exists_final = std::filesystem::exists(final_path, fec);
			if (exists_final)
			{
				json out2;
				out2["ok"] = true;
				out2["queued"] = false;
				out2["skipped"] = true;
				out2["reason"] = "exists";
				out2["path"] = Helper::path_to_utf8(final_path);
				return { status::ok, out2.dump() };
			}
		}

		j.headers.push_back(std::string("User-Agent: ") + kDefaultUserAgent);
		j.headers.push_back("Referer: https://www.udemy.com/");
		j.headers.push_back("Origin: https://www.udemy.com");

		append_auth_headers_for_url(j.url, j.headers);

		if (in.contains("headers") && in["headers"].is_array())
			for (auto& h : in["headers"]) if (h.is_string()) j.headers.push_back(h.get<std::string>());

		uint64_t job_id = j.id;

		{
			std::lock_guard<std::mutex> lk(mtx_);

			auto dup = std::find_if(
				queue_.begin(), queue_.end(),
				[&](const Job& q)
				{
					return (q.url == j.url ||
						(q.out_path_dir == j.out_path_dir &&
							q.filename == j.filename)) &&
						q.state != Job::State::Done &&
						q.state != Job::State::Failed;
				});

			if (dup != queue_.end())
			{
				json out2;
				out2["ok"] = true;
				out2["queued"] = false;
				out2["skipped"] = true;
				out2["reason"] = "queued";
				out2["id"] = dup->id;
				return { status::ok, out2.dump() };
			}

			int c_id = j.course_id;
			std::string c_title = j.course_title;

			std::cout << "[QUEUE] Added Job ID " << job_id << " -> " << j.out_path_dir << "/" << j.filename << std::endl;

			if (paused_courses_.find(c_id) != paused_courses_.end())
				j.state = Job::State::Paused;

			queue_.push_back(j);

			if (c_id) {
				auto& cp = progress_[c_id];
				if (cp.title.empty()) cp.title = c_title;
				cp.total += 1;
			}
		}

		cv_.notify_one();

		out["ok"] = true;
		out["queued"] = true;
		out["id"] = job_id;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleQueueList() {
	json out;
	out["ok"] = true;
	out["running"] = running_;
	out["items"] = json::array();

	auto jstate = [](RequestHandler::Job::State s) -> const char*
		{
			using S = RequestHandler::Job::State;
			if (s == S::Queued) return "queued";
			if (s == S::Downloading) return "downloading";
			if (s == S::Done) return "done";
			if (s == S::Failed) return "failed";
			if (s == S::Paused) return "paused";
			return "unknown";
		};

	std::lock_guard<std::mutex> lk(mtx_);
	out["items"] = json::array();
	for (auto const& j : queue_)
	{
		double eta = -1.0;
		if (j.state == Job::State::Downloading &&
			j.speed_bps > 1.0 &&
			j.bytes_total > 0 &&
			j.bytes_now >= 0 &&
			j.bytes_total >= j.bytes_now)
		{
			const double remain = static_cast<double>(j.bytes_total - j.bytes_now);
			eta = remain / j.speed_bps;
		}

		json it;
		it["id"] = j.id;
		it["url"] = j.url;
		it["filename"] = j.filename;
		it["state"] = jstate(j.state);
		it["progress"] = j.progress;
		it["message"] = j.message;
		it["out_path"] = j.out_path;

		it["course_id"] = j.course_id;
		it["course_title"] = j.course_title;
		it["section_index"] = j.section_index;
		it["section_title"] = j.section_title;
		it["lecture_index"] = j.lecture_index;
		it["lecture_title"] = j.lecture_title;

		it["out_dir"] = j.out_path_dir;

		it["bytes_now"] = j.bytes_now;
		it["bytes_total"] = j.bytes_total;
		it["speed_bps"] = j.speed_bps;

		it["eta_sec"] = eta;

		out["items"].push_back(std::move(it));
	}

	json courses = json::array();
	for (auto const& [cid, cp] : progress_)
	{
		json c;
		c["course_id"] = cid;
		c["title"] = cp.title;
		c["done"] = cp.done;
		c["total"] = cp.total;
		courses.push_back(std::move(c));
	}
	out["courses"] = courses;

	return { boost::beast::http::status::ok, out.dump() };
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleQueuePause(const std::string& body) {
	using status = boost::beast::http::status;
	json out;
	try
	{
		json in = json::parse(body);
		int course_id = in.value("course_id", 0);
		if (!course_id) throw std::runtime_error("missing course_id");

		{
			std::lock_guard<std::mutex> lk(mtx_);
			paused_courses_.insert(course_id);
			for (auto& q : queue_)
			{
				if (q.course_id == course_id &&
					q.state == Job::State::Queued)
				{
					q.state = Job::State::Paused;
				}
			}
		}

		out["ok"] = true;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleQueueResume(const std::string& body) {
	using status = boost::beast::http::status;
	json out;
	try
	{
		json in = json::parse(body);
		int course_id = in.value("course_id", 0);
		if (!course_id) throw std::runtime_error("missing course_id");

		{
			std::lock_guard<std::mutex> lk(mtx_);
			paused_courses_.erase(course_id);

			int newly_queued = 0;
			for (auto& q : queue_)
			{
				if (q.course_id == course_id &&
					(q.state == Job::State::Paused || q.state == Job::State::Failed))
				{
					q.state = Job::State::Queued;
					q.message = "";
				}
				else if (q.state == Job::State::Done)
				{
					std::error_code ec;
					if (!std::filesystem::exists(std::filesystem::u8path(q.out_path), ec))
					{
						q.state = Job::State::Queued;
						q.bytes_now = 0;
						q.progress = 0.0;
						newly_queued++;
					}
				}
			}
			if (newly_queued > 0 && progress_.count(course_id)) {
				progress_[course_id].done = std::max(0, progress_[course_id].done - newly_queued);
			}
		}
		cv_.notify_one();

		out["ok"] = true;
		return { status::ok, out.dump() };
	}
	catch (const std::exception& e)
	{
		out["ok"] = false;
		out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleReconcile(const std::string& target) {
	using status = boost::beast::http::status;
	auto get_param = [&](const char* key)->std::string
		{
			auto qpos = target.find('?'); if (qpos == std::string::npos) return {};
			std::string qs = target.substr(qpos + 1);
			std::istringstream ss(qs); std::string kv;
			while (std::getline(ss, kv, '&'))
			{
				auto eq = kv.find('='); if (eq == std::string::npos) continue;
				auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
				if (k == key) return v;
			}
			return {};
		};

	int course_id = 0;
	try { course_id = std::stoi(get_param("course_id")); }
	catch (...) {}

	json out; out["ok"] = true;
	out["present"] = json::array();


	std::string dir = "";
	{
		std::lock_guard<std::mutex> lk(mtx_);
		auto it = progress_.find(course_id);
		if (it != progress_.end() && !it->second.title.empty())
		{
			dir = Helper::course_dir(course_id, it->second.title);
		}
	}
	if (dir.empty())
	{
		out["note"] = "course_dir not resolved";
		return { status::ok, out.dump() };
	}

	std::error_code ec;
	auto dir_path = std::filesystem::u8path(dir);
	if (std::filesystem::exists(dir_path, ec))
	{
		for (auto& p : std::filesystem::recursive_directory_iterator(dir_path, ec))
		{
			if (p.is_regular_file())
			{
				auto name = Helper::path_to_utf8(p.path().filename());
				int idx = 0;
				if (name.size() >= 3 &&
					std::isdigit(static_cast<unsigned char>(name[0])) &&
					std::isdigit(static_cast<unsigned char>(name[1])) &&
					std::isdigit(static_cast<unsigned char>(name[2])))
				{
					idx = (name[0] - '0') * 100 + (name[1] - '0') * 10 + (name[2] - '0');
				}
				json f; f["file"] = name; f["lecture_index"] = idx;
				out["present"].push_back(std::move(f));
			}
		}
	}
	return { status::ok, out.dump() };
}

std::pair<boost::beast::http::status, std::string> RequestHandler::handleEstimate(const std::string& target) {
	using status = boost::beast::http::status;

	auto get_param = [&](const char* key) -> std::string {
		auto qpos = target.find('?'); if (qpos == std::string::npos) return {};
		std::string qs = target.substr(qpos + 1);
		std::istringstream ss(qs); std::string kv;
		while (std::getline(ss, kv, '&')) {
			auto eq = kv.find('='); if (eq == std::string::npos) continue;
			auto k = kv.substr(0, eq), v = kv.substr(eq + 1);
			if (k == key) return v;
		}
		return {};
		};

	int course_id = 0;
	try { course_id = std::stoi(get_param("course_id")); }
	catch (...) {}
	std::string quality = get_param("quality"); if (quality.empty()) quality = "720";

	nlohmann::json out; out["ok"] = true;
	if (!course_id) { out["ok"] = false; out["error"] = "missing course_id"; return { status::bad_request, out.dump() }; }
	if (token_.empty()) { out["ok"] = false; out["error"] = "not authenticated"; return { status::unauthorized, out.dump() }; }

	long long total_bytes = 0;
	int sized_mp4 = 0, unknown_mp4 = 0, hls_count = 0, total_videos = 0;

	try {
		int page = 1;
		while (true) {
			std::ostringstream url;
			url << api_base_ << "/api-2.0/courses/" << course_id
				<< "/subscriber-curriculum-items/?page=" << page << "&page_size=200"
				<< "&fields[lecture]=asset,title,object_index,asset_type"
				<< "&fields[asset]=stream_urls,download_urls,download_url,hls_url,media_sources,asset_type";

			auto body = udemy_get(url.str(), 20000);
			nlohmann::json raw = nlohmann::json::parse(body);

			if (!(raw.contains("results") && raw["results"].is_array())) break;

			for (auto& it : raw["results"]) {
				const std::string klass = it.value("_class", it.value("type", ""));
				if (klass != "lecture") continue;

				auto asset = it.contains("asset") ? it["asset"] : nlohmann::json{};
				std::string urlv = pick_from_asset_for_size(asset, quality);
				if (urlv.empty()) continue;

				total_videos++;

				if (urlv.find(".m3u8") != std::string::npos) {
					hls_count++;
					continue;
				}

				long long bytes = -1; std::string emsg;
				std::vector<std::string> hdrs = {
					std::string("User-Agent: ") + kDefaultUserAgent,
					"Referer: https://www.udemy.com/",
					"Origin: https://www.udemy.com"
				};

				append_auth_headers_for_url(urlv, hdrs);

				if (probe_content_length(urlv, hdrs, bytes, emsg) && bytes >= 0) {
					total_bytes += bytes;
					sized_mp4++;
				}
				else {
					unknown_mp4++;
				}

				std::this_thread::sleep_for(std::chrono::milliseconds(150));
			}

			if (raw.contains("next") && !raw["next"].is_null()) {
				page++;
				std::this_thread::sleep_for(std::chrono::milliseconds(1000));
			}
			else break;
		}

		out["total_bytes"] = total_bytes;
		out["videos"] = total_videos;
		out["sized"] = sized_mp4;
		out["unknown"] = unknown_mp4 + hls_count;
		out["details"] = { {"hls", hls_count}, {"error_mp4", unknown_mp4} };
		out["quality"] = quality;

		return { status::ok, out.dump() };
	}
	catch (const std::exception& e) {
		out["ok"] = false; out["error"] = e.what();
		return { status::bad_request, out.dump() };
	}
}


static size_t file_write(void* ptr, size_t size, size_t nmemb, void* userdata) {
	FILE* fp = (FILE*)userdata;
	return fwrite(ptr, size, nmemb, fp);
}

static int curl_xferinfo_trampoline(void* clientp, curl_off_t dltotal, curl_off_t dlnow, ...) {
	using CB = std::function<bool(double, double)>; 
	auto* cb = reinterpret_cast<CB*>(clientp);
	if (cb && *cb) {
		bool continue_dl = (*cb)(static_cast<double>(dlnow), static_cast<double>(dltotal));
		return continue_dl ? 0 : 1; 
	}
	return 0;
}

bool RequestHandler::curl_download_file(const std::string& url, const std::string& out_path, const std::vector<std::string>& extra_headers, std::function<void(double, double)> on_progress, std::string& msg) {
	msg.clear();
	auto out_fs = std::filesystem::u8path(out_path);
	auto tmp_path = out_fs;
	tmp_path += ".part";

	auto tmp_utf8_u8 = tmp_path.u8string();
	std::string tmp_utf8(tmp_utf8_u8.begin(), tmp_utf8_u8.end());

	std::cout << "\n--------------------------------------------------" << std::endl;
	std::cout << "[DOWNLOAD] Target: " << tmp_utf8 << std::endl;
	std::cout << "[INFO] Path Length: " << tmp_utf8.length() << " characters." << std::endl;

	if (tmp_utf8.length() >= 250) {
		std::cout << "[WARNING] Path length is close to Windows 260 limit!" << std::endl;
	}

	std::error_code ec;
	std::filesystem::remove(tmp_path, ec);

	FILE* fp = Helper::xfopen(tmp_utf8.c_str(), "wb");
	if (!fp) {
		msg = std::string("Cannot open file. OS Error: ") + std::strerror(errno);
		std::cout << "[DOWNLOAD ERROR] File IO Failed: " << msg << std::endl;
		return false;
	}

	CurlHandle ch;
	if (!ch.h) {
		fclose(fp);
		msg = "curl init failed";
		return false;
	}

	struct curl_slist* hdr = nullptr;
	for (auto& h : extra_headers) hdr = curl_slist_append(hdr, h.c_str());

	curl_easy_setopt(ch.h, CURLOPT_URL, url.c_str());
	curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 8L);
	curl_easy_setopt(ch.h, CURLOPT_USERAGENT, kDefaultUserAgent);
	curl_easy_setopt(ch.h, CURLOPT_ACCEPT_ENCODING, "");
	curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, hdr);
	curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, fp);
	curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, file_write);

	curl_easy_setopt(ch.h, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(ch.h, CURLOPT_XFERINFOFUNCTION, curl_xferinfo_trampoline);
	curl_easy_setopt(ch.h, CURLOPT_XFERINFODATA, &on_progress);

	curl_easy_setopt(ch.h, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
	curl_easy_setopt(ch.h, CURLOPT_TIMEOUT, 0L);

	if (!proxy_.empty()) {
		curl_easy_setopt(ch.h, CURLOPT_PROXY, proxy_.c_str());
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 0L);
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 0L);
	}
	else {
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);
		curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 2L);
	}

	CURLcode rc = curl_easy_perform(ch.h);

	long http_code = 0;
	curl_easy_getinfo(ch.h, CURLINFO_RESPONSE_CODE, &http_code);

	if (hdr) curl_slist_free_all(hdr);
	fclose(fp);

	if (rc != CURLE_OK) {
		msg = curl_easy_strerror(rc);
		std::cout << "[DOWNLOAD ERROR] cURL Error: " << msg << std::endl;
		return false;
	}

	if (http_code >= 400) {
		msg = "HTTP " + std::to_string(http_code);
		std::cout << "[DOWNLOAD ERROR] Server rejected request. Status: " << msg << std::endl;
		return false;
	}

	ec.clear();
	std::filesystem::rename(tmp_path, out_fs, ec);
	if (ec) {
		msg = "Rename failed: " + ec.message();
		std::cout << "[DOWNLOAD ERROR] " << msg << std::endl;
		return false;
	}

	std::cout << "[DOWNLOAD] Completed successfully." << std::endl;
	return true;
}

void RequestHandler::append_auth_headers_for_url(const std::string& url, std::vector<std::string>& headers) const
{
	if (token_.empty()) return;

	if (url.find("Signature=") != std::string::npos ||
		url.find("token=") != std::string::npos)
	{
		return;
	}

	headers.push_back(std::string("Authorization: Bearer ") + token_);

	std::string cookie_header = "Cookie: access_token=" + token_;
	if (!client_id_.empty()) {
		cookie_header += "; client_id=" + client_id_;
	}
	headers.push_back(cookie_header);
}

std::string RequestHandler::resolve_lecture_stream(int course_id, int lecture_id, const std::string& prefer_quality) {
	std::ostringstream url;
	url << api_base_
		<< "/api-2.0/users/me/subscribed-courses/" << course_id
		<< "/lectures/" << lecture_id
		<< "?fields[asset]=stream_urls,download_urls,download_url,captions,title,filename,data,body,hls_url,media_sources,asset_type,length,media_license_token,course_is_drmed,thumbnail_sprite,slides,slide_urls,external_url"
		<< "&fields[lecture]=asset,supplementary_assets,description,download_url,is_free,last_watched_second";

	auto body = udemy_get(url.str(), 15000);
	json j = json::parse(body);

	if (!j.contains("asset") || !j["asset"].is_object())
		throw std::runtime_error("lecture has no asset");

	auto& a = j["asset"];
	if (is_drm_protected_asset(j) || is_drm_protected_asset(a))
		throw std::runtime_error(kDrmProtectedMessage);

	auto is_exact_1080 = [](const std::string& s) -> bool
		{
			if (s.empty()) return false;
			for (char c : s)
			{
				if (!std::isdigit(static_cast<unsigned char>(c))) return false;
			}
			return Helper::extract_quality_value(s) == 1080;
		};

	if (is_exact_1080(prefer_quality))
	{
		auto make_absolute = [](const std::string& base_url, const std::string& rel) -> std::string
			{
				if (rel.empty()) return base_url;
				if (rel.find("://") != std::string::npos) return rel;

				std::string base_no_query = base_url;
				std::string base_query;
				auto qpos = base_no_query.find('?');
				if (qpos != std::string::npos)
				{
					base_query = base_url.substr(qpos);
					base_no_query = base_no_query.substr(0, qpos);
				}

				auto append_query = [&](std::string result) -> std::string
					{
						if (!base_query.empty() && result.find('?') == std::string::npos)
							result += base_query;
						return result;
					};

				if (!rel.empty() && rel[0] == '/')
				{
					auto scheme_pos = base_no_query.find("://");
					if (scheme_pos == std::string::npos) return append_query(rel);
					auto host_end = base_no_query.find('/', scheme_pos + 3);
					std::string origin = (host_end == std::string::npos)
						? base_no_query
						: base_no_query.substr(0, host_end);
					return append_query(origin + rel);
				}

				std::string base_dir = base_no_query;
				auto slash = base_dir.rfind('/');
				if (slash == std::string::npos)
					base_dir += '/';
				else
					base_dir = base_dir.substr(0, slash + 1);

				return append_query(base_dir + rel);
			};

		auto fetch_with_headers = [&](const std::string& src) -> std::string
			{
				CurlHandle ch; if (!ch.h) throw std::runtime_error("curl init failed");

				struct curl_slist* hdr = nullptr;
				std::vector<std::string> headers = {
											std::string("User-Agent: ") + kDefaultUserAgent,
											"Referer: https://www.udemy.com/",
											"Origin: https://www.udemy.com"
				};
				append_auth_headers_for_url(src, headers);

				for (auto& h : headers) hdr = curl_slist_append(hdr, h.c_str());

				std::string out;
				curl_easy_setopt(ch.h, CURLOPT_URL, src.c_str());
				curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
				curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 8L);
				curl_easy_setopt(ch.h, CURLOPT_USERAGENT, kDefaultUserAgent);
				curl_easy_setopt(ch.h, CURLOPT_ACCEPT_ENCODING, "");
				curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, hdr);
				curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, Helper::write_to_string);
				curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, &out);
				curl_easy_setopt(ch.h, CURLOPT_CONNECTTIMEOUT_MS, 8000L);
				curl_easy_setopt(ch.h, CURLOPT_TIMEOUT_MS, 20000L);
				if (!proxy_.empty()) {
					curl_easy_setopt(ch.h, CURLOPT_PROXY, proxy_.c_str());
					curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 0L);
					curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 0L);
				}
				else {
					curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);
					curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 2L);
				}

				CURLcode rc = curl_easy_perform(ch.h);
				if (hdr) curl_slist_free_all(hdr);

				if (rc != CURLE_OK)
				{
					throw std::runtime_error(std::string("curl: ") + curl_easy_strerror(rc));
				}

				long code = 0;
				curl_easy_getinfo(ch.h, CURLINFO_RESPONSE_CODE, &code);
				if (code < 200 || code >= 300)
				{
					throw std::runtime_error("http " + std::to_string(code));
				}

				return out;
			};

		auto pick_variant_from_master = [&](const std::string& master_url, const std::string& playlist) -> std::string
			{
				if (is_drm_protected_playlist(playlist))
					throw std::runtime_error(kDrmProtectedMessage);

				struct Variant {
					std::string uri;
					int width = 0;
					int height = 0;
					long long bandwidth = 0;
				};

				std::vector<Variant> variants;
				std::istringstream ss(playlist);
				std::string line;
				while (std::getline(ss, line))
				{
					if (!line.empty() && line.back() == '\r') line.pop_back();
					std::string trimmed = Helper::trim(line);
					if (trimmed.rfind("#EXT-X-STREAM-INF", 0) != 0) continue;

					Variant v;
					auto colon = trimmed.find(':');
					std::string attrs = (colon == std::string::npos) ? std::string{} : trimmed.substr(colon + 1);

					auto find_attr = [&](const std::string& key) -> std::string
						{
							auto pos = attrs.find(key);
							if (pos == std::string::npos) return {};
							pos += key.size();
							auto end = attrs.find(',', pos);
							std::string val = (end == std::string::npos) ? attrs.substr(pos) : attrs.substr(pos, end - pos);
							return Helper::trim(val);
						};

					std::string res = find_attr("RESOLUTION=");
					auto x = res.find('x');
					if (x != std::string::npos)
					{
						try
						{
							v.width = std::stoi(res.substr(0, x));
							v.height = std::stoi(res.substr(x + 1));
						}
						catch (...)
						{
							v.width = v.height = 0;
						}
					}

					std::string bw = find_attr("BANDWIDTH=");
					if (!bw.empty())
					{
						try { v.bandwidth = std::stoll(bw); }
						catch (...) { v.bandwidth = 0; }
					}

					std::string next_line;
					while (std::getline(ss, next_line))
					{
						if (!next_line.empty() && next_line.back() == '\r') next_line.pop_back();
						std::string url_line = Helper::trim(next_line);
						if (url_line.empty()) continue;
						if (url_line.rfind("#", 0) == 0) continue;
						v.uri = url_line;
						break;
					}

					if (!v.uri.empty()) variants.push_back(std::move(v));
				}

				if (variants.empty()) return {};

				int target_h = Helper::extract_quality_value(prefer_quality);
				bool want_highest = (prefer_quality == "Highest" || target_h == 0);
				bool want_lowest = (prefer_quality == "Lowest");

				const Variant* chosen = nullptr;

				if (want_highest) {
					for (auto& v : variants) {
						if (!chosen || v.height > chosen->height || (v.height == chosen->height && v.bandwidth > chosen->bandwidth)) chosen = &v;
					}
				}
				else if (want_lowest) {
					for (auto& v : variants) {
						if (!chosen || v.height < chosen->height || (v.height == chosen->height && v.bandwidth < chosen->bandwidth)) chosen = &v;
					}
				}
				else {
					for (auto& v : variants) {
						if (v.height == target_h) {
							if (!chosen || v.bandwidth > chosen->bandwidth) chosen = &v;
						}
					}
					if (!chosen) {
						for (auto& v : variants) {
							if (v.height <= target_h) {
								if (!chosen || v.height > chosen->height) chosen = &v;
							}
						}
					}
					if (!chosen && !variants.empty()) {
						for (auto& v : variants) {
							if (!chosen || v.height > chosen->height) chosen = &v;
						}
					}
				}

				if (!chosen) return {};
				return make_absolute(master_url, chosen->uri);
			};

		auto first_media_source = [&]() -> std::string
			{
				if (a.contains("media_sources") && a["media_sources"].is_array())
				{
					for (auto& m : a["media_sources"])
					{
						if (m.contains("src") && m["src"].is_string())
						{
							std::string src = m["src"].get<std::string>();
							if (!src.empty()) return src;
						}
					}
				}
				return {};
			};

		std::string master_src = first_media_source();
		if (!master_src.empty())
		{
			try
			{
				std::cout << "[HLS] Fetching master playlist: " << master_src << std::endl;
				std::string playlist = fetch_with_headers(master_src);
				std::string variant = pick_variant_from_master(master_src, playlist);
				if (!variant.empty())
				{
					std::cout << "[HLS] Selected variant playlist: " << variant << std::endl;
				}
				if (!variant.empty()) return variant;
			}
			catch (const std::runtime_error& e)
			{
				if (std::string(e.what()) == kDrmProtectedMessage) throw;
			}
			catch (...)
			{
			}
		}
	}

	if (a.contains("stream_urls") && a["stream_urls"].is_object())
	{
		auto& su = a["stream_urls"];
		if (su.contains("Video") && su["Video"].is_array() && !su["Video"].empty())
		{
			std::map<int, std::string> qmap;
			std::map<int, std::string> hls_map;
			std::string autoSrc;
			std::string hlsSrc;
			std::string autoHlsSrc;

			for (auto& v : su["Video"])
			{
				if (!v.contains("file")) continue;
				std::string file = v["file"].get<std::string>();
				if (file.empty()) continue;

				std::string label = v.value("label", "");
				std::string type = v.value("type", "");

				bool is_hls = (type == "application/x-mpegURL") || label == "Auto";
				if (is_hls)
				{
					if (hlsSrc.empty()) hlsSrc = file;
					if (autoSrc.empty()) autoSrc = file;
					if (autoHlsSrc.empty() && label == "Auto") autoHlsSrc = file;

					int q = Helper::extract_quality_value(label);
					if (q > 0)
					{
						hls_map[q] = file;
					}
					continue;
				}

				int q = Helper::extract_quality_value(label);
				if (q > 0)
				{
					qmap[q] = file;
				}
				else if (autoSrc.empty())
				{
					autoSrc = file;
				}
			}

			if (a.contains("hls_url") && a["hls_url"].is_string() && hlsSrc.empty())
			{
				hlsSrc = a["hls_url"].get<std::string>();
			}

			if (a.contains("media_sources") && a["media_sources"].is_array())
			{
				for (auto& m : a["media_sources"])
				{
					if (!m.contains("src") || !m["src"].is_string()) continue;

					std::string src = m["src"].get<std::string>();
					if (src.empty()) continue;

					if (hlsSrc.empty()) hlsSrc = src;
					if (autoSrc.empty()) autoSrc = src;
					if (autoHlsSrc.empty())
					{
						std::string lbl = m.value("label", m.value("quality", std::string{}));
						if (lbl == "Auto") autoHlsSrc = src;
					}

					std::string label = m.value("label", m.value("quality", std::string{}));
					int q = Helper::extract_quality_value(label);
					if (q > 0)
					{
						hls_map[q] = src;
					}
				}
			}

			int highest_mp4 = qmap.empty() ? 0 : qmap.rbegin()->first;
			bool prefer_hls = false;
			if (!hlsSrc.empty())
			{
				if (prefer_quality == "Highest")
				{
					prefer_hls = true;
				}
				else
				{
					int wanted = Helper::extract_quality_value(prefer_quality);
					if (wanted >= 1080)
					{
						prefer_hls = true;
					}
					else if (wanted > 0 && highest_mp4 > 0 && wanted > highest_mp4)
					{
						prefer_hls = true;
					}
				}
			}


			if (autoHlsSrc.empty()) autoHlsSrc = hlsSrc;

			if (prefer_hls)
			{
				int wanted = Helper::extract_quality_value(prefer_quality);
				if (wanted > 0 && !hls_map.empty())
				{
					auto it = hls_map.find(wanted);
					if (it != hls_map.end())
					{
						std::cout << "[HLS] Using HLS playlist: " << it->second << std::endl;
						return it->second;
					}

					auto it_up = hls_map.lower_bound(wanted);
					if (it_up != hls_map.end())
					{
						std::cout << "[HLS] Using HLS playlist: " << it_up->second << std::endl;
						return it_up->second;
					}

					auto best = hls_map.rbegin();
					if (best != hls_map.rend())
					{
						std::cout << "[HLS] Using HLS playlist: " << best->second << std::endl;
						return best->second;
					}
				}

				if (!autoHlsSrc.empty())
				{
					std::cout << "[HLS] Using HLS playlist: " << autoHlsSrc << std::endl;
					return autoHlsSrc;
				}

				if (!hlsSrc.empty())
				{
					std::cout << "[HLS] Using HLS playlist: " << hlsSrc << std::endl;
				}
				return hlsSrc;
			}

			if (prefer_quality == "Auto")
			{
				if (!autoSrc.empty()) return autoSrc;
				if (!qmap.empty()) return qmap.rbegin()->second;
				if (!hlsSrc.empty()) return hlsSrc;
			}

			if (prefer_quality == "Lowest")
			{
				if (!qmap.empty()) return qmap.begin()->second;
				if (!autoSrc.empty()) return autoSrc;
				if (!hlsSrc.empty()) return hlsSrc;
			}

			if (!qmap.empty())
			{
				if (prefer_quality == "Highest")
					return qmap.rbegin()->second;

				int wanted = Helper::extract_quality_value(prefer_quality);
				if (wanted > 0)
				{
					auto it = qmap.find(wanted);
					if (it != qmap.end()) return it->second;
					auto it_up = qmap.lower_bound(wanted);
					if (it_up != qmap.end()) return it_up->second;
					return qmap.rbegin()->second;
				}

				return qmap.rbegin()->second;
			}

			if (!autoSrc.empty()) return autoSrc;
			if (!hlsSrc.empty()) return hlsSrc;
		}
	}

	if (a.contains("hls_url") && a["hls_url"].is_string())
	{
		return a["hls_url"].get<std::string>();
	}

	if (a.contains("media_sources") && a["media_sources"].is_array())
	{
		for (auto& m : a["media_sources"])
		{
			if (m.contains("src") && m["src"].is_string())
			{
				return m["src"].get<std::string>();
			}
		}
	}

	throw std::runtime_error("no stream url found in lecture");
}

std::string RequestHandler::resolve_supplementary_asset(int asset_id) {
	std::ostringstream url;
	url << api_base_ << "/api-2.0/assets/" << asset_id
		<< "/?fields[asset]=download_urls,download_url,external_url,filename,asset_type";

	auto body = udemy_get(url.str(), 15000);
	json j = json::parse(body);

	const json& target = j.contains("asset") ? j["asset"] : j;

	if (target.contains("download_urls") && target["download_urls"].is_object()) {
		auto& d_urls = target["download_urls"];
		for (auto it = d_urls.begin(); it != d_urls.end(); ++it) {
			if (it.value().is_array() && !it.value().empty()) {
				auto& entry = it.value()[0];
				if (entry.contains("file")) return entry["file"].get<std::string>();
			}
		}
	}

	if (target.value("download_url", "") != "") return target["download_url"];
	if (target.value("external_url", "") != "") return target["external_url"];

	throw std::runtime_error("no downloadable url found");
}

static inline double now_sec() {
	using namespace std::chrono;
	return duration_cast<duration<double>>(steady_clock::now().time_since_epoch()).count();
}

void RequestHandler::worker_loop() {
	while (true)
	{
		Job j;
		bool should_stop = false;
		bool has_job = false;

		{
			std::unique_lock<std::mutex> lk(mtx_);
			cv_.wait(lk, [&] {
				if (stop_) return true;
				return std::any_of(queue_.begin(), queue_.end(), [](const Job& x) { return x.state == Job::State::Queued; });
				});

			if (stop_) {
				should_stop = true;
			}
			else {
				auto it = std::find_if(queue_.begin(), queue_.end(), [](const Job& x) { return x.state == Job::State::Queued; });
				if (it != queue_.end())
				{
					j = *it;
					it->state = Job::State::Downloading;
					has_job = true;
				}
			}
		}

		if (should_stop) break;
		if (!has_job) continue;

		std::string lower_url = j.url;
		std::transform(lower_url.begin(), lower_url.end(), lower_url.begin(), [](unsigned char c) { return (char)std::tolower(c); });
		bool is_hls = lower_url.find(".m3u8") != std::string::npos;

		std::cout << "\n==================================================" << std::endl;
		std::cout << "[WORKER] PROCESSING STARTED" << std::endl;
		std::cout << "[WORKER] Job ID : " << j.id << std::endl;
		std::cout << "[WORKER] Course : " << j.course_title << std::endl;
		std::cout << "[WORKER] File   : " << j.filename << std::endl;
		std::cout << "[WORKER] HLS?   : " << (is_hls ? "YES" : "NO") << std::endl;
		std::cout << "==================================================" << std::endl;

		auto out_dir = std::filesystem::u8path(j.out_path_dir);
		std::filesystem::path target_path = out_dir / std::filesystem::u8path(j.filename);
		if (is_hls)
		{
			target_path.replace_extension(".mp4");
			j.filename = Helper::path_to_utf8(target_path.filename());
			std::cout << "[HLS] Downloading HLS stream to MP4: " << Helper::path_to_utf8(target_path) << std::endl;
		}

		j.out_path = Helper::path_to_utf8(target_path);

		{
			std::lock_guard<std::mutex> lk(mtx_);
			auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Job& q) { return q.id == j.id; });
			if (it != queue_.end()) {
				it->bytes_now = 0;
				it->progress = 0.0;
			}
		}

		std::atomic<double>     last_ts{ now_sec() };
		std::atomic<long long>  last_bytes{ 0 };

		auto on_progress = [this, &j, &last_ts, &last_bytes](double dlnow, double dltotal)
			{
				std::lock_guard<std::mutex> lk(mtx_);

				if (paused_courses_.count(j.course_id) || stop_) {
					return false;
				}

				j.bytes_now = static_cast<long long>(dlnow);
				j.bytes_total = static_cast<long long>(dltotal);
				double ts = now_sec();
				const double dt = std::max(0.25, ts - last_ts.load());
				long long db = j.bytes_now - last_bytes.load();
				double inst = (double)db / dt;
				if (j.speed_bps <= 0) j.speed_bps = inst;
				else                  j.speed_bps = 0.25 * inst + 0.75 * j.speed_bps;

				last_ts.store(ts);
				last_bytes.store(j.bytes_now);

				if (j.bytes_total > 0)
					j.progress = (j.bytes_now * 100.0) / (double)j.bytes_total;

				auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Job& q) { return q.id == j.id; });
				if (it != queue_.end()) {
					it->bytes_now = j.bytes_now;
					it->bytes_total = j.bytes_total;
					it->speed_bps = j.speed_bps;
					it->progress = j.progress;
					it->filename = j.filename;
					it->out_path = j.out_path;
				}

				return true;
			};

		{
			std::error_code ed;
			std::filesystem::create_directories(out_dir, ed);

			std::string msg = "";
			bool ok = false;

			if (is_hls)
			{
				std::cout << "[HLS] Intercepting Playlist to bypass CDN Auth checks..." << std::endl;
				std::string variant_m3u8_body;
				std::string effective_url = j.url;
				std::string current_url = j.url;

				auto fetch_playlist = [&](const std::string& target_url, std::string& body, std::string& eff_url) -> bool {
					CurlHandle ch;
					if (!ch.h) return false;
					curl_easy_setopt(ch.h, CURLOPT_URL, target_url.c_str());
					curl_easy_setopt(ch.h, CURLOPT_FOLLOWLOCATION, 1L);
					curl_easy_setopt(ch.h, CURLOPT_MAXREDIRS, 8L);
					curl_easy_setopt(ch.h, CURLOPT_USERAGENT, kDefaultUserAgent);
					curl_easy_setopt(ch.h, CURLOPT_ACCEPT_ENCODING, "");

					struct curl_slist* hdr = nullptr;
					std::vector<std::string> headers = {
						std::string("User-Agent: ") + kDefaultUserAgent,
						"Referer: https://www.udemy.com/",
						"Origin: https://www.udemy.com"
					};
					append_auth_headers_for_url(target_url, headers);
					for (const auto& h : headers) hdr = curl_slist_append(hdr, h.c_str());

					curl_easy_setopt(ch.h, CURLOPT_HTTPHEADER, hdr);
					curl_easy_setopt(ch.h, CURLOPT_WRITEFUNCTION, Helper::write_to_string);
					curl_easy_setopt(ch.h, CURLOPT_WRITEDATA, &body);

					if (!proxy_.empty()) {
						curl_easy_setopt(ch.h, CURLOPT_PROXY, proxy_.c_str());
						curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 0L);
						curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 0L);
					}
					else {
						curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYPEER, 1L);
						curl_easy_setopt(ch.h, CURLOPT_SSL_VERIFYHOST, 2L);
					}

					CURLcode rc = curl_easy_perform(ch.h);
					long code = 0;
					curl_easy_getinfo(ch.h, CURLINFO_RESPONSE_CODE, &code);

					if (rc == CURLE_OK) {
						char* eff_url_ptr = nullptr;
						curl_easy_getinfo(ch.h, CURLINFO_EFFECTIVE_URL, &eff_url_ptr);
						if (eff_url_ptr) eff_url = eff_url_ptr;
					}
					if (hdr) curl_slist_free_all(hdr);
					return (rc == CURLE_OK && code < 400);
					};

				if (!fetch_playlist(current_url, variant_m3u8_body, effective_url)) {
					ok = false;
					msg = "Initial m3u8 playlist fetch failed";
				}
				else {
					ok = true;

					if (variant_m3u8_body.find("#EXT-X-STREAM-INF") != std::string::npos) {
						std::cout << "[HLS] Master Playlist detected. Extracting best variant..." << std::endl;
						std::istringstream ss(variant_m3u8_body);
						std::string line, variant_uri;

						int max_res = -1;
						int max_bw = -1;

						while (std::getline(ss, line)) {
							if (!line.empty() && line.back() == '\r') line.pop_back();

							std::string_view sv(line);

							if (sv.starts_with("#EXT-X-STREAM-INF")) {
								int current_bw = 0;
								int current_h = 0;

								auto bw_pos = sv.find("BANDWIDTH=");
								if (bw_pos != std::string_view::npos) {
									current_bw = std::atoi(line.c_str() + bw_pos + 10);
								}

								auto res_pos = sv.find("RESOLUTION=");
								if (res_pos != std::string_view::npos) {
									auto x_pos = sv.find('x', res_pos);
									if (x_pos != std::string_view::npos) {
										current_h = std::atoi(line.c_str() + x_pos + 1);
									}
								}

								std::string uri_line;
								while (std::getline(ss, uri_line)) {
									if (!uri_line.empty() && uri_line.back() == '\r') uri_line.pop_back();
									if (uri_line.empty() || uri_line.front() == '#') continue;

									if (current_h > max_res || (current_h == max_res && current_bw > max_bw)) {
										max_res = current_h;
										max_bw = current_bw;
										variant_uri = std::move(uri_line);
									}
									break;
								}
							}
						}

						if (!variant_uri.empty()) {
							std::cout << "[HLS] Selected highest variant: " << max_res << "p (BW: " << max_bw << ")" << std::endl;

							if (variant_uri.find("://") == std::string::npos) {
								std::string base_no_query = effective_url;
								auto qpos = base_no_query.find('?');
								std::string query_params;
								if (qpos != std::string::npos) {
									query_params = base_no_query.substr(qpos);
									base_no_query = base_no_query.substr(0, qpos);
								}
								auto slash = base_no_query.rfind('/');
								if (slash != std::string::npos) base_no_query = base_no_query.substr(0, slash + 1);

								if (!variant_uri.empty() && variant_uri[0] == '/') {
									auto scheme_pos = effective_url.find("://");
									auto host_end = effective_url.find('/', scheme_pos + 3);
									std::string host = (host_end != std::string::npos) ? effective_url.substr(0, host_end) : effective_url;
									current_url = host + variant_uri;
								}
								else {
									current_url = base_no_query + variant_uri;
								}
								if (current_url.find('?') == std::string::npos) current_url += query_params;
							}
							else {
								current_url = variant_uri;
							}

							variant_m3u8_body.clear();
							effective_url.clear();
							if (!fetch_playlist(current_url, variant_m3u8_body, effective_url)) {
								ok = false;
								msg = "Variant playlist fetch failed";
							}
						}
					}
				}

				if (ok && is_drm_protected_playlist(variant_m3u8_body)) {
					ok = false;
					msg = kDrmProtectedMessage;
				}

				if (ok) {
					std::string base_url = effective_url;
					std::string query_params;
					auto qpos = base_url.find('?');
					if (qpos != std::string::npos) {
						query_params = base_url.substr(qpos);
						base_url = base_url.substr(0, qpos);
					}
					auto slash = base_url.rfind('/');
					if (slash != std::string::npos) base_url = base_url.substr(0, slash + 1);

					std::string local_m3u8_content;
					std::istringstream iss(variant_m3u8_body);
					std::string line;
					while (std::getline(iss, line)) {
						if (!line.empty() && line.back() == '\r') line.pop_back();
						if (line.empty()) continue;

						if (line[0] == '#') {
							local_m3u8_content += line + "\n";
						}
						else {
							if (line.find("://") == std::string::npos) {
								std::string abs_url;
								if (!line.empty() && line[0] == '/') {
									auto scheme_pos = effective_url.find("://");
									auto host_end = effective_url.find('/', scheme_pos + 3);
									std::string host = (host_end != std::string::npos) ? effective_url.substr(0, host_end) : effective_url;
									abs_url = host + line;
								}
								else {
									abs_url = base_url + line;
								}
								if (abs_url.find('?') == std::string::npos) abs_url += query_params;
								local_m3u8_content += abs_url + "\n";
							}
							else {
								local_m3u8_content += line + "\n";
							}
						}
					}

					std::string local_m3u8_path = Helper::path_to_utf8(target_path) + ".local.m3u8";
					FILE* fp = Helper::xfopen(local_m3u8_path.c_str(), "wb");
					if (fp) {
						fwrite(local_m3u8_content.data(), 1, local_m3u8_content.size(), fp);
						fclose(fp);

						std::vector<std::string> safe_headers;
						for (const auto& h : j.headers) {
							std::string lower_h = h;
							std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(), ::tolower);
							if (lower_h.find("authorization:") != std::string::npos ||
								lower_h.find("cookie:") != std::string::npos) {
								continue;
							}
							safe_headers.push_back(h);
						}

						std::cout << "[HLS] Processing local playlist through FFmpeg..." << std::endl;
						ok = FFmpegHelper::convert_m3u8_to_ts(local_m3u8_path, j.out_path, safe_headers, proxy_, on_progress, msg);

						std::error_code ec_rem;
						std::filesystem::remove(std::filesystem::u8path(local_m3u8_path), ec_rem);
					}
					else {
						ok = false;
						msg = "Could not create local m3u8 file";
					}
				}
			}
			else
			{
				std::cout << "[WORKER] Standard file (non-HLS) is downloading..." << std::endl;
				ok = curl_download_file(j.url, j.out_path, j.headers, on_progress, msg);
			}

			{
				std::lock_guard<std::mutex> lk(mtx_);
				auto it = std::find_if(queue_.begin(), queue_.end(), [&](const Job& q) { return q.id == j.id; });
				if (it != queue_.end()) {
					it->filename = j.filename;
					it->out_path = j.out_path;
					if (ok)
					{
						it->state = Job::State::Done;
						it->message = "ok";
						it->progress = 100.0;
						if (it->course_id)
						{
							auto itp = progress_.find(it->course_id);
							if (itp != progress_.end()) itp->second.done += 1;
						}
					}
					else
					{
						std::string lower_msg = msg;
						std::transform(lower_msg.begin(), lower_msg.end(), lower_msg.begin(), ::tolower);

						if (lower_msg.find("aborted") != std::string::npos || lower_msg.find("user") != std::string::npos) {
							it->state = Job::State::Paused; 
							it->message = "paused";
							std::cout << "[WORKER] Job PAUSED by user." << std::endl;
						}
						else {
							it->state = Job::State::Failed; 
							it->message = msg.empty() ? "failed" : msg;
							std::cout << "[WORKER] JOB FAILED! Reason: " << it->message << std::endl;
						}
					}
				}
			}
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1000));
	}
}
