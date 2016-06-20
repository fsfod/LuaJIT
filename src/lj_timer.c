#pragma once

#include "lj_tab.h"
#include "lj_buf.h"
#include <stdio.h>
#include "lj_timer.h"

const char* gcstates[] = {
  "GCpause",
  "GCSpropagate",
  "GCSatomic",
  "GCSsweepstring",
  "GCSsweep",
  "GCSfinalize"
};

const char *getgcsname(int gcs)
{
  switch (gcs) {
  case GCSpause:
    return "GCSpause";
  case GCSpropagate:
    return "GCSpropagate";
  case GCSatomic:
    return "GCSatomic";
  case GCSsweepstring:
    return "GCSsweepstring";
  case GCSsweep:
    return "GCSsweep";
  case GCSfinalize:
    return "GCSfinalize";
  default:
    return NULL;
    break;
  }
}


#ifdef LJ_ENABLESTATS

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

uint32_t perf_counter[Counter_MAX] = { 0 };

void perf_resetcounters()
{
  memset(perf_counter, 0, sizeof(perf_counter));
}

int perf_getcounters(lua_State *L)
{
  GCtab *t = lj_tab_new(L, 0, Counter_MAX*2);
  settabV(L, L->top++, t);
  
  for (MSize i = 0; i < Counter_MAX; i++) {
    TValue *tv = lj_tab_setstr(L, t, lj_str_newz(L, Counter_names[i]));
    setintV(tv, (int32_t)perf_counter[i]);
  }
  
  return 1;
}

void perf_printcounters()
{
  int seenfirst = 0;
  
  for (MSize i = 0; i < Counter_MAX; i++) {
    if (perf_counter[i] == 0) continue;
    if (!seenfirst) {
      seenfirst = 1;
      printf("Perf Counters\n");
    }
    printf("  %s: %d\n", Counter_names[i], perf_counter[i]);
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

uint64_t secstart[Section_MAX] = { 0 };
uint64_t sectotal[Section_MAX] = { 0 };
uint64_t timertotal[Timer_MAX] = { 0 };
uint64_t laststatets = 0;
uint64_t statetime[GCSfinalize+1] = { 0 };

void printsectotals()
{
  for (MSize i = 0; i < Section_MAX; i++) {
    printf("Section %s total ", sections_names[i]);
    printtickms(sectotal[i]);
  }

  for (MSize i = 0; i < Timer_MAX; i++) {
    printf("Timer %s total ", timers_names[i]);
    printtickms(timertotal[i]);
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
        MSize gcs = gcstatemsg_state(msg);
        MSize prevgcs = gcstatemsg_prevstate(msg);
        printf("GC State = %s, mem = %ukb\n", getgcsname(gcs), msg->totalmem/1024);
        pos += msgsizes[MSGID_gcstate];

        uint64_t start = secstart[Section_gc_step] ? secstart[Section_gc_step] : secstart[Section_gc_fullgc];

        /* Check if we transitioned gc state inside a step and continued running the new gc state */
        if (laststatets != 0 && start != 0 && laststatets > start) {
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
      case MSGID_stringmarker: {
        MSG_stringmarker *msg = (MSG_stringmarker *)pos;
        printf("@%s - %llu\n", (const char*)(msg+1), msg->time);
        fprintf(dumpfile, "\n\n@%s - %llu", (const char*)(msg+1), msg->time);
        // printf("stringmarker: flags %u, size %u, label %s, time %ull\n", ((msg->msgid >> 8) & 0xffff), msg->size, (const char*)(msg+1), msg->time);
        pos += msg->size;
        break;
      }
      case MSGID_time: {
        MSG_time *msg = (MSG_time *)pos;
        uint32_t id = timemsg_id(msg);
        timertotal[id] += msg->time;
        pos += sizeof(MSG_time);
        break;
      }
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

#else

void timers_setuplog(lua_State *L)
{
  UNUSED(L);
}

void timers_freelog(global_State *g)
{
  UNUSED(g);
}
#endif
