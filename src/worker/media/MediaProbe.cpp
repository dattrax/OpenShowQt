#include "media/MediaProbe.h"

#include "media/FFmpeg.h"

namespace {

// av_err2str is a C99 compound-literal macro and not portable C++; turn
// error codes into text through a buffer instead.
std::string ffError(int err) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buf, sizeof(buf));
    return std::string(buf);
}

double rationalToDouble(AVRational rate) {
    return rate.den > 0 ? static_cast<double>(rate.num) / static_cast<double>(rate.den) : 0.0;
}

std::string describeChannelLayout(const AVCodecParameters* par) {
    char buf[128] = {};
    if (av_channel_layout_describe(&par->ch_layout, buf, sizeof(buf)) > 0) {
        return std::string(buf);
    }
    return {};
}

}  // namespace

namespace openshow::media {

ProbeStatus probeMedia(const std::string& path, MediaInfo* out, std::string* error) {
    auto fail = [&](std::string message) {
        if (error) {
            *error = std::move(message);
        }
        return ProbeStatus::kOpenFailed;
    };

    AVFormatContext* format = nullptr;
    int err = avformat_open_input(&format, path.c_str(), nullptr, nullptr);
    if (err < 0) {
        return fail("cannot open '" + path + "': " + ffError(err));
    }

    err = avformat_find_stream_info(format, nullptr);
    if (err < 0) {
        avformat_close_input(&format);
        return fail("cannot read stream info from '" + path + "': " + ffError(err));
    }

    out->path = path;
    out->formatName = format->iformat && format->iformat->name ? format->iformat->name : "";
    // duration is in AV_TIME_BASE units and AV_NOPTS_VALUE when unknown.
    out->durationSeconds =
        format->duration > 0 ? static_cast<double>(format->duration) / AV_TIME_BASE : 0.0;

    for (unsigned i = 0; i < format->nb_streams; ++i) {
        const AVStream* stream = format->streams[i];
        const AVCodecParameters* par = stream->codecpar;
        const AVCodec* decoder = avcodec_find_decoder(par->codec_id);

        if (par->codec_type == AVMEDIA_TYPE_VIDEO) {
            VideoStreamInfo info;
            info.index = static_cast<int>(stream->index);
            info.codec = avcodec_get_name(par->codec_id);
            if (decoder && par->profile != AV_PROFILE_UNKNOWN) {
                if (const char* name = av_get_profile_name(decoder, par->profile)) {
                    info.profile = name;
                }
            }
            info.width = static_cast<int>(par->width);
            info.height = static_cast<int>(par->height);
            if (const char* name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(par->format))) {
                info.pixelFormat = name;
            }
            info.frameRate = rationalToDouble(av_guess_frame_rate(format,
                                                                   const_cast<AVStream*>(stream),
                                                                   nullptr));
            // Without an average the guess came from the codec-tag hints.
            info.frameRateConstant = stream->avg_frame_rate.num != 0;
            info.colorSpace = av_color_space_name(par->color_space);
            info.colorRange = av_color_range_name(par->color_range);
            info.decoderAvailable = decoder != nullptr;
            out->videoStreams.push_back(std::move(info));
        } else if (par->codec_type == AVMEDIA_TYPE_AUDIO) {
            AudioStreamInfo info;
            info.index = static_cast<int>(stream->index);
            info.codec = avcodec_get_name(par->codec_id);
            info.sampleRate = static_cast<int>(par->sample_rate);
            info.channels = par->ch_layout.nb_channels;
            info.channelLayout = describeChannelLayout(par);
            if (const char* name = av_get_sample_fmt_name(
                    static_cast<AVSampleFormat>(par->format))) {
                info.sampleFormat = name;
            }
            info.decoderAvailable = decoder != nullptr;
            out->audioStreams.push_back(std::move(info));
        }
    }

    avformat_close_input(&format);

    bool usable = false;
    for (const auto& video : out->videoStreams) {
        usable = usable || video.decoderAvailable;
    }
    for (const auto& audio : out->audioStreams) {
        usable = usable || audio.decoderAvailable;
    }
    if (!usable) {
        if (error) {
            std::string list;
            for (const auto& v : out->videoStreams) {
                list += " video:" + v.codec;
            }
            for (const auto& a : out->audioStreams) {
                list += " audio:" + a.codec;
            }
            *error = "'" + path + "' has no stream this FFmpeg can decode (" +
                     (list.empty() ? std::string("no media streams") : list.substr(1)) + ")";
        }
        return ProbeStatus::kNoUsableStreams;
    }
    return ProbeStatus::kOk;
}

}  // namespace openshow::media
