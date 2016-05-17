#pragma once

#include "lj_buf.h"
#include <stdio.h>
#include "lj_timer.h"

SBuf eventbuf = { 0 };

void timers_setuplog(lua_State *L)
{
  if (mref(eventbuf.L, lua_State) == NULL) {
    MSize total = G(L)->gc.total;
    lj_buf_init(L, &eventbuf);
    lj_buf_more(&eventbuf, 1024*1024*100);
    /* Don't distort the gc rate by our buffer size */
    G(L)->gc.total = total;
  }
}

void timers_freelog(global_State *g)
{
  if (sbufB(&eventbuf)) {
    timers_printlog();
    g->gc.total += sbufsz(&eventbuf);
    lj_buf_free(g, &eventbuf);
  }
}

static int64_t tscfrequency = 3400000000;
static int64_t turbofrequency = 4800000000;

void printtickms(uint64_t time)
{
  double t = ((double)time)/(tscfrequency/1000);
  printf("took %.4g ms(%llu)\n", t, time);
}

void timers_print(const char *name, uint64_t time)
{
  double t = ((double)time)/(tscfrequency/1000);
  printf("took %.4g ms(%ull)", name, t, time);
}

static const char* gcstates[] = {
  "GCpause",
  "GCSpropagate",
  "GCSatomic",
  "GCSsweepstring",
  "GCSsweep",
  "GCSfinalize"
};

uint64_t secstart[Section_MAX] = { 0 };
uint64_t sectotal[Section_MAX] = { 0 };
uint64_t laststatets = 0;
uint64_t statetime[GCSfinalize+1] = { 0 };

void printsectotals()
{
  for (MSize i = 0; i < Section_MAX; i++) {
    printf("Section %s total ", sections_names[i]);
    printtickms(sectotal[i]);
  }

  for (MSize i = 0; i < 6; i++) {
    printf("GC state %s total ", gcstates[i]);
    printtickms(statetime[i]);
  }
}

void timers_printlog()
{
  char* pos = sbufB(&eventbuf);
  uint64_t steptime = 0;/* Acculated step time for current GC state */
  uint64_t laststatets = 0; 
  FILE* dumpfile = fopen("gcstats.csv", "a+");

  for (; pos < sbufP(&eventbuf); ) {
    uint32_t header = *(uint32_t*)pos;
    uint8_t id = (uint8_t)header;

    switch (id) {
      case MSGID_gcstate:{
        MSG_gcstate *msg = (MSG_gcstate *)pos;
        MSize gcs = (uint8_t)(header >> 8);
        MSize prevgcs = (uint8_t)(header >> 16);
        printf("GC State = %s, mem = %ukb\n", gcstates[gcs], msg->totalmem/1024);
        pos += msgsizes[MSGID_gcstate];

        /* Check if we transitioned gc state inside a step and continued running the new gc state */
        if (laststatets != 0 && secstart[Section_gc_step] != 0 && laststatets > secstart[Section_gc_step]) {
          steptime = msg->time-laststatets;
        }
        statetime[prevgcs] += steptime;
        if (prevgcs != GCSpause) {
          fprintf(dumpfile, "%s%llu", prevgcs != GCSpropagate ? ", " : "\n", steptime);
        }
        steptime = 0;
        laststatets = msg->time;
      }break;

      case MSGID_section: {
        MSG_section *msg = (MSG_section *)pos;
        MSize secid = ((msg->msgid >> 8) & 0x7fffff);

        if ((msg->msgid >> 31)) {
          if(secid != Section_gc_step)printf("Start(%s)\n", sections_names[secid]);
          secstart[secid] = msg->time;
        } else {
          uint64_t time = msg->time-secstart[secid];
          lua_assert(secstart[secid] > 0 && secstart[secid] < msg->time);

          if (secid != Section_gc_step) {
            printf("End(%s) ", sections_names[secid]);
            printtickms(time);
          } else {
            /* If a new GC state started part the way through the step only record the
            ** step time from when it started to the step end
            */
            if (laststatets > secstart[Section_gc_step]) {
              steptime += msg->time-laststatets;
            } else {
              steptime += time;
            }
          }
          sectotal[secid] += time;
          secstart[secid] = 0;
        }
        pos += sizeof(MSG_section);
      }break;

      default:
        pos += msgprinters[id](pos);
    }
  }
  printsectotals();
  fflush(dumpfile);
  fclose(dumpfile);
  lj_buf_reset(&eventbuf);
}

void recgcstate(global_State *g, int newstate)
{
  lua_State *L = mainthread(g);
  log_gcstate(newstate, g->gc.state, g->gc.total);
}
