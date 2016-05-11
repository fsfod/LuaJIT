#pragma once

#include "lj_buf.h"
#include <stdio.h>
#include "lj_timer.h"


#define deftimer(name)  
#define endtimer(name) 

SBuf eventbuf = { 0 };


void timers_setuplog(lua_State *L)
{
  if (mref(eventbuf.L, lua_State) == NULL) {
    lj_buf_init(L, &eventbuf);
    lj_buf_more(&eventbuf, 1 << 20);
  }
}

void timers_freelog(global_State *g)
{
  if (sbufB(&eventbuf)) {
    lj_buf_free(g, &eventbuf);
  }
}

static int64_t tscfrequency = 3400000000;

void timers_print(const char *name, uint64_t time)
{
  double t = ((double)time)/(tscfrequency/1000);
  printf("%s took %.4g ms(%d)\n", name, t, (uint32_t)time);
}

static const char* gcstates[] = {
  "GCpause",
  "GCSpropagate",
  "GCSatomic",
  "GCSsweepstring",
  "GCSsweep",
  "GCSfinalize"
};

void timers_printlog()
{
  char* pos = sbufB(&eventbuf);

  for (; pos < sbufP(&eventbuf); ) {
    uint32_t header = *(uint32_t*)pos;
    uint8_t id = (uint8_t)header;

    switch (id) {
      case MSGID_gcstate:
        printf("GC State = %s\n", gcstates[(uint8_t)(header >> 8)]);
        pos += msgsizes[MSGID_gcstate];
        break;
      default:
        pos += msgprinters[id](pos);
    }
  }
  lj_buf_reset(&eventbuf);
}
