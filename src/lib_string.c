/*
** String library.
** Copyright (C) 2005-2015 Mike Pall. See Copyright Notice in luajit.h
**
** Major portions taken verbatim or adapted from the Lua interpreter.
** Copyright (C) 1994-2008 Lua.org, PUC-Rio. See Copyright Notice in lua.h
*/

#define lib_string_c
#define LUA_LIB

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_meta.h"
#include "lj_state.h"
#include "lj_ff.h"
#include "lj_bcdump.h"
#include "lj_char.h"
#include "lj_strfmt.h"
#include "lj_lib.h"

#define udstrbufV(o) ((SBuf *)uddata(udataV(o)))

static SBuf *check_bufarg(lua_State *L)
{
  if (!(L->base < L->top && tvisstrbuf(L->base)))
    lj_err_argtype(L, 1, "string buffer");

  return udstrbufV(L->base);
}

SBuf *stringbuf_putobj_default(SBuf *sb, cTValue *o)
{
  char *p;
  lj_buf_putmem(sb, lj_typename(o), (MSize)strlen(lj_typename(o)));
  p = lj_buf_more(sb, 8 + 2 + 2 + 16);
  *p++ = ':'; *p++ = ' ';
  if (tvisfunc(o) && isffunc(funcV(o))) {
    p = lj_buf_wmem(p, "builtin#", 8);
    p = lj_strfmt_wint(p, funcV(o)->c.ffid);
  } else {
    p = lj_strfmt_wptr(p, lj_obj_ptr(o));
  }
  setsbufP(sb, p);
  return sb;
}

cTValue *stringbuf_tryputobj(SBuf *sb, cTValue *o, int checkmt)
{
  if (tvisstr(o)) {
    lj_buf_putstr(sb, strV(o));
  } else if (tvisnumber(o)) {
    if (tvisint(o))
      lj_strfmt_putint(sb, intV(o));
    else
      setsbufP(sb, lj_strfmt_wnum(lj_buf_more(sb, STRFMT_MAXBUF_NUM), o));
  } else if (tvisnil(o)) {
    lj_buf_putmem(sb, "nil", 3);
  } else if (tvisfalse(o)) {
    lj_buf_putmem(sb, "false", 5);
  } else if (tvistrue(o)) {
    lj_buf_putmem(sb, "true", 4);
  } else if(!checkmt){
    stringbuf_putobj_default(sb, o);
  } else {
    cTValue *mo = lj_meta_lookup(sbufL(sb), o, MM_tostring);

    if (tvisnil(mo)) {
      stringbuf_putobj_default(sb, o);
      return NULL;
    } else {
      return mo;
    }
  }

  return NULL;
}

static TValue *stringbuf_putobj_hastostring(SBuf *sb, cTValue *mo, int arg)
{
  lua_State *L = sbufL(sb);
  TValue *o = L->base + arg - 1;
  lua_assert(o < L->top);  /* Caller already checks for existence. */

  copyTV(L, L->top++, mo);
  copyTV(L, L->top++, o);
  lua_call(L, 1, 1);
  /* Stack may of been reallocated */
  o = L->base + arg - 1;
  copyTV(L, o, --L->top);
  
  return  o;
}


/* ------------------------------------------------------------------------ */

#define LJLIB_MODULE_string

LJLIB_LUA(string_len) /*
  function(s)
    CHECK_str(s)
    return #s
  end
*/

LJLIB_ASM(string_byte)		LJLIB_REC(string_range 0)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  int32_t len = (int32_t)s->len;
  int32_t start = lj_lib_optint(L, 2, 1);
  int32_t stop = lj_lib_optint(L, 3, start);
  int32_t n, i;
  const unsigned char *p;
  if (stop < 0) stop += len+1;
  if (start < 0) start += len+1;
  if (start <= 0) start = 1;
  if (stop > len) stop = len;
  if (start > stop) return FFH_RES(0);  /* Empty interval: return no results. */
  start--;
  n = stop - start;
  if ((uint32_t)n > LUAI_MAXCSTACK)
    lj_err_caller(L, LJ_ERR_STRSLC);
  lj_state_checkstack(L, (MSize)n);
  p = (const unsigned char *)strdata(s) + start;
  for (i = 0; i < n; i++)
    setintV(L->base + i-1-LJ_FR2, p[i]);
  return FFH_RES(n);
}

LJLIB_ASM(string_char)		LJLIB_REC(.)
{
  int i, nargs = (int)(L->top - L->base);
  char *buf = lj_buf_tmp(L, (MSize)nargs);
  for (i = 1; i <= nargs; i++) {
    int32_t k = lj_lib_checkint(L, i);
    if (!checku8(k))
      lj_err_arg(L, i, LJ_ERR_BADVAL);
    buf[i-1] = (char)k;
  }
  setstrV(L, L->base-1-LJ_FR2, lj_str_new(L, buf, (size_t)nargs));
  return FFH_RES(1);
}

LJLIB_ASM(string_sub)		LJLIB_REC(string_range 1)
{
  lj_lib_checkstr(L, 1);
  lj_lib_checkint(L, 2);
  setintV(L->base+2, lj_lib_optint(L, 3, -1));
  return FFH_RETRY;
}

static int string_rep(lua_State *L, int isstrbuf)
{
  int base = isstrbuf ? 1 : 0;
  SBuf *sb = isstrbuf ? check_bufarg(L) : NULL;
  GCstr *s = lj_lib_checkstr(L, base + 1);
  int32_t rep = lj_lib_checkint(L, base + 2);
  GCstr *sep = lj_lib_optstr(L, base + 3);
  
  if (!isstrbuf)
    sb = lj_buf_tmp_(L);

  if (sep && rep > 1) {
    GCstr *s2 = lj_buf_cat2str(L, sep, s);
    if (!isstrbuf)
      lj_buf_reset(sb);
    lj_buf_putstr(sb, s);
    s = s2;
    rep--;
  }
  sb = lj_buf_putstr_rep(sb, s, rep);
  if (isstrbuf) {
    return 0;
  }
  setstrV(L, L->top - 1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(string_rep)		LJLIB_REC(.)
{
  return string_rep(L, 0);
}

LJLIB_ASM(string_reverse)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_reverse)
{
  lj_lib_checkstr(L, 1);
  return FFH_RETRY;
}
LJLIB_ASM_(string_lower)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_lower)
LJLIB_ASM_(string_upper)  LJLIB_REC(string_op IRCALL_lj_buf_putstr_upper)

/* ------------------------------------------------------------------------ */

static int writer_buf(lua_State *L, const void *p, size_t size, void *sb)
{
  lj_buf_putmem((SBuf *)sb, p, (MSize)size);
  UNUSED(L);
  return 0;
}

LJLIB_CF(string_dump)
{
  GCfunc *fn = lj_lib_checkfunc(L, 1);
  int strip = L->base+1 < L->top && tvistruecond(L->base+1);
  SBuf *sb = lj_buf_tmp_(L);  /* Assumes lj_bcwrite() doesn't use tmpbuf. */
  L->top = L->base+1;
  if (!isluafunc(fn) || lj_bcwrite(L, funcproto(fn), writer_buf, sb, strip))
    lj_err_caller(L, LJ_ERR_STRDUMP);
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

/* ------------------------------------------------------------------------ */

/* macro to `unsign' a character */
#define uchar(c)        ((unsigned char)(c))

#define CAP_UNFINISHED	(-1)
#define CAP_POSITION	(-2)

typedef struct MatchState {
  const char *src_init;  /* init of source string */
  const char *src_end;  /* end (`\0') of source string */
  lua_State *L;
  int level;  /* total number of captures (finished or unfinished) */
  int depth;
  struct {
    const char *init;
    ptrdiff_t len;
  } capture[LUA_MAXCAPTURES];
} MatchState;

#define L_ESC		'%'

static int check_capture(MatchState *ms, int l)
{
  l -= '1';
  if (l < 0 || l >= ms->level || ms->capture[l].len == CAP_UNFINISHED)
    lj_err_caller(ms->L, LJ_ERR_STRCAPI);
  return l;
}

static int capture_to_close(MatchState *ms)
{
  int level = ms->level;
  for (level--; level>=0; level--)
    if (ms->capture[level].len == CAP_UNFINISHED) return level;
  lj_err_caller(ms->L, LJ_ERR_STRPATC);
  return 0;  /* unreachable */
}

static const char *classend(MatchState *ms, const char *p)
{
  switch (*p++) {
  case L_ESC:
    if (*p == '\0')
      lj_err_caller(ms->L, LJ_ERR_STRPATE);
    return p+1;
  case '[':
    if (*p == '^') p++;
    do {  /* look for a `]' */
      if (*p == '\0')
	lj_err_caller(ms->L, LJ_ERR_STRPATM);
      if (*(p++) == L_ESC && *p != '\0')
	p++;  /* skip escapes (e.g. `%]') */
    } while (*p != ']');
    return p+1;
  default:
    return p;
  }
}

static const unsigned char match_class_map[32] = {
  0,LJ_CHAR_ALPHA,0,LJ_CHAR_CNTRL,LJ_CHAR_DIGIT,0,0,LJ_CHAR_GRAPH,0,0,0,0,
  LJ_CHAR_LOWER,0,0,0,LJ_CHAR_PUNCT,0,0,LJ_CHAR_SPACE,0,
  LJ_CHAR_UPPER,0,LJ_CHAR_ALNUM,LJ_CHAR_XDIGIT,0,0,0,0,0,0,0
};

static int match_class(int c, int cl)
{
  if ((cl & 0xc0) == 0x40) {
    int t = match_class_map[(cl&0x1f)];
    if (t) {
      t = lj_char_isa(c, t);
      return (cl & 0x20) ? t : !t;
    }
    if (cl == 'z') return c == 0;
    if (cl == 'Z') return c != 0;
  }
  return (cl == c);
}

static int matchbracketclass(int c, const char *p, const char *ec)
{
  int sig = 1;
  if (*(p+1) == '^') {
    sig = 0;
    p++;  /* skip the `^' */
  }
  while (++p < ec) {
    if (*p == L_ESC) {
      p++;
      if (match_class(c, uchar(*p)))
	return sig;
    }
    else if ((*(p+1) == '-') && (p+2 < ec)) {
      p+=2;
      if (uchar(*(p-2)) <= c && c <= uchar(*p))
	return sig;
    }
    else if (uchar(*p) == c) return sig;
  }
  return !sig;
}

static int singlematch(int c, const char *p, const char *ep)
{
  switch (*p) {
  case '.': return 1;  /* matches any char */
  case L_ESC: return match_class(c, uchar(*(p+1)));
  case '[': return matchbracketclass(c, p, ep-1);
  default:  return (uchar(*p) == c);
  }
}

static const char *match(MatchState *ms, const char *s, const char *p);

static const char *matchbalance(MatchState *ms, const char *s, const char *p)
{
  if (*p == 0 || *(p+1) == 0)
    lj_err_caller(ms->L, LJ_ERR_STRPATU);
  if (*s != *p) {
    return NULL;
  } else {
    int b = *p;
    int e = *(p+1);
    int cont = 1;
    while (++s < ms->src_end) {
      if (*s == e) {
	if (--cont == 0) return s+1;
      } else if (*s == b) {
	cont++;
      }
    }
  }
  return NULL;  /* string ends out of balance */
}

static const char *max_expand(MatchState *ms, const char *s,
			      const char *p, const char *ep)
{
  ptrdiff_t i = 0;  /* counts maximum expand for item */
  while ((s+i)<ms->src_end && singlematch(uchar(*(s+i)), p, ep))
    i++;
  /* keeps trying to match with the maximum repetitions */
  while (i>=0) {
    const char *res = match(ms, (s+i), ep+1);
    if (res) return res;
    i--;  /* else didn't match; reduce 1 repetition to try again */
  }
  return NULL;
}

static const char *min_expand(MatchState *ms, const char *s,
			      const char *p, const char *ep)
{
  for (;;) {
    const char *res = match(ms, s, ep+1);
    if (res != NULL)
      return res;
    else if (s<ms->src_end && singlematch(uchar(*s), p, ep))
      s++;  /* try with one more repetition */
    else
      return NULL;
  }
}

static const char *start_capture(MatchState *ms, const char *s,
				 const char *p, int what)
{
  const char *res;
  int level = ms->level;
  if (level >= LUA_MAXCAPTURES) lj_err_caller(ms->L, LJ_ERR_STRCAPN);
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = level+1;
  if ((res=match(ms, s, p)) == NULL)  /* match failed? */
    ms->level--;  /* undo capture */
  return res;
}

static const char *end_capture(MatchState *ms, const char *s,
			       const char *p)
{
  int l = capture_to_close(ms);
  const char *res;
  ms->capture[l].len = s - ms->capture[l].init;  /* close capture */
  if ((res = match(ms, s, p)) == NULL)  /* match failed? */
    ms->capture[l].len = CAP_UNFINISHED;  /* undo capture */
  return res;
}

static const char *match_capture(MatchState *ms, const char *s, int l)
{
  size_t len;
  l = check_capture(ms, l);
  len = (size_t)ms->capture[l].len;
  if ((size_t)(ms->src_end-s) >= len &&
      memcmp(ms->capture[l].init, s, len) == 0)
    return s+len;
  else
    return NULL;
}

static const char *match(MatchState *ms, const char *s, const char *p)
{
  if (++ms->depth > LJ_MAX_XLEVEL)
    lj_err_caller(ms->L, LJ_ERR_STRPATX);
  init: /* using goto's to optimize tail recursion */
  switch (*p) {
  case '(':  /* start capture */
    if (*(p+1) == ')')  /* position capture? */
      s = start_capture(ms, s, p+2, CAP_POSITION);
    else
      s = start_capture(ms, s, p+1, CAP_UNFINISHED);
    break;
  case ')':  /* end capture */
    s = end_capture(ms, s, p+1);
    break;
  case L_ESC:
    switch (*(p+1)) {
    case 'b':  /* balanced string? */
      s = matchbalance(ms, s, p+2);
      if (s == NULL) break;
      p+=4;
      goto init;  /* else s = match(ms, s, p+4); */
    case 'f': {  /* frontier? */
      const char *ep; char previous;
      p += 2;
      if (*p != '[')
	lj_err_caller(ms->L, LJ_ERR_STRPATB);
      ep = classend(ms, p);  /* points to what is next */
      previous = (s == ms->src_init) ? '\0' : *(s-1);
      if (matchbracketclass(uchar(previous), p, ep-1) ||
	 !matchbracketclass(uchar(*s), p, ep-1)) { s = NULL; break; }
      p=ep;
      goto init;  /* else s = match(ms, s, ep); */
      }
    default:
      if (lj_char_isdigit(uchar(*(p+1)))) {  /* capture results (%0-%9)? */
	s = match_capture(ms, s, uchar(*(p+1)));
	if (s == NULL) break;
	p+=2;
	goto init;  /* else s = match(ms, s, p+2) */
      }
      goto dflt;  /* case default */
    }
    break;
  case '\0':  /* end of pattern */
    break;  /* match succeeded */
  case '$':
    /* is the `$' the last char in pattern? */
    if (*(p+1) != '\0') goto dflt;
    if (s != ms->src_end) s = NULL;  /* check end of string */
    break;
  default: dflt: {  /* it is a pattern item */
    const char *ep = classend(ms, p);  /* points to what is next */
    int m = s<ms->src_end && singlematch(uchar(*s), p, ep);
    switch (*ep) {
    case '?': {  /* optional */
      const char *res;
      if (m && ((res=match(ms, s+1, ep+1)) != NULL)) {
	s = res;
	break;
      }
      p=ep+1;
      goto init;  /* else s = match(ms, s, ep+1); */
      }
    case '*':  /* 0 or more repetitions */
      s = max_expand(ms, s, p, ep);
      break;
    case '+':  /* 1 or more repetitions */
      s = (m ? max_expand(ms, s+1, p, ep) : NULL);
      break;
    case '-':  /* 0 or more repetitions (minimum) */
      s = min_expand(ms, s, p, ep);
      break;
    default:
      if (m) { s++; p=ep; goto init; }  /* else s = match(ms, s+1, ep); */
      s = NULL;
      break;
    }
    break;
    }
  }
  ms->depth--;
  return s;
}

static void push_onecapture(MatchState *ms, int i, const char *s, const char *e)
{
  if (i >= ms->level) {
    if (i == 0)  /* ms->level == 0, too */
      lua_pushlstring(ms->L, s, (size_t)(e - s));  /* add whole match */
    else
      lj_err_caller(ms->L, LJ_ERR_STRCAPI);
  } else {
    ptrdiff_t l = ms->capture[i].len;
    if (l == CAP_UNFINISHED) lj_err_caller(ms->L, LJ_ERR_STRCAPU);
    if (l == CAP_POSITION)
      lua_pushinteger(ms->L, ms->capture[i].init - ms->src_init + 1);
    else
      lua_pushlstring(ms->L, ms->capture[i].init, (size_t)l);
  }
}

static int push_captures(MatchState *ms, const char *s, const char *e)
{
  int i;
  int nlevels = (ms->level == 0 && s) ? 1 : ms->level;
  luaL_checkstack(ms->L, nlevels, "too many captures");
  for (i = 0; i < nlevels; i++)
    push_onecapture(ms, i, s, e);
  return nlevels;  /* number of strings pushed */
}

static int str_find_aux(lua_State *L, int find)
{
  GCstr *s = lj_lib_checkstr(L, 1);
  GCstr *p = lj_lib_checkstr(L, 2);
  int32_t start = lj_lib_optint(L, 3, 1);
  MSize st;
  if (start < 0) start += (int32_t)s->len; else start--;
  if (start < 0) start = 0;
  st = (MSize)start;
  if (st > s->len) {
#if LJ_52
    setnilV(L->top-1);
    return 1;
#else
    st = s->len;
#endif
  }
  if (find && ((L->base+3 < L->top && tvistruecond(L->base+3)) ||
	       !lj_str_haspattern(p))) {  /* Search for fixed string. */
    const char *q = lj_str_find(strdata(s)+st, strdata(p), s->len-st, p->len);
    if (q) {
      setintV(L->top-2, (int32_t)(q-strdata(s)) + 1);
      setintV(L->top-1, (int32_t)(q-strdata(s)) + (int32_t)p->len);
      return 2;
    }
  } else {  /* Search for pattern. */
    MatchState ms;
    const char *pstr = strdata(p);
    const char *sstr = strdata(s) + st;
    int anchor = 0;
    if (*pstr == '^') { pstr++; anchor = 1; }
    ms.L = L;
    ms.src_init = strdata(s);
    ms.src_end = strdata(s) + s->len;
    do {  /* Loop through string and try to match the pattern. */
      const char *q;
      ms.level = ms.depth = 0;
      q = match(&ms, sstr, pstr);
      if (q) {
	if (find) {
	  setintV(L->top++, (int32_t)(sstr-(strdata(s)-1)));
	  setintV(L->top++, (int32_t)(q-strdata(s)));
	  return push_captures(&ms, NULL, NULL) + 2;
	} else {
	  return push_captures(&ms, sstr, q);
	}
      }
    } while (sstr++ < ms.src_end && !anchor);
  }
  setnilV(L->top-1);  /* Not found. */
  return 1;
}

LJLIB_CF(string_find)		LJLIB_REC(.)
{
  return str_find_aux(L, 1);
}

LJLIB_CF(string_match)
{
  return str_find_aux(L, 0);
}

LJLIB_NOREG LJLIB_CF(string_gmatch_aux)
{
  const char *p = strVdata(lj_lib_upvalue(L, 2));
  GCstr *str = strV(lj_lib_upvalue(L, 1));
  const char *s = strdata(str);
  TValue *tvpos = lj_lib_upvalue(L, 3);
  const char *src = s + tvpos->u32.lo;
  MatchState ms;
  ms.L = L;
  ms.src_init = s;
  ms.src_end = s + str->len;
  for (; src <= ms.src_end; src++) {
    const char *e;
    ms.level = ms.depth = 0;
    if ((e = match(&ms, src, p)) != NULL) {
      int32_t pos = (int32_t)(e - s);
      if (e == src) pos++;  /* Ensure progress for empty match. */
      tvpos->u32.lo = (uint32_t)pos;
      return push_captures(&ms, src, e);
    }
  }
  return 0;  /* not found */
}

LJLIB_CF(string_gmatch)
{
  lj_lib_checkstr(L, 1);
  lj_lib_checkstr(L, 2);
  L->top = L->base+3;
  (L->top-1)->u64 = 0;
  lj_lib_pushcc(L, lj_cf_string_gmatch_aux, FF_string_gmatch_aux, 3);
  return 1;
}

static void add_s(MatchState *ms, luaL_Buffer *b, const char *s, const char *e)
{
  size_t l, i;
  const char *news = lua_tolstring(ms->L, 3, &l);
  for (i = 0; i < l; i++) {
    if (news[i] != L_ESC) {
      luaL_addchar(b, news[i]);
    } else {
      i++;  /* skip ESC */
      if (!lj_char_isdigit(uchar(news[i]))) {
	luaL_addchar(b, news[i]);
      } else if (news[i] == '0') {
	luaL_addlstring(b, s, (size_t)(e - s));
      } else {
	push_onecapture(ms, news[i] - '1', s, e);
	luaL_addvalue(b);  /* add capture to accumulated result */
      }
    }
  }
}

static void add_value(MatchState *ms, luaL_Buffer *b,
		      const char *s, const char *e)
{
  lua_State *L = ms->L;
  switch (lua_type(L, 3)) {
    case LUA_TNUMBER:
    case LUA_TSTRING: {
      add_s(ms, b, s, e);
      return;
    }
    case LUA_TFUNCTION: {
      int n;
      lua_pushvalue(L, 3);
      n = push_captures(ms, s, e);
      lua_call(L, n, 1);
      break;
    }
    case LUA_TTABLE: {
      push_onecapture(ms, 0, s, e);
      lua_gettable(L, 3);
      break;
    }
  }
  if (!lua_toboolean(L, -1)) {  /* nil or false? */
    lua_pop(L, 1);
    lua_pushlstring(L, s, (size_t)(e - s));  /* keep original text */
  } else if (!lua_isstring(L, -1)) {
    lj_err_callerv(L, LJ_ERR_STRGSRV, luaL_typename(L, -1));
  }
  luaL_addvalue(b);  /* add result to accumulator */
}

LJLIB_CF(string_gsub)
{
  size_t srcl;
  const char *src = luaL_checklstring(L, 1, &srcl);
  const char *p = luaL_checkstring(L, 2);
  int  tr = lua_type(L, 3);
  int max_s = luaL_optint(L, 4, (int)(srcl+1));
  int anchor = (*p == '^') ? (p++, 1) : 0;
  int n = 0;
  MatchState ms;
  luaL_Buffer b;
  if (!(tr == LUA_TNUMBER || tr == LUA_TSTRING ||
	tr == LUA_TFUNCTION || tr == LUA_TTABLE))
    lj_err_arg(L, 3, LJ_ERR_NOSFT);
  luaL_buffinit(L, &b);
  ms.L = L;
  ms.src_init = src;
  ms.src_end = src+srcl;
  while (n < max_s) {
    const char *e;
    ms.level = ms.depth = 0;
    e = match(&ms, src, p);
    if (e) {
      n++;
      add_value(&ms, &b, src, e);
    }
    if (e && e>src) /* non empty match? */
      src = e;  /* skip it */
    else if (src < ms.src_end)
      luaL_addchar(&b, *src++);
    else
      break;
    if (anchor)
      break;
  }
  luaL_addlstring(&b, src, (size_t)(ms.src_end-src));
  luaL_pushresult(&b);
  lua_pushinteger(L, n);  /* number of substitutions */
  return 2;
}

/* ------------------------------------------------------------------------ */

/* Emulate tostring() inline. */
static GCstr *string_fmt_tostring(lua_State *L, int arg, int retry)
{
  TValue *o = L->base+arg-1;
  cTValue *mo;
  lua_assert(o < L->top);  /* Caller already checks for existence. */
  if (LJ_LIKELY(tvisstr(o)))
    return strV(o);
  if (retry != 2 && !tvisnil(mo = lj_meta_lookup(L, o, MM_tostring))) {
    copyTV(L, L->top++, mo);
    copyTV(L, L->top++, o);
    lua_call(L, 1, 1);
    copyTV(L, L->base+arg-1, --L->top);
    o = L->base + arg - 1;
    
    if (retry != -1)
      return NULL;  /* Buffer may be overwritten, retry. */

    /* Caller is not using the temp buffer so we can keep going */
    if (tvisstr(o))
      return strV(o);
  }

  return lj_strfmt_obj(L, o);
}

static int string_format(lua_State *L, int isstrbuf)
{
  int arg, top = (int)(L->top - L->base);
  GCstr *fmt;
  SBuf *sb;
  MSize savedbufpos;
  FormatState fs;
  SFormat sf;
  int retry = 0;
again:
  if (isstrbuf) {
    arg = 2;
    sb = check_bufarg(L);
    savedbufpos = sbuflen(sb); /* save the position so we can reset to it if theres an error */  
    retry = -1;/* were not writing to temp buffer so can ignore it being used by tostring metacalls */
  } else {
    arg = 1;
    sb = lj_buf_tmp_(L);
  } 
  fmt = lj_lib_checkstr(L, arg);
  lj_strfmt_init(&fs, strdata(fmt), fmt->len);
  while ((sf = lj_strfmt_parse(&fs)) != STRFMT_EOF) {
    if (sf == STRFMT_LIT) {
      lj_buf_putmem(sb, fs.str, fs.len);
    } else if (sf == STRFMT_ERR) {
      if (isstrbuf)
        setsbufofs(sb, savedbufpos);
      lj_err_callerv(L, LJ_ERR_STRFMT, strdata(lj_str_new(L, fs.str, fs.len)));
    } else {
      if (++arg > top) {
        if (isstrbuf)
          setsbufofs(sb, savedbufpos);
        luaL_argerror(L, arg, lj_obj_typename[0]);
      }
      switch (STRFMT_TYPE(sf)) {
      case STRFMT_INT:
	if (tvisint(L->base+arg-1)) {
	  int32_t k = intV(L->base+arg-1);
	  if (sf == STRFMT_INT)
	    lj_strfmt_putint(sb, k);  /* Shortcut for plain %d. */
	  else
	    lj_strfmt_putfxint(sb, sf, k);
	} else {
	  lj_strfmt_putfnum_int(sb, sf, lj_lib_checknum(L, arg));
	}
	break;
      case STRFMT_UINT:
	if (tvisint(L->base+arg-1))
	  lj_strfmt_putfxint(sb, sf, intV(L->base+arg-1));
	else
	  lj_strfmt_putfnum_uint(sb, sf, lj_lib_checknum(L, arg));
	break;
      case STRFMT_NUM:
	lj_strfmt_putfnum(sb, sf, lj_lib_checknum(L, arg));
	break;
      case STRFMT_STR: {
        const char *s;
        MSize len;

        if (!tvisstrbuf(L->base + arg - 1)) {
          GCstr *str;

          if (isstrbuf) {
            MSize bufpos = sbuflen(sb);
            /* tostring metamethod could throw so restore our buffer its original length so theres not our half written string */
            setsbufofs(sb, savedbufpos);
            str = string_fmt_tostring(L, arg, retry);
            setsbufofs(sb, bufpos);

          } else {
            str = string_fmt_tostring(L, arg, retry);
            if (str == NULL) {
              retry = 1;
              break;
            }
          }
          s = strdata(str);
          len = str->len;
        } else {
          SBuf *sbsrc = udstrbufV(L->base + arg - 1);
          len = sbuflen(sbsrc);
          lj_buf_nullterm(sbsrc); /* add fake null terminator since lj_strfmt_putquoted relies on it */
          s = sbufB(sbsrc);
        }

	if ((sf & STRFMT_T_QUOTED))
	  lj_strfmt_putquoted(sb, s, len);  /* No formatting. */
	else
	  lj_strfmt_putf(sb, sf, s, len);
	break;
	}
      case STRFMT_CHAR:
	lj_strfmt_putfchar(sb, sf, lj_lib_checkint(L, arg));
	break;
      case STRFMT_PTR:  /* No formatting. */
	lj_strfmt_putptr(sb, lj_obj_ptr(L->base+arg-1));
	break;
      default:
	lua_assert(0);
	break;
      }
    }
  }

  if (isstrbuf) 
    return 0;
  
  if (retry++ == 1) goto again;
  setstrV(L, L->top-1, lj_buf_str(L, sb));
  lj_gc_check(L);
  return 1;
}

LJLIB_CF(string_format)	LJLIB_REC(string_format 0)
{
  return string_format(L, 0);
}

LJLIB_PUSH(top-2) LJLIB_SET(!)  /* Set environment. */

LJLIB_CF(string_createbuffer)
{
  SBuf *sb = (SBuf *)lua_newuserdata(L, sizeof(SBuf));
  GCudata *ud = udataV(L->top - 1);
  ud->udtype = UDTYPE_STRING_BUF;
  /* NOBARRIER: The GCudata is new (marked white). */
  setgcrefr(ud->metatable, curr_func(L)->c.env);
  
  lj_buf_init(L, sb);

  return 1;
}

#include "lj_libdef.h"

/* -- string buffer methods ---------------------------------------------------- */

#define LJLIB_MODULE_stringbuf

LJLIB_CF(stringbuf_format) LJLIB_REC(string_format 1)
{
  return string_format(L, 1);
}

static SBuf *stringbuf_write(lua_State *L)
{
  SBuf *sb = check_bufarg(L);
  MSize savedpos = sbuflen(sb);
  cTValue *tv;

  for (tv = L->base + 1; tv < L->top; tv++) {
    cTValue *mo;

    mo = stringbuf_tryputobj(sb, tv, 1);

    if (mo) {
      MSize curpos = sbuflen(sb);
      setsbufofs(sb, savedpos);
      /* call could invalidate our stack pointer so switch to the one returned to us */
      tv = stringbuf_putobj_hastostring(sb, mo, (tv - L->base) + 1);
      setsbufofs(sb, curpos);
      stringbuf_tryputobj(sb, tv, 0);
    }
  }

  return sb;
}

LJLIB_CF(stringbuf_write) LJLIB_REC(stringbuf_write 0)
{
  stringbuf_write(L);
  return 0;
}

LJLIB_CF(stringbuf_writeln) LJLIB_REC(stringbuf_write 1)
{
  SBuf *sb = stringbuf_write(L);
  lj_buf_putb(sb, '\n');
  return 0;
}

LJLIB_CF(stringbuf_writesub) LJLIB_REC(stringbuf_writerange)
{
  MSize len;
  SBuf *sb = check_bufarg(L);
  const char *s = lj_lib_checkstrorbuf(L, 2, &len);
  int32_t start = lj_lib_checkint(L, 3);
  int32_t end = lj_lib_optint(L, 4, -1);

  lj_buf_putrang(sb, s, len, start, end);

  return 0;
}

LJLIB_CF(stringbuf_rep)
{
  return string_rep(L, 1);
}

LJLIB_CF(stringbuf_byte)
{
  SBuf *sb = check_bufarg(L);
  int32_t index = lj_lib_checkint(L, 2);

  if (index < 0) {
    index = sbuflen(sb)-index;
  }else{
    if ((MSize)index > sbuflen(sb))
      lj_err_arg(L, 2, LJ_ERR_BADVAL);
    index--;
  }

  setintV(L->top++, *(sbufB(sb) + index));
  
  return 1;
}

LJLIB_CF(stringbuf_getcapacity)
{
  SBuf *sb = check_bufarg(L);
  setintV(L->top - 1, sbufsz(sb));
  return 1;
}

LJLIB_CF(stringbuf_setcapacity)
{
  SBuf *sb = check_bufarg(L);
  int capacity = lj_lib_checkint(L, 2);

  if (capacity < 0) {
    lj_err_arg(L, 1, LJ_ERR_BADVAL);
  }

  if (capacity == 0) {
    lj_buf_free(G(L), sb);
    lj_buf_init(L, sb);
  }

  if ((MSize)capacity > sbufsz(sb)) {
    lj_buf_need(sb, capacity);
  } else {
    /* TODO: shrink*/
  }

  return 0;
}

LJLIB_CF(stringbuf_equals)
{
  MSize len;
  SBuf *sb = check_bufarg(L);
  const char* s = lj_lib_checkstrorbuf(L, 2, &len);

  int eq = len == sbuflen(sb) && strncmp(s, sbufB(sb), len) == 0;
  setboolV(L->top - 1, eq);

  return 1;
}

LJLIB_CF(stringbuf_clear) LJLIB_REC(.)
{
  SBuf *sb = check_bufarg(L);
  lj_buf_reset(sb);
  return 0;
}

LJLIB_CF(stringbuf_tostring) LJLIB_REC(.)
{
  SBuf *sb = check_bufarg(L);
  setstrV(L, L->top - 1, lj_buf_str(L, sb));
  return 1;
}

LJLIB_CF(stringbuf___tostring) LJLIB_REC(stringbuf_tostring)
{
  SBuf *sb = check_bufarg(L);
  setstrV(L, L->top - 1, lj_buf_str(L, sb));
  return 1;
}

LJLIB_CF(stringbuf___len)
{
  SBuf *sb = check_bufarg(L);
  setintptrV(L->top - 1, sbuflen(sb));
  return 1;
}

LJLIB_CF(stringbuf___gc)
{
  SBuf *sb = check_bufarg(L);
  lj_buf_free(G(L), sb);
  /* make sure */
  setmref(sb->p, NULL); setmref(sb->e, NULL); setmref(sb->b, NULL);

  return 0;
}



LJLIB_PUSH(top-1) LJLIB_SET(__index)

/* ------------------------------------------------------------------------ */

#include "lj_libdef.h"


LUALIB_API int luaopen_string(lua_State *L)
{
  GCtab *mt;
  global_State *g;
  LJ_LIB_REG(L, NULL, stringbuf);
  copyTV(L, L->top, L->top - 1); L->top++;
  lua_setfield(L, LUA_REGISTRYINDEX, "stringbuffer");

  LJ_LIB_REG(L, LUA_STRLIBNAME, string);
#if defined(LUA_COMPAT_GFIND) && !LJ_52
  lua_getfield(L, -1, "gmatch");
  lua_setfield(L, -2, "gfind");
#endif
  mt = lj_tab_new(L, 0, 1);
  /* NOBARRIER: basemt is a GC root. */
  g = G(L);
  setgcref(basemt_it(g, LJ_TSTR), obj2gco(mt));
  settabV(L, lj_tab_setstr(L, mt, mmname_str(g, MM_index)), tabV(L->top-1));
  mt->nomm = (uint8_t)(~(1u<<MM_index));
  return 1;
}

