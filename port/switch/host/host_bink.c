/*
HOST_BINK.C

The game's movies (the .bik files of bink/, next to maps/: the intro, the attract mode,
the credits), decoded with FFmpeg's Bink decoders for the guest's Bink SDK
(port/linux/src/bink_host.c), which paces them: each frame the guest asks
for, the host demuxes up to it, converting the picture to the X8R8G8B8 rows
of the game's texture, and sending the sound decoded meanwhile, as 48 kHz
stereo, to the audio output (host_audio.c), which mixes it into the game's.

One movie at a time, as the game plays them.
*/

#include "host.h"

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
#include <string.h>
#include <switch.h>

#define MOVIE_HANDLE 1

static struct
{
	AVFormatContext *format;
	AVCodecContext *video;
	AVCodecContext *audio;
	int video_stream, audio_stream;
	AVPacket *packet;
	AVFrame *frame;
	AVFrame *audio_frame;
	struct SwsContext *scale;
	SwrContext *resample;
	int has_picture;
	int ended;
} movie;

static AVCodecContext *decoder_open(AVStream *stream)
{
	const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
	AVCodecContext *context;

	if (!codec || !(context = avcodec_alloc_context3(codec)))
		return NULL;
	if (avcodec_parameters_to_context(context, stream->codecpar) < 0 || avcodec_open2(context, codec, NULL) < 0)
	{
		avcodec_free_context(&context);
		return NULL;
	}
	return context;
}

void host_bink_close(uint32_t handle)
{
	(void)handle;
	host_audio_movie_stop();
	sws_freeContext(movie.scale);
	swr_free(&movie.resample);
	av_frame_free(&movie.frame);
	av_frame_free(&movie.audio_frame);
	av_packet_free(&movie.packet);
	avcodec_free_context(&movie.video);
	avcodec_free_context(&movie.audio);
	avformat_close_input(&movie.format);
	memset(&movie, 0, sizeof(movie));
}

uint32_t host_bink_open(const char *path, uint32_t *description)
{
	AVStream *stream;
	AVRational rate;

	if (movie.format)
		host_bink_close(MOVIE_HANDLE);
	if (avformat_open_input(&movie.format, path, NULL, NULL) < 0)
		return 0;
	if (avformat_find_stream_info(movie.format, NULL) < 0)
		goto failed;
	movie.video_stream = av_find_best_stream(movie.format, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
	movie.audio_stream = av_find_best_stream(movie.format, AVMEDIA_TYPE_AUDIO, -1, movie.video_stream, NULL, 0);
	if (movie.video_stream < 0)
		goto failed;
	stream = movie.format->streams[movie.video_stream];
	movie.video = decoder_open(stream);
	if (!movie.video)
		goto failed;
	if (movie.audio_stream >= 0)
	{
		AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

		movie.audio = decoder_open(movie.format->streams[movie.audio_stream]);
		if (movie.audio && swr_alloc_set_opts2(&movie.resample, &stereo, AV_SAMPLE_FMT_S16, 48000,
				&movie.audio->ch_layout, movie.audio->sample_fmt, movie.audio->sample_rate, 0, NULL) >= 0 &&
			swr_init(movie.resample) >= 0)
			host_audio_movie_start();
		else
			host_logf(HOST_LOG_WARN, "movie %s: its sound cannot play", path);
	}
	movie.packet = av_packet_alloc();
	movie.frame = av_frame_alloc();
	movie.audio_frame = av_frame_alloc();
	if (!movie.packet || !movie.frame || !movie.audio_frame)
		goto failed;
	rate = stream->avg_frame_rate.num ? stream->avg_frame_rate : stream->r_frame_rate;
	description[0] = (uint32_t)movie.video->width;
	description[1] = (uint32_t)movie.video->height;
	description[2] = (uint32_t)(stream->nb_frames > 0 ? stream->nb_frames :
		(rate.num && stream->duration > 0 ? av_rescale_q(stream->duration, stream->time_base, av_inv_q(rate)) : 0));
	description[3] = (uint32_t)(rate.num > 0 ? rate.num : 30);
	description[4] = (uint32_t)(rate.den > 0 ? rate.den : 1);
	return MOVIE_HANDLE;

failed:
	host_logf(HOST_LOG_WARN, "movie %s: cannot decode it", path);
	host_bink_close(MOVIE_HANDLE);
	return 0;
}

/* the sound decoded from a packet, to the audio output */
static void audio_packet(const AVPacket *packet)
{
	if (!movie.audio || !movie.resample || avcodec_send_packet(movie.audio, packet) < 0)
		return;
	while (avcodec_receive_frame(movie.audio, movie.audio_frame) >= 0)
	{
		int16_t samples[8192 * 2];
		uint8_t *out = (uint8_t *)samples;
		int frames = swr_convert(movie.resample, &out, 8192, (const uint8_t **)movie.audio_frame->extended_data,
			movie.audio_frame->nb_samples);

		if (frames > 0)
			host_audio_movie_put(samples, (uint32_t)frames);
	}
}

int host_bink_decode(uint32_t handle)
{
	(void)handle;
	if (!movie.format || movie.ended)
		return 0;
	for (;;)
	{
		int result;

		/* a frame the decoder already has */
		result = avcodec_receive_frame(movie.video, movie.frame);
		if (result >= 0)
		{
			movie.has_picture = 1;
			return 1;
		}
		if (result == AVERROR_EOF)
		{
			movie.ended = 1;
			return 0;
		}
		if (av_read_frame(movie.format, movie.packet) < 0)
		{
			/* the end: what the decoder still holds */
			avcodec_send_packet(movie.video, NULL);
			continue;
		}
		if (movie.packet->stream_index == movie.video_stream)
			avcodec_send_packet(movie.video, movie.packet);
		else if (movie.packet->stream_index == movie.audio_stream)
			audio_packet(movie.packet);
		av_packet_unref(movie.packet);
	}
}

void host_bink_copy(uint32_t handle, void *destination, int pitch, uint32_t height)
{
	uint8_t *planes[4] = { destination, NULL, NULL, NULL };
	int pitches[4] = { pitch, 0, 0, 0 };

	(void)handle;
	if (!movie.has_picture || !destination)
		return;
	/* (the texture's X8R8G8B8: blue, green, red, unused, in memory order) */
	movie.scale = sws_getCachedContext(movie.scale, movie.frame->width, movie.frame->height,
		(enum AVPixelFormat)movie.frame->format, movie.frame->width, (int)height < movie.frame->height ?
		(int)height : movie.frame->height, AV_PIX_FMT_BGR0, SWS_POINT, NULL, NULL, NULL);
	if (movie.scale)
		sws_scale(movie.scale, (const uint8_t *const *)movie.frame->data, movie.frame->linesize, 0,
			movie.frame->height, planes, pitches);
}
