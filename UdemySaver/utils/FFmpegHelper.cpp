#include "FFmpegHelper.h"
#include "Helper.h"

#include <chrono>
#include <filesystem>
#include <system_error>
#include <map> 

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
#include <iostream>
}

bool FFmpegHelper::convert_m3u8_to_ts(
	const std::string& url,
	const std::string& out_path,
	const std::vector<std::string>& extra_headers,
	const std::string& proxy,
	std::function<void(double, double)> on_progress,
	std::string& msg) {
	msg.clear();

	std::string tmp_path = out_path + ".part";
	{
		std::error_code ec;
		std::filesystem::remove(std::filesystem::u8path(tmp_path), ec);
	}

	auto cleanup_tmp = [&]()
		{
			std::error_code ec;
			std::filesystem::remove(std::filesystem::u8path(tmp_path), ec); 
		};

	AVFormatContext* in_ctx = nullptr;
	AVFormatContext* out_ctx = nullptr;
	AVDictionary* in_opts = nullptr;

	auto last_progress = std::chrono::steady_clock::now();
	const std::chrono::milliseconds progress_interval(350);
	long long estimated_total_bytes = 0;
	long long bytes_written = 0;

	auto report_progress = [&](bool force = false)
		{
			if (!on_progress) return;
			auto now = std::chrono::steady_clock::now();
			if (!force && now - last_progress < progress_interval) return;
			last_progress = now;

			double total = static_cast<double>(estimated_total_bytes);
			double current = static_cast<double>(bytes_written);
			if (total > 0.0)
			{
				if (current > total) current = total;
				on_progress(current, total);
			}
			else
			{
				on_progress(current, 0.0);
			}
		};

	auto cleanup_ctx = [&]()
		{
			if (in_ctx)
			{
				avformat_close_input(&in_ctx);
				in_ctx = nullptr;
			}
			if (out_ctx)
			{
				if (!(out_ctx->oformat->flags & AVFMT_NOFILE) && out_ctx->pb)
				{
					avio_closep(&out_ctx->pb);
				}
				avformat_free_context(out_ctx);
				out_ctx = nullptr;
			}
			if (in_opts)
			{
				av_dict_free(&in_opts);
				in_opts = nullptr;
			}
		};

	std::string header_block;
	std::string custom_ua = "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/122.0.0.0 Safari/537.36";

	if (!extra_headers.empty())
	{
		header_block.reserve(extra_headers.size() * 32);
		for (const auto& h : extra_headers)
		{
			std::string lower_h = h;
			std::transform(lower_h.begin(), lower_h.end(), lower_h.begin(), [](unsigned char c) { return (char)std::tolower(c); });

			if (lower_h.find("user-agent:") == 0) {
				custom_ua = h.substr(11);
				custom_ua.erase(0, custom_ua.find_first_not_of(" \t")); 
				continue;
			}

			header_block += h;
			if (h.size() < 2 || h[h.size() - 1] != '\n' || h[h.size() - 2] != '\r')
			{
				header_block += "\r\n";
			}
		}
		if (!header_block.empty() && (header_block.back() != '\n' || header_block.size() < 2 || header_block[header_block.size() - 2] != '\r'))
		{
			header_block += "\r\n";
		}
		if (!header_block.empty()) {
			av_dict_set(&in_opts, "headers", header_block.c_str(), 0);
		}
	}

	av_dict_set(&in_opts, "user_agent", custom_ua.c_str(), 0);
	av_dict_set(&in_opts, "allowed_extensions", "ALL", 0);
	av_dict_set(&in_opts, "protocol_whitelist", "file,http,https,tcp,tls,crypto,data", 0);
	av_dict_set(&in_opts, "reconnect", "1", 0);
	av_dict_set(&in_opts, "reconnect_streamed", "1", 0);
	av_dict_set(&in_opts, "reconnect_delay_max", "2", 0);
	av_dict_set(&in_opts, "http_persistent", "1", 0);  
	av_dict_set(&in_opts, "multiple_requests", "1", 0);

	if (!proxy.empty())
	{
		av_dict_set(&in_opts, "http_proxy", proxy.c_str(), 0);
	}

	int ret = avformat_open_input(&in_ctx, url.c_str(), nullptr, &in_opts);
	if (ret < 0)
	{
		cleanup_ctx();
		cleanup_tmp();
		msg = "avformat_open_input failed: " + Helper::ff_errstr(ret);
		std::cout << "[FFmpeg] ERROR: Input could not be opened - " << msg << std::endl;
		return false;
	}

	ret = avformat_find_stream_info(in_ctx, nullptr);
	if (ret < 0)
	{
		cleanup_ctx();
		cleanup_tmp();
		msg = "avformat_find_stream_info failed: " + Helper::ff_errstr(ret);
		std::cout << "[FFmpeg] ERROR: Stream information not found - " << msg << std::endl;
		return false;
	}

	double duration_seconds = (in_ctx->duration > 0) ? (double)in_ctx->duration / AV_TIME_BASE : 0.0;
	double bitrate_total = (in_ctx->bit_rate > 0) ? (double)in_ctx->bit_rate : 0.0;
	if (bitrate_total <= 0.0)
	{
		for (unsigned int i = 0; i < in_ctx->nb_streams; ++i)
		{
			auto* st = in_ctx->streams[i];
			if (st && st->codecpar && st->codecpar->bit_rate > 0)
			{
				bitrate_total += (double)st->codecpar->bit_rate;
			}
		}
	}
	if (bitrate_total > 0.0 && duration_seconds > 0.0)
	{
		estimated_total_bytes = static_cast<long long>((bitrate_total / 8.0) * duration_seconds);
	}

	report_progress(true);

	ret = avformat_alloc_output_context2(&out_ctx, nullptr, "mp4", tmp_path.c_str());
	if (ret < 0 || !out_ctx)
	{
		int err = ret < 0 ? ret : AVERROR_UNKNOWN;
		cleanup_ctx();
		cleanup_tmp();
		msg = "avformat_alloc_output_context2 failed: " + Helper::ff_errstr(err);
		std::cout << "[FFmpeg] ERROR: Failed to allocate output context - " << msg << std::endl;
		return false;
	}

	int video_idx = av_find_best_stream(in_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
	int audio_idx = av_find_best_stream(in_ctx, AVMEDIA_TYPE_AUDIO, -1, video_idx, nullptr, 0);

	std::cout << "[FFmpeg] Selected Stream Indices-> Video: " << video_idx << ", Audio : " << audio_idx << std::endl;

	std::map<int, AVStream*> stream_mapping;

	if (video_idx >= 0) {
		AVStream* in_stream = in_ctx->streams[video_idx];
		AVStream* out_stream = avformat_new_stream(out_ctx, nullptr);
		if (!out_stream) {
			cleanup_ctx(); cleanup_tmp();
			msg = "avformat_new_stream failed for video";
			return false;
		}
		ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
		if (ret < 0) {
			cleanup_ctx(); cleanup_tmp();
			msg = "avcodec_parameters_copy failed: " + Helper::ff_errstr(ret);
			return false;
		}
		out_stream->codecpar->codec_tag = 0;
		out_stream->time_base = in_stream->time_base;
		out_stream->avg_frame_rate = in_stream->avg_frame_rate;
		out_stream->r_frame_rate = in_stream->r_frame_rate;
		out_stream->sample_aspect_ratio = in_stream->sample_aspect_ratio;
		stream_mapping[video_idx] = out_stream;
	}

	if (audio_idx >= 0) {
		AVStream* in_stream = in_ctx->streams[audio_idx];
		AVStream* out_stream = avformat_new_stream(out_ctx, nullptr);
		if (!out_stream) {
			cleanup_ctx(); cleanup_tmp();
			msg = "avformat_new_stream failed for audio";
			return false;
		}
		ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
		if (ret < 0) {
			cleanup_ctx(); cleanup_tmp();
			msg = "avcodec_parameters_copy failed: " + Helper::ff_errstr(ret);
			return false;
		}
		out_stream->codecpar->codec_tag = 0;
		out_stream->time_base = in_stream->time_base;
		out_stream->avg_frame_rate = in_stream->avg_frame_rate;
		out_stream->r_frame_rate = in_stream->r_frame_rate;
		out_stream->sample_aspect_ratio = in_stream->sample_aspect_ratio;
		stream_mapping[audio_idx] = out_stream;
	}

	if (!(out_ctx->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&out_ctx->pb, tmp_path.c_str(), AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			cleanup_ctx();
			cleanup_tmp();
			msg = "avio_open failed: " + Helper::ff_errstr(ret);
			std::cout << "[FFmpeg] ERROR: Could not open output file - " << msg << std::endl;
			return false;
		}
	}

	ret = avformat_write_header(out_ctx, nullptr);
	if (ret < 0)
	{
		cleanup_ctx();
		cleanup_tmp();
		msg = "avformat_write_header failed: " + Helper::ff_errstr(ret);
		return false;
	}

	AVPacket* pkt = av_packet_alloc();
	if (!pkt) {
		cleanup_ctx();
		cleanup_tmp();
		msg = "av_packet_alloc failed";
		std::cout << "[FFmpeg] ERROR: Memory allocation failed(AVPacket)" << std::endl;
		return false;
	}

	while (true)
	{
		ret = av_read_frame(in_ctx, pkt);
		if (ret == AVERROR_EOF) break;
		if (ret == AVERROR(EAGAIN))
		{
			continue;
		}
		if (ret < 0)
		{
			av_packet_free(&pkt);
			cleanup_ctx();
			cleanup_tmp();
			msg = "av_read_frame failed: " + Helper::ff_errstr(ret);
			std::cout << "[FFmpeg] ERROR: Failed to read frame - " << msg << std::endl;
			return false;
		}

		if (stream_mapping.find(pkt->stream_index) == stream_mapping.end()) {
			av_packet_unref(pkt);
			continue;
		}

		AVStream* in_stream = in_ctx->streams[pkt->stream_index];
		AVStream* out_stream = stream_mapping[pkt->stream_index];

		pkt->stream_index = out_stream->index;

		if (pkt->pts != AV_NOPTS_VALUE)
		{
			pkt->pts = av_rescale_q_rnd(pkt->pts, in_stream->time_base, out_stream->time_base,
				(AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		}
		if (pkt->dts != AV_NOPTS_VALUE)
		{
			pkt->dts = av_rescale_q_rnd(pkt->dts, in_stream->time_base, out_stream->time_base,
				(AVRounding)(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
		}
		if (pkt->duration > 0)
		{
			pkt->duration = av_rescale_q(pkt->duration, in_stream->time_base, out_stream->time_base);
		}
		pkt->pos = -1;

		ret = av_interleaved_write_frame(out_ctx, pkt);
		if (ret >= 0 && pkt->size > 0)
		{
			bytes_written += pkt->size;
		}
		av_packet_unref(pkt);
		if (ret < 0)
		{
			av_packet_free(&pkt);
			cleanup_ctx();
			cleanup_tmp();
			msg = "av_interleaved_write_frame failed: " + Helper::ff_errstr(ret);
			std::cout << "[FFmpeg] ERROR: Failed to write output frame - " << msg << std::endl;
			return false;
		}

		report_progress();
	}
	av_packet_free(&pkt);

	if (out_ctx && out_ctx->pb)
	{
		avio_flush(out_ctx->pb);
	}

	ret = av_write_trailer(out_ctx);
	if (ret < 0)
	{
		cleanup_ctx();
		cleanup_tmp();
		msg = "av_write_trailer failed: " + Helper::ff_errstr(ret);
		return false;
	}

	cleanup_ctx();

	std::error_code ec;
	std::filesystem::remove(std::filesystem::u8path(out_path), ec);
	ec.clear();

	std::filesystem::rename(std::filesystem::u8path(tmp_path), std::filesystem::u8path(out_path), ec);
	if (ec)
	{
		cleanup_tmp();
		msg = "rename failed: " + ec.message();
		std::cout << "[FFmpeg] ERROR: File renaming failed. Msg: " << msg << std::endl;
		return false;
	}

	if (on_progress)
	{
		std::error_code fec;
		auto final_size = std::filesystem::file_size(out_path, fec);
		if (!fec && final_size > 0)
		{
			estimated_total_bytes = static_cast<long long>(final_size);
			on_progress(static_cast<double>(final_size), static_cast<double>(final_size));
		}
		else if (estimated_total_bytes > 0)
		{
			on_progress(static_cast<double>(estimated_total_bytes), static_cast<double>(estimated_total_bytes));
		}
		else
		{
			on_progress(1.0, 1.0);
		}
	}

	std::cout << "[FFmpeg] Conversion completed successfully : " << out_path << std::endl;
	return true;
}
