// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/libraries/avplayer/avplayer_ajm_wrapper.h"
#include "core/libraries/avplayer/avplayer_error.h"
#include "core/libraries/avplayer/avplayer_file_streamer.h"
#include "core/libraries/avplayer/avplayer_handle_streamer.h"
#include "core/libraries/avplayer/avplayer_source.h"
#include "core/libraries/videodec/video_utils.h"
#include "core/memory.h"

#include <magic_enum/magic_enum.hpp>

#include <Ap4.h>
#include <Ap4BitStream.h>
#include <Ap4Mp4AudioInfo.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include "common/support/avdec.h"

struct AP4_Packet {
    AP4_Sample sample;
    AP4_DataBuffer data;
    u32 track_index = 0;
    u32 time_scale = 0;
    bool is_last = false;
};

namespace Libraries::AvPlayer {

constexpr u32 max_video_packets = 4;
constexpr u32 max_audio_packets = 8;

bool AvPlayerSource::Init(const AvPlayerInitData& init_data, std::string_view path) {
    m_memory_replacement = init_data.memory_replacement;
    m_max_num_video_framebuffers =
        std::min(std::max(2, init_data.num_output_video_framebuffers), 16);

    if (init_data.file_replacement.open != nullptr) {
        auto up_data_streamer = std::make_unique<AP4_FileStreamer>(init_data.file_replacement);
        if (!up_data_streamer->Init(path)) {
            return false;
        }
        m_up_data_streamer = std::move(up_data_streamer);
    } else {
        const auto mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        auto handle = mnt->Open(path, /*writable=*/false);
        auto host = handle ? handle->GetHostPath() : mnt->GetHostPath(path);
        if (host) {
            const auto filepath = host.value();
            LOG_INFO(Lib_AvPlayer, "Opening {} via GetHostPath fallback {}", path,
                     filepath.string());
            AP4_ByteStream* stream = nullptr;
            const auto result = AP4_FileByteStream::Create(
                reinterpret_cast<const char*>(filepath.u8string().c_str()),
                AP4_FileByteStream::Mode::STREAM_MODE_READ, stream);
            if (AP4_FAILED(result)) {
                return false;
            }
            m_up_data_streamer.reset(stream);
        } else {
            LOG_INFO(Lib_AvPlayer, "Opening {} through a non-host backend handle", path);
            m_up_data_streamer = std::make_unique<AP4_HandleStreamer>(std::move(handle));
        }
    }
    m_ap4_file = std::make_unique<AP4_File>(*m_up_data_streamer, true);
    FindStreams();
    return true;
}

static AvPlayerStreamType TrackTypeToStreamType(AP4_Track::Type track_type) {
    switch (track_type) {
    case AP4_Track::Type::TYPE_VIDEO:
        return AvPlayerStreamType::Video;
    case AP4_Track::Type::TYPE_AUDIO:
        return AvPlayerStreamType::Audio;
    case AP4_Track::Type::TYPE_SUBTITLES:
        return AvPlayerStreamType::TimedText;
    default:
        LOG_ERROR(Lib_AvPlayer, "Unexpected AVMediaType {}", magic_enum::enum_name(track_type));
        return AvPlayerStreamType::Unknown;
    }
}

static bool IsTrackSupported(AP4_Track* track) {
    auto* desc = track->GetSampleDescription(0);
    auto sample_type = desc->GetType();
    return sample_type == AP4_SampleDescription::TYPE_AVC ||
           sample_type == AP4_SampleDescription::TYPE_HEVC ||
           sample_type == AP4_SampleDescription::TYPE_MPEG;
}

u32 InitAjmContext() {
    using namespace Libraries::Ajm;
    u32 context_id;
    ASSERT(Libraries::Ajm::sceAjmInitialize(0, &context_id) == ORBIS_OK);
    sceAjmModuleRegister(context_id, AjmCodecType::M4aacDec, 0);
    return context_id;
}

AvPlayerSource::AvPlayerSource(AvPlayerStateCallback& state) : m_state(state) {
    static u32 ajm_context_id = InitAjmContext();
    m_ajm_context_id = ajm_context_id;
}

AvPlayerSource::~AvPlayerSource() {
    Stop();
}

bool AvPlayerSource::FindStreams() {
    auto* movie = m_ap4_file->GetMovie();
    if (movie == nullptr) {
        LOG_ERROR(Lib_AvPlayer, "Could not find stream info. NULL movie.");
        return false;
    }
    m_streams.reserve(movie->GetTracks().ItemCount());
    for (size_t track_idx = 0; track_idx < movie->GetTracks().ItemCount(); ++track_idx) {
        AP4_Track* track;
        movie->GetTracks().Get(track_idx, track);
        if (!IsTrackSupported(track)) {
            continue;
        }
        m_streams.push_back({track_idx, CreateStreamInfo(track_idx, track)});
    }
    m_duration = DurationMillis();
    return m_streams.size() >= 0;
}

s32 AvPlayerSource::GetStreamCount() {
    LOG_INFO(Lib_AvPlayer, "Stream Count: {}", m_streams.size());
    return m_streams.size();
}

static u64 TimestampToMillis(s64 timestamp, AVRational time_base) {
    if (timestamp == AV_NOPTS_VALUE || timestamp <= 0 || time_base.num <= 0 || time_base.den <= 0) {
        return 0;
    }

    const auto millis = av_rescale_q(timestamp, time_base, AVRational{1, 1000});
    return millis > 0 ? u64(millis) : 0;
}

static u64 StreamDurationMillis(AP4_Track* p_track) {
    return p_track->GetDurationMs();
}

AvPlayerStreamInfo AvPlayerSource::CreateStreamInfo(size_t idx, AP4_Track* p_track) {
    AvPlayerStreamInfo info = {};
    info.type = TrackTypeToStreamType(p_track->GetType());
    AP4_Sample sample;
    ASSERT_MSG(AP4_SUCCEEDED(p_track->GetSample(0, sample)), "Could not obtain track sample");
    info.start_time = sample.GetDts();
    info.duration = p_track->GetDurationMs();
    auto* language = p_track->GetTrackLanguage();
    if (language != nullptr) {
        LOG_INFO(Lib_AvPlayer, "Stream {} language = {}", idx, language);
    } else {
        LOG_WARNING(Lib_AvPlayer, "Stream {} language is unknown", idx);
    }
    switch (info.type) {
    case AvPlayerStreamType::Video: {
        LOG_INFO(Lib_AvPlayer, "Stream {} is a video stream.", idx);
        info.details.video.aspect_ratio = f32(p_track->GetWidth()) / p_track->GetHeight();
        auto width = Common::AlignUp<u32>(p_track->GetWidth() / 65536, 16);
        auto height = Common::AlignUp<u32>(p_track->GetHeight() / 65536, 16);
        info.details.video.width = width;
        info.details.video.height = height;
        if (language != nullptr) {
            std::memcpy(info.details.video.language_code, language,
                        std::min<size_t>(strlen(language), 3));
        }
        break;
    }
    case AvPlayerStreamType::Audio: {
        auto* desc = AP4_DYNAMIC_CAST(AP4_MpegSampleDescription, p_track->GetSampleDescription(0));
        const auto& dec_info = desc->GetDecoderInfo();
        AP4_Mp4AudioDecoderConfig dec_config;
        ASSERT(AP4_SUCCEEDED(dec_config.Parse(dec_info.GetData(), dec_info.GetDataSize())));
        LOG_INFO(Lib_AvPlayer, "Stream {} is an audio stream.", idx);
        info.details.audio.channel_count = dec_config.m_ChannelCount;
        info.details.audio.sample_rate = dec_config.m_SamplingFrequency;
        info.details.audio.size = 0; // sceAvPlayerGetStreamInfo() is expected to set this to 0
        if (language != nullptr) {
            std::memcpy(info.details.audio.language_code, language,
                        std::min<size_t>(strlen(language), 3));
        }
        break;
    }
    case AvPlayerStreamType::TimedText: {
        info.details.subs.font_size = 12;
        info.details.subs.text_size = 12;
        if (language != nullptr) {
            std::memcpy(info.details.subs.language_code, language,
                        std::min<size_t>(strlen(language), 3));
        }
        break;
    }
    default: {
        UNREACHABLE_MSG("Unknown stream type {}", magic_enum::enum_name(info.type));
    }
    }
    return info;
}

bool AvPlayerSource::GetStreamInfo(u32 stream_index, AvPlayerStreamInfo& info) {
    if (stream_index >= m_streams.size()) {
        LOG_ERROR(Lib_AvPlayer, "Could not get stream {} info.", stream_index);
        return false;
    }
    info = m_streams[stream_index].info;
    return true;
}

bool AvPlayerSource::EnableStream(u32 stream_index) {
    if (m_ap4_file == nullptr || stream_index >= m_streams.size()) {
        return false;
    }
    stream_index = m_streams[stream_index].mp4_index;
    AP4_Track* p_track = nullptr;
    if (AP4_FAILED(m_ap4_file->GetMovie()->GetTracks().Get(stream_index, p_track))) {
        return false;
    }
    switch (p_track->GetType()) {
    case AP4_Track::Type::TYPE_VIDEO: {
        m_p_video_track = p_track;
        m_video_track_index = stream_index;
        LOG_INFO(Lib_AvPlayer, "Video stream {} enabled", stream_index);
        break;
    }
    case AP4_Track::Type::TYPE_AUDIO: {
        m_p_audio_track = p_track;
        m_audio_track_index = stream_index;
        LOG_INFO(Lib_AvPlayer, "Audio stream {} enabled", stream_index);
        break;
    }
    default:
        LOG_WARNING(Lib_AvPlayer, "Unknown stream type {} for stream {}",
                    magic_enum::enum_name(p_track->GetType()), stream_index);
        break;
    }
    return true;
}

void AvPlayerSource::SetLooping(bool is_looping) {
    m_is_looping = is_looping;
}

std::optional<bool> AvPlayerSource::HasFrames(u32 num_frames) {
    return m_video_frames.Size() > num_frames || m_is_eof;
}

AVCodecID SampleTypeToCodecID(AP4_SampleDescription::Type sample_type) {
    switch (sample_type) {
    case AP4_SampleDescription::TYPE_AVC:
        return AV_CODEC_ID_H264;
    case AP4_SampleDescription::TYPE_HEVC:
        return AV_CODEC_ID_HEVC;
    case AP4_SampleDescription::TYPE_MPEG:
        return AV_CODEC_ID_AAC;
    default:
        UNREACHABLE_MSG("Unknown codec type: {}", magic_enum::enum_name(sample_type));
    }
}

bool AvPlayerSource::Start() {
    std::unique_lock lock(m_state_mutex);

    if (m_p_video_track == nullptr && m_p_audio_track == nullptr) {
        LOG_ERROR(Lib_AvPlayer, "Could not start playback. No streams.");
        return false;
    }
    if (m_p_video_track != nullptr) {
        auto* sdesc = m_p_video_track->GetSampleDescription(0);
        auto* vsdesc = AP4_DYNAMIC_CAST(AP4_VideoSampleDescription, sdesc);
        auto codec_id = SampleTypeToCodecID(sdesc->GetType());
        const auto decoder = avcodec_find_decoder(codec_id);
        if (decoder == nullptr) {
            return false;
        }
        m_video_codec_context =
            AVCodecContextPtr(avcodec_alloc_context3(decoder), &ReleaseAVCodecContext);
        u32 width = vsdesc->GetWidth();
        u32 height = vsdesc->GetHeight();
        AP4_String codec_tag{};
        sdesc->GetCodecString(codec_tag);
        AVCodecParameters params{};
        params.codec_type = AVMEDIA_TYPE_VIDEO;
        params.codec_id = codec_id;
        params.codec_tag = *reinterpret_cast<const u32*>(codec_tag.GetChars());
        if (sdesc->GetType() == AP4_SampleDescription::TYPE_AVC) {
            auto* avc_sample_desc = AP4_DYNAMIC_CAST(AP4_AvcSampleDescription, sdesc);
            params.profile = avc_sample_desc->GetProfile();
            params.level = avc_sample_desc->GetLevel();
            if (sdesc->GetFormat() != AP4_SAMPLE_FORMAT_AVC3 &&
                sdesc->GetFormat() != AP4_SAMPLE_FORMAT_AVC4 &&
                sdesc->GetFormat() != AP4_SAMPLE_FORMAT_DVAV) {
                auto* atom = AP4_DYNAMIC_CAST(
                    AP4_AvccAtom, avc_sample_desc->GetDetails().GetChild(AP4_ATOM_TYPE_AVCC));
                auto& bytes = atom->GetRawBytes();
                params.extradata_size = bytes.GetBufferSize();
                params.extradata = reinterpret_cast<u8*>(av_malloc(params.extradata_size));
                std::memcpy(params.extradata, bytes.GetData(), params.extradata_size);
            }
        } else if (sdesc->GetType() == AP4_SampleDescription::TYPE_HEVC) {
            auto* hevc_sample_desc = AP4_DYNAMIC_CAST(AP4_HevcSampleDescription, sdesc);
            params.profile = hevc_sample_desc->GetGeneralProfile();
            params.level = hevc_sample_desc->GetGeneralLevel();
        }
        params.format = AV_SAMPLE_FMT_NONE;
        // params.bit_rate;
        // params.bits_per_coded_sample;
        // params.bits_per_raw_sample;
        params.width = width;
        params.height = height;
        // params.sample_aspect_ratio;
        // params.framerate;
        if (avcodec_parameters_to_context(m_video_codec_context.get(), &params) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not set video stream avcodec parameters.");
            return false;
        }
        if (avcodec_open2(m_video_codec_context.get(), decoder, nullptr) < 0) {
            LOG_ERROR(Lib_AvPlayer, "Could not open avcodec for video stream.");
            return false;
        }
        const auto pitch = Common::AlignUp<u32>(width, 64);
        const auto size = (pitch * Common::AlignUp(height, 16) * 3) / 2;
        for (u64 index = 0; index < m_max_num_video_framebuffers; ++index) {
            m_video_buffers.Push({GuestBuffer(m_memory_replacement, 0x100, size, true)});
        }
    }
    if (m_p_audio_track != nullptr) {
        auto* sample_desc = m_p_audio_track->GetSampleDescription(0);
        auto* mpeg_sample_desc = AP4_DYNAMIC_CAST(AP4_MpegSampleDescription, sample_desc);
        const auto& dec_info = mpeg_sample_desc->GetDecoderInfo();
        AP4_Mp4AudioDecoderConfig dec_config;
        auto result = dec_config.Parse(dec_info.GetData(), dec_info.GetDataSize());
        if (AP4_FAILED(result)) {
            return false;
        }
        auto codec_id = SampleTypeToCodecID(sample_desc->GetType());
        const auto decoder = avcodec_find_decoder(codec_id);
        if (decoder == nullptr) {
            return false;
        }
        AP4_String codec_tag{};
        sample_desc->GetCodecString(codec_tag);
        m_audio_decoder =
            std::make_unique<AjmAacDecoder>(m_ajm_context_id, dec_config.m_ChannelCount);
        m_audio_decoder->Init(dec_config.m_SamplingFrequencyIndex);

        constexpr u8 max_channels = 8;
        constexpr size_t max_sample_size = sizeof(s32);
        const auto size = max_channels * max_sample_size * 1024;
        for (u64 index = 0; index < (m_max_num_video_framebuffers * 2); ++index) {
            m_audio_buffers.Push({GuestBuffer(m_memory_replacement, 0x10, size, false)});
        }
    }
    m_demuxer_thread.Run([this](std::stop_token stop) { this->DemuxerThread(stop); });
    m_video_decoder_thread.Run([this](std::stop_token stop) { this->VideoDecoderThread(stop); });
    m_audio_decoder_thread.Run([this](std::stop_token stop) { this->AudioDecoderThread(stop); });
    m_start_time = std::chrono::high_resolution_clock::now();
    return true;
}

bool AvPlayerSource::Stop() {
    std::unique_lock lock(m_state_mutex);

    m_video_decoder_thread.Stop();
    m_audio_decoder_thread.Stop();
    m_demuxer_thread.Stop();

    m_video_buffers.Clear();
    m_audio_buffers.Clear();
    m_audio_packets.Clear();
    m_video_packets.Clear();
    m_audio_frames.Clear();
    m_video_frames.Clear();

    m_last_audio_ts.reset();
    m_last_data_time.reset();
    m_start_time.reset();
    m_pause_time = {};
    m_pause_duration = {};

    m_is_paused = false;
    m_is_eof = false;

    return true;
}

void AvPlayerSource::Pause() {
    m_pause_time = std::chrono::high_resolution_clock::now();
    m_is_paused = true;
}

void AvPlayerSource::Resume() {
    m_pause_duration += std::chrono::high_resolution_clock::now() - m_pause_time;
    m_is_paused = false;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfo& video_info) {
    AvPlayerFrameInfoEx info{};
    if (!GetVideoData(info)) {
        return false;
    }
    video_info = {};
    video_info.timestamp = u64(info.timestamp);
    video_info.p_data = reinterpret_cast<u8*>(info.p_data);
    video_info.details.video.aspect_ratio = info.details.video.aspect_ratio;
    video_info.details.video.width = info.details.video.width;
    video_info.details.video.height = info.details.video.height;
    return true;
}

bool AvPlayerSource::GetVideoData(AvPlayerFrameInfoEx& video_info) {
    if (!IsActive() || m_is_paused) {
        return false;
    }

    if (m_video_frames.Size() == 0) {
        return false;
    }

    const auto current_time = CurrentTime();
    const auto& new_frame = m_video_frames.Front();
    if (m_state.GetSyncMode() == AvPlayerAvSyncMode::Default) {
        if (m_p_audio_track != nullptr && m_audio_decoder_thread.Joinable()) {
            // Audio is available, sync video with it.
            if (new_frame.info.timestamp > m_last_audio_ts.value_or(0)) {
                return false;
            }
        } else {
            // Sync with the internal timer since audio is not available
            const auto current_time = CurrentTime();
            if (0 < current_time && current_time < new_frame.info.timestamp) {
                return false;
            }
        }
    }

    auto frame = m_video_frames.Pop();
    video_info = frame->info;

    if (frame->is_last) {
        if (m_is_looping) {
            m_state.OnWarning(ORBIS_AVPLAYER_ERROR_WAR_LOOPING_BACK);
        } else {
            m_state.OnEOF();
        }
    }

    m_video_buffers.Push(std::move(*frame));
    m_video_buffers_cv.Notify();

    return true;
}

bool AvPlayerSource::GetAudioData(AvPlayerFrameInfo& audio_info) {
    if (!IsActive() || m_is_paused) {
        return false;
    }

    if (m_audio_frames.Size() == 0) {
        return false;
    }

    auto frame = m_audio_frames.Pop();
    m_last_audio_ts = frame->info.timestamp;
    m_last_data_time = std::chrono::high_resolution_clock::now();
    m_pause_duration = std::chrono::high_resolution_clock::duration(0);

    audio_info = {};
    audio_info.timestamp = frame->info.timestamp;
    audio_info.p_data = reinterpret_cast<u8*>(frame->info.p_data);
    audio_info.details.audio.sample_rate = frame->info.details.audio.sample_rate;
    audio_info.details.audio.size = frame->info.details.audio.size;
    audio_info.details.audio.channel_count = frame->info.details.audio.channel_count;

    m_audio_buffers.Push(std::move(*frame));
    m_audio_buffers_cv.Notify();

    return true;
}

u64 AvPlayerSource::DurationMillis() const {
    u32 duration = 0;
    if (m_p_video_track) {
        duration = std::max(duration, m_p_video_track->GetDurationMs());
    }
    if (m_p_audio_track) {
        duration = std::max(duration, m_p_audio_track->GetDurationMs());
    }
    if (duration == 0) {
        auto* movie = m_ap4_file->GetMovie();
        for (auto track = movie->GetTracks().FirstItem(); track != nullptr;
             track = track->GetNext()) {
            duration = std::max(duration, track->GetData()->GetDurationMs());
        }
    }
    return duration;
}

u64 AvPlayerSource::CurrentTime() {
    if (!m_start_time.has_value()) {
        return 0;
    }

    if (m_is_eof && !IsActive()) {
        return m_duration;
    }
    if (!IsActive()) {
        return 0;
    }

    using namespace std::chrono;
    const auto now = m_is_paused.load() ? m_pause_time : high_resolution_clock::now();
    if (m_p_audio_track) {
        const auto last_audio = m_last_data_time.value_or(now);
        const auto elapsed =
            duration_cast<milliseconds>(now - last_audio - m_pause_duration).count();
        return m_last_audio_ts.value_or(0) + elapsed;
    }

    const auto elapsed =
        duration_cast<milliseconds>(now - m_start_time.value() - m_pause_duration).count();
    if (elapsed <= 0) {
        return 0;
    }
    const auto current_time = u64(elapsed);
    return m_duration != 0 && current_time > m_duration ? m_duration : current_time;
}

bool AvPlayerSource::IsActive() {
    return !m_is_eof || m_audio_packets.Size() != 0 || m_video_packets.Size() != 0 ||
           m_video_frames.Size() != 0 || m_audio_frames.Size() != 0;
}

void AvPlayerSource::ReleaseAVPacket(AVPacket* packet) {
    if (packet != nullptr) {
        av_packet_free(&packet);
    }
}

void AvPlayerSource::ReleaseAVFrame(AVFrame* frame) {
    if (frame != nullptr) {
        av_frame_free(&frame);
    }
}

void AvPlayerSource::ReleaseAVCodecContext(AVCodecContext* context) {
    if (context != nullptr) {
        avcodec_free_context(&context);
    }
}

void AvPlayerSource::ReleaseSWRContext(SwrContext* context) {
    if (context != nullptr) {
        swr_free(&context);
    }
}

void AvPlayerSource::ReleaseSWSContext(SwsContext* context) {
    if (context != nullptr) {
        sws_freeContext(context);
    }
}

void AvPlayerSource::DemuxerThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvDemuxer");

    if (m_p_video_track == nullptr && m_p_audio_track == nullptr) {
        LOG_WARNING(Lib_AvPlayer, "Could not start DEMUXER thread. No streams enabled.");
        return;
    }
    LOG_INFO(Lib_AvPlayer, "Demuxer Thread started");

    bool video_eos = m_p_video_track == nullptr;
    bool audio_eos = m_p_audio_track == nullptr;

    while (!stop.stop_requested()) {
        if (m_video_packets.Size() >= max_video_packets &&
            (m_p_audio_track == nullptr || m_audio_packets.Size() >= max_audio_packets)) {
            std::this_thread::sleep_for(milliseconds(5));
            continue;
        }
        if (!video_eos && m_p_video_track != nullptr &&
            m_video_packets.Size() < max_video_packets) {
            auto up_packet = std::make_unique<AP4_Packet>();
            auto result = m_p_video_track->ReadSample(m_video_sample_index++, up_packet->sample,
                                                      up_packet->data);
            if (AP4_FAILED(result)) {
                LOG_ERROR(Lib_AvPlayer, "Error reading sample from video stream: {}", result);
                break;
            }
            up_packet->track_index = m_video_track_index;
            up_packet->time_scale = m_p_video_track->GetMediaTimeScale();
            if (m_video_sample_index == m_p_video_track->GetSampleCount()) {
                up_packet->is_last = true;
                if (m_is_looping) {
                    m_video_sample_index = 0;
                } else {
                    video_eos = true;
                }
            }
            m_video_packets.Push(std::move(up_packet));
            m_video_packets_cv.Notify();
        }
        if (!audio_eos && m_p_audio_track != nullptr &&
            m_audio_packets.Size() < max_audio_packets) {
            auto up_packet = std::make_unique<AP4_Packet>();
            auto result = m_p_audio_track->ReadSample(m_audio_sample_index++, up_packet->sample,
                                                      up_packet->data);
            if (AP4_FAILED(result)) {
                LOG_ERROR(Lib_AvPlayer, "Error reading sample from audio stream: {}", result);
                break;
            }
            up_packet->track_index = m_audio_track_index;
            up_packet->time_scale = m_p_audio_track->GetMediaTimeScale();
            if (m_audio_sample_index == m_p_audio_track->GetSampleCount()) {
                if (m_is_looping) {
                    m_audio_sample_index = 0;
                } else {
                    audio_eos = true;
                }
            }
            m_audio_packets.Push(std::move(up_packet));
            m_audio_packets_cv.Notify();
        }
        if (video_eos && audio_eos) {
            LOG_INFO(Lib_AvPlayer, "EOF reached in demuxer.");
            break;
        }
    }

    m_is_eof = true;

    m_video_packets_cv.Notify();
    m_audio_packets_cv.Notify();
    m_video_frames_cv.Notify();
    m_audio_frames_cv.Notify();
    m_video_buffers_cv.Notify();
    m_audio_buffers_cv.Notify();

    m_video_decoder_thread.Join();
    m_audio_decoder_thread.Join();

    LOG_INFO(Lib_AvPlayer, "Demuxer Thread exited normally");
    m_demuxer_thread.Join();
}

AvPlayerSource::AVFramePtr AvPlayerSource::ConvertVideoFrame(const AVFrame& frame) {
    auto nv12_frame = AVFramePtr{av_frame_alloc(), &ReleaseAVFrame};
    nv12_frame->best_effort_timestamp = frame.best_effort_timestamp;
    nv12_frame->pts = frame.pts;
    nv12_frame->pkt_dts = frame.pkt_dts < 0 ? 0 : frame.pkt_dts;
    nv12_frame->format = AV_PIX_FMT_NV12;
    nv12_frame->width = frame.width;
    nv12_frame->height = frame.height;
    nv12_frame->sample_aspect_ratio = frame.sample_aspect_ratio;
    nv12_frame->crop_top = frame.crop_top;
    nv12_frame->crop_bottom = frame.crop_bottom;
    nv12_frame->crop_left = frame.crop_left;
    nv12_frame->crop_right = frame.crop_right;

    av_frame_get_buffer(nv12_frame.get(), 0);

    if (m_sws_context == nullptr) {
        m_sws_context =
            SWSContextPtr(sws_getContext(frame.width, frame.height, AVPixelFormat(frame.format),
                                         nv12_frame->width, nv12_frame->height, AV_PIX_FMT_NV12,
                                         SWS_FAST_BILINEAR, nullptr, nullptr, nullptr),
                          &ReleaseSWSContext);
    }
    const auto res = sws_scale(m_sws_context.get(), frame.data, frame.linesize, 0, frame.height,
                               nv12_frame->data, nv12_frame->linesize);
    if (res < 0) {
        LOG_ERROR(Lib_AvPlayer, "Could not convert to NV12: {}", av_err2str(res));
        return AVFramePtr{nullptr, &ReleaseAVFrame};
    }
    return nv12_frame;
}

void AvPlayerSource::PrepareVideoFrame(Frame& frame, const AVFrame& av_frame) {
    DEBUG_ASSERT(av_frame.format == AV_PIX_FMT_NV12);

    auto p_buffer = frame.buffer.GetBuffer();
    Videodec::CopyNV12Data(p_buffer.data(), av_frame);

    const auto timestamp =
        AP4_ConvertTime(av_frame.pkt_dts, m_p_video_track->GetMediaTimeScale(), 1000);

    const auto width = Common::AlignUp<u32>(av_frame.width, 16);
    const auto pitch = Common::AlignUp<u32>(av_frame.width, 64);
    const auto height = Common::AlignUp<u32>(av_frame.height, 16);

    frame.info = {
        .p_data = p_buffer.data(),
        .timestamp = timestamp,
        .details =
            {
                .video =
                    {
                        .width = width,
                        .height = height,
                        .aspect_ratio = (float)av_q2d(av_frame.sample_aspect_ratio),
                        .crop_left_offset = 0,
                        .crop_right_offset = u32(av_frame.crop_right + (pitch - av_frame.width)),
                        .crop_top_offset = 0,
                        .crop_bottom_offset =
                            u32(av_frame.crop_bottom + (height - av_frame.height)),
                        .pitch = pitch,
                        .luma_bit_depth = 8,
                        .chroma_bit_depth = 8,
                    },
            },
    };
}

void AvPlayerSource::VideoDecoderThread(std::stop_token stop) {
    using namespace std::chrono;
    Common::SetCurrentThreadName("shadPS4:AvVideoDecoder");

    LOG_INFO(Lib_AvPlayer, "Video Decoder Thread started");
    while ((!m_is_eof || m_video_packets.Size() != 0) && !stop.stop_requested()) {
        if (m_video_packets.Size() == 0 &&
            !m_video_packets_cv.Wait(stop, [this] { return m_video_packets.Size() != 0; })) {
            continue;
        }
        const auto packet = m_video_packets.Pop();
        if (!packet.has_value()) {
            continue;
        }

        AVPacket av_packet{};
        av_packet.pts = (*packet)->sample.GetCts();
        av_packet.dts = (*packet)->sample.GetDts();
        av_packet.data = const_cast<uint8_t*>((*packet)->data.GetData());
        av_packet.size = (*packet)->data.GetDataSize();
        av_packet.stream_index = m_streams[(*packet)->track_index].mp4_index;
        av_packet.flags = (*packet)->sample.IsSync() ? AV_PKT_FLAG_KEY : 0;
        av_packet.duration = (*packet)->sample.GetDuration();
        av_packet.pos = (*packet)->sample.GetOffset();
        av_packet.time_base = {1, s32((*packet)->time_scale)};
        auto res = avcodec_send_packet(m_video_codec_context.get(), &av_packet);
        if (res < 0 && res != AVERROR(EAGAIN)) {
            m_state.OnError();
            LOG_ERROR(Lib_AvPlayer, "Could not send packet to the video codec. Error = {}",
                      av_err2str(res));
            return;
        }
        while (res >= 0) {
            if (m_video_buffers.Size() == 0 &&
                !m_video_buffers_cv.Wait(stop, [this] { return m_video_buffers.Size() != 0; })) {
                break;
            }
            auto up_frame = AVFramePtr(av_frame_alloc(), &ReleaseAVFrame);
            res = avcodec_receive_frame(m_video_codec_context.get(), up_frame.get());
            if (res < 0) {
                if (res == AVERROR_EOF) {
                    LOG_INFO(Lib_AvPlayer, "EOF reached in video decoder");
                    return;
                } else if (res != AVERROR(EAGAIN)) {
                    LOG_ERROR(Lib_AvPlayer,
                              "Could not receive frame from the video codec. Error = {}",
                              av_err2str(res));
                    m_state.OnError();
                    return;
                }
            } else {
                auto buffer = m_video_buffers.Pop();
                if (!buffer.has_value()) {
                    // Video buffers queue was cleared. This means that player was stopped.
                    break;
                }
                buffer->is_last = (*packet)->is_last;
                if (up_frame->format != AV_PIX_FMT_NV12) {
                    const auto nv12_frame = ConvertVideoFrame(*up_frame);
                    if (nv12_frame == nullptr) {
                        m_state.OnError();
                        return;
                    }
                    PrepareVideoFrame(*buffer, *nv12_frame);
                    m_video_frames.Push(std::move(*buffer));
                } else {
                    PrepareVideoFrame(*buffer, *up_frame);
                    m_video_frames.Push(std::move(*buffer));
                }
                m_video_frames_cv.Notify();
            }
        }
    }

    LOG_INFO(Lib_AvPlayer, "Video Decoder Thread exited normally");
    m_video_decoder_thread.Join();
}

void AvPlayerSource::PrepareAudioFrame(Frame& frame, const AP4_Packet& packet) {
    const auto& info = m_streams[packet.track_index].info;

    auto data = frame.buffer.GetBuffer();
    frame.info = {
        .p_data = data.data(),
        .timestamp =
            AP4_ConvertTime(packet.sample.GetDts(), m_p_audio_track->GetMediaTimeScale(), 1000),
        .details =
            {
                .audio =
                    {
                        .channel_count = info.details.audio.channel_count,
                        .sample_rate = info.details.audio.sample_rate,
                        .size = u32(data.size()),
                    },
            },
    };
    frame.is_last = packet.is_last;
}

void AvPlayerSource::AudioDecoderThread(std::stop_token stop) {
    Common::SetCurrentThreadName("shadPS4:AvAudioDecoder");
    LOG_INFO(Lib_AvPlayer, "Audio Decoder Thread started");

    while ((!m_is_eof || m_audio_packets.Size() != 0) && !stop.stop_requested()) {
        if (!m_audio_buffers_cv.Wait(stop, [this] { return m_audio_buffers.Size() != 0; })) {
            break;
        }
        auto buffer = m_audio_buffers.Pop();

        while ((!m_is_eof || m_audio_packets.Size() != 0) && !stop.stop_requested()) {
            if (m_audio_packets.Size() == 0 &&
                !m_audio_packets_cv.Wait(stop, [this] { return m_audio_packets.Size() != 0; })) {
                continue;
            }
            const auto packet = m_audio_packets.Pop();
            if (!packet.has_value()) {
                continue;
            }
            const u8* input = (*packet)->data.GetData();
            const size_t size = (*packet)->data.GetDataSize();
            const auto bytes_written = m_audio_decoder->Decode(std::span<const u8>(input, size),
                                                               buffer->buffer.GetBuffer());
            if (bytes_written > 0) {
                PrepareAudioFrame(*buffer, **packet);
                m_audio_frames.Push(std::move(*buffer));
                m_audio_frames_cv.Notify();
                break;
            }
        }
    }

    LOG_INFO(Lib_AvPlayer, "Audio Decoder Thread exited normally");
    m_audio_decoder_thread.Join();
}

} // namespace Libraries::AvPlayer
