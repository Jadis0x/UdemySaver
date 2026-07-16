#include "EncryptedDashHelper.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <thread>

namespace {
constexpr const char* kUserAgent = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";

struct CurlHandle {
    CURL* value = curl_easy_init();
    ~CurlHandle() { if (value) curl_easy_cleanup(value); }
};

#ifdef _WIN32
std::wstring extended_utf8_path(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), wide.data(), size) <= 0) return {};

    const DWORD absolute_size = GetFullPathNameW(wide.c_str(), 0, nullptr, nullptr);
    if (absolute_size > 0) {
        std::wstring absolute(static_cast<std::size_t>(absolute_size), L'\0');
        const DWORD written = GetFullPathNameW(wide.c_str(), absolute_size, absolute.data(), nullptr);
        if (written > 0) {
            absolute.resize(written);
            wide = std::move(absolute);
        }
    }
    if (wide.rfind(L"\\\\?\\", 0) == 0) return wide;
    if (wide.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + wide.substr(2);
    return L"\\\\?\\" + wide;
}
#endif

FILE* open_utf8_file(const std::string& path, const char* mode) {
#ifdef _WIN32
    auto to_wide = [](const std::string& value) {
        if (value.empty()) return std::wstring{};
        const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), nullptr, 0);
        if (size <= 0) return std::wstring{};
        std::wstring result(static_cast<std::size_t>(size), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            value.data(), static_cast<int>(value.size()), result.data(), size) <= 0) return std::wstring{};
        return result;
    };
    const std::wstring wide_path = extended_utf8_path(path);
    const std::wstring wide_mode = to_wide(mode);
    if (wide_path.empty() || wide_mode.empty()) return nullptr;
    FILE* file = nullptr;
    return _wfopen_s(&file, wide_path.c_str(), wide_mode.c_str()) == 0 ? file : nullptr;
#else
    return std::fopen(path.c_str(), mode);
#endif
}

void remove_utf8_file(const std::string& path) {
#ifdef _WIN32
    const std::wstring wide = extended_utf8_path(path);
    if (!wide.empty()) _wremove(wide.c_str());
#else
    std::remove(path.c_str());
#endif
}

bool rename_utf8_file(const std::string& from, const std::string& to, std::string& error) {
#ifdef _WIN32
    const std::wstring wide_from = extended_utf8_path(from);
    const std::wstring wide_to = extended_utf8_path(to);
    if (wide_from.empty() || wide_to.empty()) { error = "invalid UTF-8 output path"; return false; }
    _wremove(wide_to.c_str());
    if (_wrename(wide_from.c_str(), wide_to.c_str()) == 0) return true;
#else
    std::remove(to.c_str());
    if (std::rename(from.c_str(), to.c_str()) == 0) return true;
#endif
    error = std::strerror(errno);
    return false;
}

size_t write_string(void* data, size_t size, size_t count, void* userdata) {
    const size_t bytes = size * count;
    static_cast<std::string*>(userdata)->append(static_cast<char*>(data), bytes);
    return bytes;
}

struct SegmentTemplate {
    std::string initialization;
    std::string media;
    long long start_number = 1;
    long long timescale = 1;
    long long duration = 0;
    std::vector<long long> timeline;
};

struct Representation {
    std::string id;
    std::string mime_type;
    long long bandwidth = 0;
    int height = 0;
    SegmentTemplate segments;
};

std::string attribute(const std::string& attrs, const std::string& name) {
    const std::regex expression("(?:^|\\s)" + name + R"(\s*=\s*["']([^"']*)["'])",
        std::regex::icase);
    std::smatch match;
    return std::regex_search(attrs, match, expression) ? match[1].str() : std::string{};
}

long long integer_attribute(const std::string& attrs, const std::string& name, long long fallback) {
    const std::string value = attribute(attrs, name);
    if (value.empty()) return fallback;
    try { return std::stoll(value); }
    catch (...) { return fallback; }
}

double parse_iso8601_duration(const std::string& value) {
    const std::regex expression(
        R"(^P(?:([0-9.]+)D)?(?:T(?:([0-9.]+)H)?(?:([0-9.]+)M)?(?:([0-9.]+)S)?)?$)",
        std::regex::icase);
    std::smatch match;
    if (!std::regex_match(value, match, expression)) return 0.0;
    auto number = [&](std::size_t index) {
        return match[index].matched ? std::stod(match[index].str()) : 0.0;
    };
    return number(1) * 86400.0 + number(2) * 3600.0 + number(3) * 60.0 + number(4);
}

bool parse_segment_template(const std::string& source, SegmentTemplate& result) {
    std::smatch match;
    const std::regex paired(
        R"(<SegmentTemplate\b([^>]*)>([\s\S]*?)</SegmentTemplate\s*>)",
        std::regex::icase);
    const std::regex empty(R"(<SegmentTemplate\b([^>]*)/\s*>)", std::regex::icase);

    std::string attrs;
    std::string body;
    if (std::regex_search(source, match, paired)) {
        attrs = match[1].str();
        body = match[2].str();
    }
    else if (std::regex_search(source, match, empty)) {
        attrs = match[1].str();
    }
    else {
        return false;
    }

    result.initialization = attribute(attrs, "initialization");
    result.media = attribute(attrs, "media");
    result.start_number = integer_attribute(attrs, "startNumber", 1);
    result.timescale = std::max(1LL, integer_attribute(attrs, "timescale", 1));
    result.duration = integer_attribute(attrs, "duration", 0);

    long long current_time = 0;
    const std::regex segment(R"(<S\b([^>]*)/?>)", std::regex::icase);
    for (std::sregex_iterator it(body.begin(), body.end(), segment), end; it != end; ++it) {
        const std::string segment_attrs = (*it)[1].str();
        const long long duration = integer_attribute(segment_attrs, "d", 0);
        const long long explicit_time = integer_attribute(segment_attrs, "t", -1);
        const long long repeat = integer_attribute(segment_attrs, "r", 0);
        if (duration <= 0 || repeat < 0) return false;
        if (explicit_time >= 0) current_time = explicit_time;
        for (long long index = 0; index <= repeat; ++index) {
            result.timeline.push_back(current_time);
            current_time += duration;
        }
    }
    return !result.initialization.empty() && !result.media.empty();
}

std::vector<Representation> parse_representations(
    const std::string& mpd,
    const std::string& wanted_type,
    double presentation_duration) {
    std::vector<Representation> output;
    const std::regex adaptation(
        R"(<AdaptationSet\b([^>]*)>([\s\S]*?)</AdaptationSet\s*>)",
        std::regex::icase);
    const std::regex representation(
        R"(<Representation\b([^>]*)(?:/\s*>|>([\s\S]*?)</Representation\s*>))",
        std::regex::icase);

    for (std::sregex_iterator set_it(mpd.begin(), mpd.end(), adaptation), end; set_it != end; ++set_it) {
        const std::string set_attrs = (*set_it)[1].str();
        const std::string set_body = (*set_it)[2].str();
        std::string set_mime = attribute(set_attrs, "mimeType");
        std::string lower_mime = set_mime;
        std::transform(lower_mime.begin(), lower_mime.end(), lower_mime.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (lower_mime.find(wanted_type) == std::string::npos) continue;

        SegmentTemplate inherited;
        const bool has_inherited = parse_segment_template(set_body, inherited);

        for (std::sregex_iterator rep_it(set_body.begin(), set_body.end(), representation);
            rep_it != end; ++rep_it) {
            const std::string rep_attrs = (*rep_it)[1].str();
            const std::string rep_body = (*rep_it)[2].str();
            Representation rep;
            rep.id = attribute(rep_attrs, "id");
            rep.mime_type = attribute(rep_attrs, "mimeType");
            if (rep.mime_type.empty()) rep.mime_type = set_mime;
            rep.bandwidth = integer_attribute(rep_attrs, "bandwidth", 0);
            rep.height = static_cast<int>(integer_attribute(rep_attrs, "height", 0));

            if (!parse_segment_template(rep_body, rep.segments)) {
                if (!has_inherited) continue;
                rep.segments = inherited;
            }

            if (rep.segments.timeline.empty() && rep.segments.duration > 0 && presentation_duration > 0) {
                const auto count = static_cast<long long>(std::ceil(
                    presentation_duration * static_cast<double>(rep.segments.timescale) /
                    static_cast<double>(rep.segments.duration)));
                long long time = 0;
                for (long long index = 0; index < count; ++index) {
                    rep.segments.timeline.push_back(time);
                    time += rep.segments.duration;
                }
            }

            if (!rep.id.empty() && !rep.segments.timeline.empty()) output.push_back(std::move(rep));
        }
    }
    return output;
}

std::string replace_all(std::string value, const std::string& needle, const std::string& replacement) {
    std::size_t position = 0;
    while ((position = value.find(needle, position)) != std::string::npos) {
        value.replace(position, needle.size(), replacement);
        position += replacement.size();
    }
    return value;
}

std::string substitute_template(
    std::string value,
    const Representation& representation,
    long long number,
    long long time) {
    value = replace_all(std::move(value), "$RepresentationID$", representation.id);
    value = replace_all(std::move(value), "$Bandwidth$", std::to_string(representation.bandwidth));
    value = replace_all(std::move(value), "$Time$", std::to_string(time));

    const std::regex number_token(R"(\$Number(?:%0([0-9]+)d)?\$)");
    std::smatch match;
    while (std::regex_search(value, match, number_token)) {
        std::ostringstream formatted;
        if (match[1].matched) formatted << std::setw(std::stoi(match[1].str())) << std::setfill('0');
        formatted << number;
        value.replace(static_cast<std::size_t>(match.position()), static_cast<std::size_t>(match.length()),
            formatted.str());
    }
    return value;
}

std::string resolve_url(const std::string& mpd_url, const std::string& relative) {
    if (relative.find("://") != std::string::npos) return relative;

    std::string without_query = mpd_url;
    std::string query;
    const auto query_position = without_query.find('?');
    if (query_position != std::string::npos) {
        query = without_query.substr(query_position);
        without_query.resize(query_position);
    }

    std::string result;
    if (!relative.empty() && relative.front() == '/') {
        const auto scheme = without_query.find("://");
        const auto host_end = without_query.find('/', scheme == std::string::npos ? 0 : scheme + 3);
        result = (host_end == std::string::npos ? without_query : without_query.substr(0, host_end)) + relative;
    }
    else {
        const auto slash = without_query.rfind('/');
        result = (slash == std::string::npos ? std::string{} : without_query.substr(0, slash + 1)) + relative;
    }
    if (!query.empty() && result.find('?') == std::string::npos) result += query;
    return result;
}

bool fetch_manifest(
    const std::string& url,
    const std::vector<std::string>& headers,
    const std::string& proxy,
    std::string& body,
    std::string& msg) {
    CurlHandle curl;
    if (!curl.value) { msg = "curl init failed"; return false; }
    curl_slist* header_list = nullptr;
    for (const auto& header : headers) header_list = curl_slist_append(header_list, header.c_str());

    curl_easy_setopt(curl.value, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.value, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl.value, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl.value, CURLOPT_USERAGENT, kUserAgent);
    curl_easy_setopt(curl.value, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl.value, CURLOPT_HTTPHEADER, header_list);
    curl_easy_setopt(curl.value, CURLOPT_WRITEFUNCTION, write_string);
    curl_easy_setopt(curl.value, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl.value, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
    curl_easy_setopt(curl.value, CURLOPT_TIMEOUT_MS, 30000L);
    if (!proxy.empty()) {
        curl_easy_setopt(curl.value, CURLOPT_PROXY, proxy.c_str());
        curl_easy_setopt(curl.value, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl.value, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const CURLcode result = curl_easy_perform(curl.value);
    long response_code = 0;
    curl_easy_getinfo(curl.value, CURLINFO_RESPONSE_CODE, &response_code);
    if (header_list) curl_slist_free_all(header_list);
    if (result != CURLE_OK) { msg = curl_easy_strerror(result); return false; }
    if (response_code < 200 || response_code >= 300) {
        msg = "MPD request returned HTTP " + std::to_string(response_code);
        return false;
    }
    return true;
}

struct ProgressContext {
    std::function<bool(double, double)>* callback = nullptr;
    long long completed_bytes = 0;
};

int transfer_progress(void* userdata, curl_off_t, curl_off_t downloaded, curl_off_t, curl_off_t) {
    auto* context = static_cast<ProgressContext*>(userdata);
    if (!context->callback || !*context->callback) return 0;
    return (*context->callback)(static_cast<double>(context->completed_bytes + downloaded), 0.0) ? 0 : 1;
}

bool append_url(
    CURL* curl,
    const std::string& url,
    FILE* file,
    long long& bytes,
    const std::vector<std::string>& headers,
    const std::string& proxy,
    std::function<bool(double, double)>& callback,
    std::string& msg) {
    for (int attempt = 1; attempt <= 3; ++attempt) {
        if (!curl) { msg = "curl init failed"; return false; }
        // Reset options but retain libcurl's connection cache. Creating a new
        // easy handle per small DASH segment repeats DNS/TLS and is very slow.
        curl_easy_reset(curl);
        curl_slist* header_list = nullptr;
        for (const auto& header : headers) header_list = curl_slist_append(header_list, header.c_str());
        std::string payload;
        ProgressContext progress_context{ &callback, bytes };

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, kUserAgent);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, header_list);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_string);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &payload);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progress_context);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
        if (!proxy.empty()) {
            curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        const CURLcode result = curl_easy_perform(curl);
        long response_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response_code);
        if (header_list) curl_slist_free_all(header_list);
        if (result == CURLE_ABORTED_BY_CALLBACK) {
            msg = "Operation aborted by user";
            return false;
        }
        if (result == CURLE_OK && response_code >= 200 && response_code < 300) {
            if (std::fwrite(payload.data(), 1, payload.size(), file) != payload.size()) {
                msg = "failed to write encrypted segment";
                return false;
            }
            bytes += static_cast<long long>(payload.size());
            return true;
        }

        msg = result != CURLE_OK
            ? curl_easy_strerror(result)
            : "segment request returned HTTP " + std::to_string(response_code);
        if (attempt < 3) std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
    }
    return false;
}

bool download_representation(
    const std::string& mpd_url,
    const Representation& representation,
    const std::string& output_path,
    const std::vector<std::string>& headers,
    const std::string& proxy,
    std::function<bool(double, double)>& callback,
    long long& total_bytes,
    std::string& msg) {
    if (callback && !callback(static_cast<double>(total_bytes), 0.0)) {
        msg = "Operation aborted by user";
        return false;
    }
    const std::string partial_path = output_path + ".part";
    remove_utf8_file(partial_path);
    FILE* file = open_utf8_file(partial_path, "wb");
    if (!file) { msg = "cannot create encrypted output file"; return false; }
    CurlHandle curl;
    if (!curl.value) { std::fclose(file); msg = "curl init failed"; return false; }

    auto close_and_fail = [&] {
        std::fclose(file);
        remove_utf8_file(partial_path);
        return false;
    };

    const auto& segments = representation.segments;
    std::string init = substitute_template(segments.initialization, representation,
        segments.start_number, segments.timeline.front());
    if (!append_url(curl.value, resolve_url(mpd_url, init), file, total_bytes, headers, proxy, callback, msg)) {
        msg = "initialization segment for representation " + representation.id + ": " + msg;
        return close_and_fail();
    }

    for (std::size_t index = 0; index < segments.timeline.size(); ++index) {
        const long long number = segments.start_number + static_cast<long long>(index);
        const std::string media = substitute_template(segments.media, representation,
            number, segments.timeline[index]);
        if (!append_url(curl.value, resolve_url(mpd_url, media), file, total_bytes, headers, proxy, callback, msg)) {
            msg = "media segment " + std::to_string(number) + " for representation " +
                representation.id + ": " + msg;
            return close_and_fail();
        }
    }

    std::fclose(file);
    std::string rename_error;
    if (!rename_utf8_file(partial_path, output_path, rename_error)) {
        msg = "cannot finalize encrypted output: " + rename_error;
        return false;
    }
    return true;
}

std::vector<std::string> extract_pssh(const std::string& mpd) {
    std::vector<std::string> values;
    const std::regex expression(R"(<(?:[A-Za-z0-9_-]+:)?pssh\b[^>]*>\s*([^<\s]+)\s*</(?:[A-Za-z0-9_-]+:)?pssh\s*>)",
        std::regex::icase);
    for (std::sregex_iterator it(mpd.begin(), mpd.end(), expression), end; it != end; ++it) {
        const std::string value = (*it)[1].str();
        if (std::find(values.begin(), values.end(), value) == values.end()) values.push_back(value);
    }
    return values;
}

bool write_metadata(const std::string& path, const std::string& mpd_url,
    const std::string& license_url, const std::vector<std::string>& pssh,
    const std::string& video_path, const std::string& audio_path, std::string& msg) {
    nlohmann::json metadata = {
        {"encrypted", true},
        {"decrypted", false},
        {"mpd_url", mpd_url},
        {"license_url", license_url},
        {"pssh", pssh},
        {"video_file", video_path},
        {"audio_file", audio_path},
        {"note", "Encrypted media only; no license challenge, content-key extraction or decryption was performed."}
    };
    const std::string payload = metadata.dump(2);
    FILE* file = open_utf8_file(path, "wb");
    if (!file) { msg = "cannot create DRM metadata file"; return false; }
    const bool ok = std::fwrite(payload.data(), 1, payload.size(), file) == payload.size();
    std::fclose(file);
    if (!ok) { msg = "cannot write DRM metadata file"; return false; }
    return true;
}
}

bool EncryptedDashHelper::download(
    const std::string& mpd_url,
    const std::string& output_base,
    const std::vector<std::string>& extra_headers,
    const std::string& proxy,
    const std::string& license_url,
    std::function<bool(double, double)> on_progress,
    std::string& msg) {
    msg.clear();
    std::string mpd;
    if (!fetch_manifest(mpd_url, extra_headers, proxy, mpd, msg)) return false;

    std::smatch mpd_match;
    double presentation_duration = 0.0;
    const std::regex mpd_tag(R"(<MPD\b([^>]*)>)", std::regex::icase);
    if (std::regex_search(mpd, mpd_match, mpd_tag)) {
        presentation_duration = parse_iso8601_duration(
            attribute(mpd_match[1].str(), "mediaPresentationDuration"));
    }

    auto videos = parse_representations(mpd, "video", presentation_duration);
    auto audios = parse_representations(mpd, "audio", presentation_duration);
    if (videos.empty() || audios.empty()) {
        msg = "MPD does not contain downloadable video and audio representations";
        return false;
    }

    const auto video = std::max_element(videos.begin(), videos.end(), [](const auto& left, const auto& right) {
        return left.height != right.height ? left.height < right.height : left.bandwidth < right.bandwidth;
    });
    const auto audio = std::max_element(audios.begin(), audios.end(), [](const auto& left, const auto& right) {
        return left.bandwidth < right.bandwidth;
    });

    const std::string video_path = output_base + ".video.mp4";
    const std::string audio_path = output_base + ".audio.m4a";
    const std::string metadata_path = output_base + ".drm.json";
    const auto pssh = extract_pssh(mpd);
    if (!write_metadata(metadata_path, mpd_url, license_url, pssh, video_path, audio_path, msg)) return false;
    long long bytes = 0;
    std::cout << "[DRM] Video representation: " << video->id << " (" << video->height << "p)" << std::endl;
    if (!download_representation(mpd_url, *video, video_path, extra_headers, proxy,
        on_progress, bytes, msg)) return false;
    std::cout << "[DRM] Audio representation: " << audio->id << std::endl;
    if (!download_representation(mpd_url, *audio, audio_path, extra_headers, proxy,
        on_progress, bytes, msg)) return false;

    if (on_progress) on_progress(static_cast<double>(bytes), static_cast<double>(bytes));
    msg = "encrypted DASH video and audio downloaded without decryption";
    return true;
}
