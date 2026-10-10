/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswresample/swresample.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  const struct {
    const char *name;
    const char *(*license)(void);
    const char *(*configuration)(void);
  } libraries[] = {
    {"avformat", avformat_license, avformat_configuration},
    {"avcodec", avcodec_license, avcodec_configuration},
    {"avutil", avutil_license, avutil_configuration},
    {"swresample", swresample_license, swresample_configuration}
  };
  const char *required[] = {"--disable-gpl", "--disable-version3", "--disable-nonfree",
    "--disable-autodetect", "--enable-shared", "--disable-static", "--build-suffix=-pipetune"};
  int failed = 0;
  for (size_t i = 0; i < sizeof(libraries) / sizeof(libraries[0]); ++i) {
    const char *license = libraries[i].license();
    printf("%s: %s\n", libraries[i].name, license);
    if (strcmp(license, "LGPL version 2.1 or later") != 0) failed = 1;
    const char *configuration = libraries[i].configuration();
    for (size_t j = 0; j < sizeof(required) / sizeof(required[0]); ++j) {
      if (strstr(configuration, required[j]) == NULL) {
        fprintf(stderr, "%s: missing %s\n", libraries[i].name, required[j]);
        failed = 1;
      }
    }
  }
  if (failed) return 1;
  const char *formats[] = {"wav", "aiff", "flac", "mp3", "ogg", "mov"};
  for (size_t i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i) {
    if (av_find_input_format(formats[i]) == NULL) {
      fprintf(stderr, "Required audio container is unavailable: %s\n", formats[i]);
      return 1;
    }
  }
  const enum AVCodecID codecs[] = {AV_CODEC_ID_PCM_U8, AV_CODEC_ID_PCM_S16LE,
    AV_CODEC_ID_PCM_S24LE, AV_CODEC_ID_PCM_S24BE, AV_CODEC_ID_PCM_S32LE,
    AV_CODEC_ID_PCM_F32LE, AV_CODEC_ID_PCM_F32BE, AV_CODEC_ID_PCM_F64LE,
    AV_CODEC_ID_PCM_ALAW, AV_CODEC_ID_PCM_MULAW, AV_CODEC_ID_ADPCM_IMA_WAV,
    AV_CODEC_ID_ADPCM_IMA_QT, AV_CODEC_ID_ADPCM_MS, AV_CODEC_ID_FLAC,
    AV_CODEC_ID_MP3, AV_CODEC_ID_VORBIS, AV_CODEC_ID_AAC, AV_CODEC_ID_ALAC, AV_CODEC_ID_OPUS};
  for (size_t i = 0; i < sizeof(codecs) / sizeof(codecs[0]); ++i) {
    if (avcodec_find_decoder(codecs[i]) == NULL) {
      fprintf(stderr, "Required audio decoder is unavailable: %s\n", avcodec_get_name(codecs[i]));
      return 1;
    }
  }
  void *iterator = NULL;
  const AVCodec *codec;
  while ((codec = av_codec_iterate(&iterator)) != NULL) {
    if (codec->type != AVMEDIA_TYPE_AUDIO || av_codec_is_encoder(codec)) {
      fprintf(stderr, "Unexpected video, subtitle or encoder: %s\n", codec->name);
      return 1;
    }
  }
  iterator = NULL;
  if (avio_enum_protocols(&iterator, 0) != NULL) {
    fputs("Audio libraries must not open files or network protocols\n", stderr);
    return 1;
  }
  puts("Private LGPL audio decoding dependencies are available");
  return 0;
}
