#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <pulse/def.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavformat/avformat.h>
#include <pulse/simple.h>
#include <pulse/pulseaudio.h>
#include <pthread.h>
#include <termios.h>
#include <fcntl.h>
#include <string.h>

#define SAMPLE_RATE 44100

typedef struct{
  char *name;
  int counter;
  float volume;
  int sample_rate;
} shared;

void *threads(void *args){
  shared *share = (shared *)args;
  AVFormatContext *fmt_ctx = NULL;
  avformat_open_input(&fmt_ctx,share->name,NULL,NULL);
  avformat_find_stream_info(fmt_ctx,NULL);
  int s_index = av_find_best_stream(fmt_ctx,AVMEDIA_TYPE_AUDIO,-1,-1,NULL,0);
  AVStream *stream_audi = fmt_ctx->streams[s_index];

  const AVCodec *cdx = avcodec_find_decoder(stream_audi->codecpar->codec_id);
  AVCodecContext *cdx_ctx = avcodec_alloc_context3(cdx);
  avcodec_parameters_to_context(cdx_ctx,stream_audi->codecpar);
  avcodec_open2(cdx_ctx,cdx,NULL);
  
  SwrContext *swr = NULL;
  AVChannelLayout in_channel = cdx_ctx->ch_layout;
  AVChannelLayout out_channel = AV_CHANNEL_LAYOUT_STEREO;

  swr_alloc_set_opts2(&swr,&out_channel,AV_SAMPLE_FMT_S16,SAMPLE_RATE,&in_channel,cdx_ctx->sample_fmt,cdx_ctx->sample_rate,0,NULL);
  swr_init(swr);

  printf("File name : %s\n",share->name);
  printf("sample rate : %d\n",share->sample_rate);

  AVPacket *pack = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  uint8_t *out_buf = NULL;
  int out_nsize = 0;

  pa_sample_spec ss;
  ss.format = PA_SAMPLE_S16LE;
  ss.channels = 2;
  ss.rate = share->sample_rate;
  printf("format sample : %d\n",ss.format);
  printf("channel : %d\n",ss.channels);
  pa_simple *simple = pa_simple_new(NULL,"Test",PA_STREAM_PLAYBACK,NULL,"Music",&ss,NULL,NULL,NULL);
  while(av_read_frame(fmt_ctx,pack) >= 0 && share->counter == 0){
    if(pack->stream_index == s_index){
      if(avcodec_send_packet(cdx_ctx,pack) >= 0){
        while(avcodec_receive_frame(cdx_ctx,frame) >= 0){
          int out_samples = swr_get_out_samples(swr,frame->nb_samples);
          av_samples_alloc(&out_buf,&out_nsize,cdx_ctx->sample_rate,out_samples,AV_SAMPLE_FMT_S16,1);
          int convert = swr_convert(swr,&out_buf,out_samples,(const uint8_t **)frame->extended_data,frame->nb_samples);

          int size = convert * cdx_ctx->ch_layout.nb_channels * av_get_bytes_per_sample(AV_SAMPLE_FMT_S16);

          int16_t *pcm = (int16_t *)out_buf;
          int16_t nsamples = size/sizeof(int16_t);
          for(int i = 0;i < nsamples;i++){
            float factor = powf(share->volume,2.0f);
            float scaled = pcm[i] * factor;
            if(scaled > 32767) scaled = 32767;
            if(scaled < -32767) scaled = -32767;
            pcm[i] = (int16_t)scaled;
          }
          pa_simple_write(simple,out_buf,size,NULL);
          av_freep(&out_buf);
        }
      }
    }
    av_packet_unref(pack);
  }
  av_frame_free(&frame);
  av_packet_free(&pack);
  swr_free(&swr);
  avcodec_free_context(&cdx_ctx);
  avformat_close_input(&fmt_ctx);
  pa_simple_drain(simple,NULL);
  pa_simple_free(simple);
  return NULL;
}

int main(int argc,char **argv){
  if(argc < 3){
    printf("[USE] : %s [filename] [sample_rate]\n sample rate normal : 44100\n",argv[0]);
    return 0;
  }
  shared *share = malloc(sizeof(shared));
  share->name = strdup(argv[1]);
  share->sample_rate = atoi(argv[2]);
  share->counter = 0;
  share->volume = 1.0f;
  pthread_t tid;
  pthread_create(&tid,NULL,threads,share);
  struct termios old,new;
  tcgetattr(STDIN_FILENO,&old);
  new = old;
  new.c_lflag &= ~(ICANON | ECHO);
  tcsetattr(STDIN_FILENO,TCSANOW,&new);
  int ret = fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO,F_GETFL,0) | O_NONBLOCK);
  // printf("Vol : %f",share->volume);
  // fflush(stdout);
  char c;
  while(1){
    ssize_t n = read(STDIN_FILENO,&c,1);
    if(n > 0){
      if(c == 'q'){
        share->counter = 1;
        printf("[INFO] : waiting free resource\n");
        break;
      }
      else if(c == '1'){
        share->volume += 0.05f;
        if(share->volume > 150.0f) share->volume = 150.0f;
      }
      else if(c == '2'){
        share->volume -= 0.05f;
        if(share->volume < 0.0f) share->volume = 0.0f;
      }
      printf("\r\033[KVol : %f",share->volume);
      fflush(stdout);
    }
  }
  free(share);
  pthread_detach(tid);
  tcsetattr(STDIN_FILENO,TCSANOW,&old);
  return 0;
}
