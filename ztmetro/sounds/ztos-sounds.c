// SPDX-License-Identifier: GPL-2.0-or-later
// Four predecoded voices share one event stream; taps never queue new streams.
#define _GNU_SOURCE
#include <pulse/pulseaudio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define VOICES 4
#define RATE 48000
#define IDLE_MS 500
struct sound { int16_t *pcm; size_t frames; };
struct voice { int sound; size_t position; double gain; uint64_t age; bool active; };
#define SOUNDS 10
static struct sound sounds[SOUNDS];
static struct voice voices[VOICES];
static uint64_t next_age, last_tap;
static pa_context *context;
static pa_stream *stream;
static pa_mainloop *loop;
static pa_mainloop_api *api;
static pa_time_event *idle_event, *scan_event;
static bool corked = true, scanning, again;
static int fd, playing, reported = -1;
static char socket_path[108], ctl_path[256];
// Indices are the shell's protocol (ztmetro.c, enum ui_sound): keyboard 0..2,
// then the Windows Phone system sounds, converted to 48 kHz 16-bit.
static const char *const files[SOUNDS] = {
 "keyboard/KbdKeyTap.wav", "keyboard/KbdSpacebar.wav", "keyboard/KbdFunction.wav",
 "system/lock.wav", "system/unlock.wav", "system/notify.wav", "system/copy.wav",
 "system/menu.wav", "system/battery-low.wav", "system/pip.wav",
};

static uint64_t now_ms(void) {
 struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
 return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static struct timeval after_ms(int ms) {
 struct timeval tv; gettimeofday(&tv, NULL);
 tv.tv_sec += ms / 1000; tv.tv_usec += (ms % 1000) * 1000;
 if (tv.tv_usec >= 1000000) { tv.tv_sec++; tv.tv_usec -= 1000000; }
 return tv;
}
static uint32_t le32(const unsigned char *p) {
 return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;
}
static void load_sound(int index) {
 char path[256]; snprintf(path, sizeof(path), "/usr/local/share/sounds/ztos/%s", files[index]);
 FILE *f = fopen(path, "rb"); if (!f) return;
 unsigned char header[12], chunk[8], *raw = NULL;
 uint32_t rate = 0, size = 0; unsigned channels = 0;
 if (fread(header,1,12,f)!=12 || memcmp(header,"RIFF",4) || memcmp(header+8,"WAVE",4)) goto done;
 while (fread(chunk,1,8,f)==8) {
  uint32_t n=le32(chunk+4); if(n>4*1024*1024) break;
  if(!memcmp(chunk,"fmt ",4) && n>=16) {
   unsigned char fmt[16]; if(fread(fmt,1,16,f)!=16) break;
   if(fmt[0]!=1 || fmt[1] || fmt[14]!=16 || fmt[15]) break;
   channels=fmt[2] | (unsigned)fmt[3]<<8; rate=le32(fmt+4);
   if(fseek(f,n-16+(n&1),SEEK_CUR)) break;
  } else if(!memcmp(chunk,"data",4)) {
   size=n; raw=malloc(n);
   if(!raw || fread(raw,1,n,f)!=n) { free(raw); raw=NULL; }
   break;
  } else if(fseek(f,n+(n&1),SEEK_CUR)) break;
 }
 if(raw && (channels==1 || channels==2) && (rate==44100 || rate==48000) && size%(channels*2)==0) {
  size_t count=size/(channels*2);
  size_t frames=count ? (count*RATE+rate-1)/rate : 0;
  int16_t *pcm=frames ? malloc(frames*2*sizeof(*pcm)) : NULL;
  if(pcm) {
   for(size_t i=0;i<frames;i++) {
    double at=(double)i*rate/RATE; size_t a=(size_t)at, b=a+1<count?a+1:a;
    if(a>=count) a=b=count-1;
    double fraction=at-a;
    for(unsigned c=0;c<2;c++) {
     unsigned channel=channels==1?0:c;
     int16_t x=(int16_t)((unsigned)raw[(a*channels+channel)*2] | (unsigned)raw[(a*channels+channel)*2+1]<<8);
     int16_t y=(int16_t)((unsigned)raw[(b*channels+channel)*2] | (unsigned)raw[(b*channels+channel)*2+1]<<8);
     pcm[i*2+c]=(int16_t)(x+(y-x)*fraction);
    }
   }
   sounds[index]=(struct sound){pcm,frames};
  }
 }
done:
 free(raw); fclose(f);
}
static void trigger_voice(int sound, unsigned volume) {
 int pick=-1;
 for(int i=0;i<VOICES;i++) if(!voices[i].active) {pick=i;break;}
 if(pick<0) {
  pick=0;
  for(int i=1;i<VOICES;i++) if(voices[i].age<voices[pick].age)pick=i;
 }
 voices[pick]=(struct voice){sound,0,volume/100.0,++next_age,true};
 last_tap=now_ms();
}
static void mix(int16_t *out, size_t frames) {
 memset(out,0,frames*2*sizeof(*out));
 for(size_t frame=0;frame<frames;frame++) {
  int sum[2]={0,0}, count=0;
  for(int i=0;i<VOICES;i++) {
   struct voice *v=&voices[i]; if(!v->active)continue;
   struct sound *s=&sounds[v->sound];
   if(v->position>=s->frames) {v->active=false;continue;}
   count++;
   for(int c=0;c<2;c++)sum[c]+=(int)(s->pcm[v->position*2+c]*v->gain);
   if(++v->position==s->frames)v->active=false;
  }
  // A lone key uses its requested volume. Only overlapping voices share
  // headroom, instead of permanently attenuating every tap by 11 dB.
  if(count>1)for(int c=0;c<2;c++)sum[c]/=count;
  for(int c=0;c<2;c++)out[frame*2+c]=sum[c]>32767?32767:sum[c]<-32768?-32768:sum[c];
 }
}
static void write_audio(pa_stream *s,size_t bytes,void *data) {
 if(pa_stream_get_state(s)!=PA_STREAM_READY)return;
 void *buffer=NULL;
 if(pa_stream_begin_write(s,&buffer,&bytes)<0 || !buffer)return;
 bytes-=bytes%4;
 if(!bytes) {pa_stream_cancel_write(s);return;}
 // Always supply the entire requested block, padding the tail with silence.
 // Short, underfilled writes followed by immediate corking caused underruns.
 mix(buffer,bytes/4);
 if(pa_stream_write(s,buffer,bytes,NULL,0,PA_SEEK_RELATIVE)<0)pa_stream_cancel_write(s);
}
static void idle(pa_mainloop_api *a,pa_time_event *event,const struct timeval *tv,void *data) {
 bool active=false;
 for(int i=0;i<VOICES;i++)active|=voices[i].active;
 if(active || now_ms()-last_tap<IDLE_MS) {
  struct timeval next=after_ms(50); a->time_restart(event,&next); return;
 }
 a->time_free(event);idle_event=NULL;
 if(stream && pa_stream_get_state(stream)==PA_STREAM_READY && !corked) {
  // The grace period exceeds our maximum buffered audio; only silence remains.
  corked=true;pa_operation *op=pa_stream_cork(stream,1,NULL,NULL);
  if(op)pa_operation_unref(op);
 }
}
static void stream_state(pa_stream *s,void *data) {
 pa_stream_state_t state=pa_stream_get_state(s);
 if(state==PA_STREAM_FAILED || state==PA_STREAM_TERMINATED) {
  pa_stream_set_state_callback(s,NULL,NULL);pa_stream_set_write_callback(s,NULL,NULL);
  if(stream==s)stream=NULL;
  pa_stream_unref(s);memset(voices,0,sizeof(voices));corked=true;
 }
}
static void create_stream(void) {
 if(stream)return;
 pa_sample_spec spec={PA_SAMPLE_S16LE,RATE,2};
 pa_buffer_attr attr={.maxlength=2048*4,.tlength=512*4,.prebuf=0,.minreq=128*4,.fragsize=(uint32_t)-1};
 pa_proplist *props=pa_proplist_new();
 pa_proplist_sets(props,PA_PROP_MEDIA_ROLE,"event");
 pa_proplist_sets(props,PA_PROP_APPLICATION_ID,"os.ztos.sounds");
 stream=pa_stream_new_with_proplist(context,"Keyboard taps (4 voices)",&spec,NULL,props);
 pa_proplist_free(props);if(!stream)return;
 pa_stream_set_write_callback(stream,write_audio,NULL);
 pa_stream_set_state_callback(stream,stream_state,NULL);corked=true;
 if(pa_stream_connect_playback(stream,NULL,&attr,PA_STREAM_START_CORKED|PA_STREAM_ADJUST_LATENCY,NULL,NULL)<0) {
  pa_stream_set_state_callback(stream,NULL,NULL);pa_stream_unref(stream);stream=NULL;
 }
}
static void tell_audio(void) {
 int value=playing>0;if(value==reported)return;
 int out=open(ctl_path,O_WRONLY|O_NONBLOCK|O_CLOEXEC);
 if(out>=0) {
  char text[48];int n=snprintf(text,sizeof(text),"audio-active %d\n",value);
  if(write(out,text,n)==n)reported=value;
  close(out);
 }
}
static void scan(void);
static void input_info(pa_context *c,const pa_sink_input_info *info,int eol,void *data) {
 if(eol) {scanning=false;tell_audio();if(again){again=false;scan();}return;}
 if(!info)return;
 const char *role=pa_proplist_gets(info->proplist,PA_PROP_MEDIA_ROLE);
 const char *app=pa_proplist_gets(info->proplist,PA_PROP_APPLICATION_ID);
 if(app && !strcmp(app,"os.ztos.sounds"))return;
 // ztOS's own routing (ztos-speaker.out, a loopback with no role) uncorks
 // whenever anything plays, the volume pip too: it is not media playing.
 const char *node=pa_proplist_gets(info->proplist,"node.name");
 if(node && !strncmp(node,"ztos-",5))return;
 if(!info->corked && (!role || (strcmp(role,"event") && strcmp(role,"phone"))))playing++;
}
static void scan(void) {
 if(scanning){again=true;return;}
 playing=0;scanning=true;
 pa_operation *op=pa_context_get_sink_input_info_list(context,input_info,NULL);
 if(op)pa_operation_unref(op);else scanning=false;
}
static void scan_due(pa_mainloop_api *a,pa_time_event *event,const struct timeval *tv,void *data) {
 a->time_free(event);scan_event=NULL;
 if(pa_context_get_state(context)==PA_CONTEXT_READY)scan();
}
static void subscription(pa_context *c,pa_subscription_event_type_t type,uint32_t index,void *data) {
 if(!scan_event){struct timeval tv=after_ms(30);scan_event=api->time_new(api,&tv,scan_due,NULL);}
}
static void reconnect(pa_mainloop_api *a,pa_time_event *event,const struct timeval *tv,void *data);
static void state(pa_context *c,void *data) {
 pa_context_state_t s=pa_context_get_state(c);
 if(s==PA_CONTEXT_READY) {
  create_stream();pa_context_set_subscribe_callback(c,subscription,NULL);
  pa_operation *op=pa_context_subscribe(c,PA_SUBSCRIPTION_MASK_SINK_INPUT,NULL,NULL);
  if(op)pa_operation_unref(op);
  reported=-1;scan();
 } else if(s==PA_CONTEXT_FAILED || s==PA_CONTEXT_TERMINATED) {
  if(stream) {
   pa_stream_set_state_callback(stream,NULL,NULL);pa_stream_set_write_callback(stream,NULL,NULL);
   pa_stream_disconnect(stream);pa_stream_unref(stream);stream=NULL;
  }
  memset(voices,0,sizeof(voices));corked=true;
  pa_context_set_state_callback(c,NULL,NULL);scanning=again=false;playing=0;tell_audio();
  struct timeval tv=after_ms(2000);api->time_new(api,&tv,reconnect,NULL);
 }
}
static void connect_audio(void) {
 if(context)pa_context_unref(context);
 context=pa_context_new(api,"ztOS system sounds");
 pa_context_set_state_callback(context,state,NULL);
 pa_context_connect(context,NULL,PA_CONTEXT_NOAUTOSPAWN,NULL);
}
static void reconnect(pa_mainloop_api *a,pa_time_event *event,const struct timeval *tv,void *data) {
 a->time_free(event);connect_audio();
}
static void tap(pa_mainloop_api *a,pa_io_event *event,int socket_fd,pa_io_event_flags_t flags,void *data) {
 unsigned char packet[2];ssize_t received;
 // Keep the newest four pending taps if the socket accumulated a burst while
 // busy. Never replay an old backlog after the user has finished typing.
 unsigned char recent[VOICES][2];int count=0;
 while((received=recv(socket_fd,packet,sizeof(packet),0))>=0) {
  if(!received){reported=-1;if(pa_context_get_state(context)==PA_CONTEXT_READY)scan();continue;}
  if(received!=2 || packet[0]>=SOUNDS || packet[1]>100 || !packet[1] || !sounds[packet[0]].pcm)continue;
  memcpy(recent[count%VOICES],packet,2);count++;
 }
 if(!count || pa_context_get_state(context)!=PA_CONTEXT_READY)return;
 if(!stream)create_stream();
 if(!stream || pa_stream_get_state(stream)!=PA_STREAM_READY)return;
 int first=count>VOICES?count-VOICES:0;
 for(int i=first;i<count;i++)trigger_voice(recent[i%VOICES][0],recent[i%VOICES][1]);
 struct timeval due=after_ms(IDLE_MS);
 if(idle_event)api->time_restart(idle_event,&due);else idle_event=api->time_new(api,&due,idle,NULL);
 if(corked) {
  // Clear silence left from the previous idle period before waking.
  pa_operation *flush=pa_stream_flush(stream,NULL,NULL);
  if(flush)pa_operation_unref(flush);
  corked=false;pa_operation *op=pa_stream_cork(stream,0,NULL,NULL);
  if(op)pa_operation_unref(op);
 }
}
int main(void) {
 const char *runtime=getenv("XDG_RUNTIME_DIR");if(!runtime)return 1;
 snprintf(socket_path,sizeof(socket_path),"%s/ztos-sounds.sock",runtime);
 snprintf(ctl_path,sizeof(ctl_path),"%s/ztmetro.ctl",runtime);
 fd=socket(AF_UNIX,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);if(fd<0)return 1;
 struct sockaddr_un addr={.sun_family=AF_UNIX};snprintf(addr.sun_path,sizeof(addr.sun_path),"%s",socket_path);
 umask(0077);
 if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0) {
  if(errno!=EADDRINUSE || sendto(fd,"",0,0,(struct sockaddr*)&addr,sizeof(addr))>=0)return 0;
  unlink(socket_path);if(bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0)return 1;
 }
 for(int i=0;i<SOUNDS;i++)load_sound(i);
 loop=pa_mainloop_new();api=pa_mainloop_get_api(loop);
 api->io_new(api,fd,PA_IO_EVENT_INPUT,tap,NULL);connect_audio();
 int result;pa_mainloop_run(loop,&result);unlink(socket_path);return result;
}
