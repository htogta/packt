#include "commands.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_POSITIONALS (8)

// every short flag is the first letter of its long flag
static const char* LONG_FLAGS[] = {"states", "force", "rollback", "error", "quiet", "text"};

typedef struct {
  bool flag[256]; // flag['f'] is true if -f/--force given
  const char* pos[3]; // positional args (blockfile comes first)
  int n;
} Opts;

typedef struct {
  uint32_t i, j;
  bool has_i, has_j;
} Range;

static bool fail(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "packt error: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\ntry 'packt help' for usage\n");
  va_end(ap);
  return false;
}
 
// split into flags and positionals
// "allowed" lists the short flags the command accepts 
// (e.g. "eq" for --error and --quiet) 
// requires at least one positional (the blockfile)
static bool parse(int argc, char** argv, const char* allowed, Opts* o) {
  memset(o, 0, sizeof(*o));
  
  for (int a = 0; a < argc; a++) {
    const char* arg = argv[a];
    
    if (arg[0] != '-') {
      if (o->n == 3) return fail("too many arguments");
      o->pos[o->n++] = arg;
      continue;
    }
     
    char c = 0;
    if (arg[1] == '-') {
      for (int k = 0; k < 6; k++) {
        if (strcmp(arg + 2, LONG_FLAGS[k]) == 0) c = LONG_FLAGS[k][0];
      }
    } else if (arg[1] && !arg[2]) {
      c = arg[1];
    }
    
    if (!c || !strchr(allowed, c)) return fail("unknown option '%s'", arg);
    o->flag[(unsigned char)c] = true;
  }
  
  if (o->n < 1) return fail("missing blockfile argument");
  return true;
}

static bool parse_u32(const char* s, uint32_t* out) {
  if (!isdigit((unsigned char)*s)) return false;
  
  char* end;
  errno = 0;
  unsigned long v = strtoul(s, &end, 10);
  
  if (*end != '\0' || errno || v > UINT32_MAX) return false;
  
  *out = (uint32_t)v;
  return true;
}

// parses "i" or "i:j" (temporarily splits the string at the colon)
static bool parse_range(char* s, Range* r) {
  char* colon = strchr(s, ':');
  if (colon) *colon = '\0';
  
  r->has_i = parse_u32(s, &r->i);
  r->has_j = colon && parse_u32(colon + 1, &r->j);
  bool ok = r->has_i && (!colon || r->has_j);
  
  if (colon) *colon = ':';
  return ok;
}
 
// parses the [i[:j]] [file] part. 
// file=NULL for commands that don't take a file
static bool parse_tail(const Opts* o, Range* r, const char** file) {
  int k = 1;
  memset(r, 0, sizeof(*r));
  if (file) *file = NULL;
  
  if (k < o->n && isdigit((unsigned char)o->pos[k][0])) {
    if (!parse_range((char*)o->pos[k], r)) {
      return fail("invalid block range '%s' (expected i or i:j)", o->pos[k]);
    }
    k++;
  }
  
  if (file && k < o->n) *file = o->pos[k++];
  if (k < o->n) return fail("unexpected argument '%s'", o->pos[k]);
  return true;
}
// TODO a leading digit means "range", so a file whose name starts with a 
// digit needs a "./" prefix, ideally this wouldn't be the case
 
int main(int argc, char** argv) {
  if (argc < 2) { 
    packt_help(); 
    return 1; 
  }
  
  const char* cmd = argv[1];
  int ac = argc - 2;
  char** av = argv + 2;
  
  Opts o;
  Range r;
  const char* file;
  
  if (!strcmp(cmd, "help")) { 
    packt_help(); 
    return 0; 
  }
  
  if (!strcmp(cmd, "version")) { 
    packt_version(); 
    return 0; 
  }
  
  // packt_* functions return true on success, so ! gives the exit code
  if (!strcmp(cmd, "new")) {
    uint32_t cap;
    if (!parse(ac, av, "", &o)) return 1;
    if (o.n != 2) { 
      fail("usage: packt new blockfile cap"); 
      return 1; 
    }
    if (!parse_u32(o.pos[1], &cap)) { 
      fail("invalid capacity '%s'", o.pos[1]); 
      return 1; 
    }
    return !packt_new(o.pos[0], cap);
  }
  
  if (!strcmp(cmd, "info")) {
    if (!parse(ac, av, "s", &o)) return 1;
    if (o.n != 1) { 
      fail("usage: packt info blockfile [-s]"); 
      return 1; 
    }
    return !packt_info(o.pos[0], o.flag['s']);
  }
  
  if (!strcmp(cmd, "clear")) {
    if (!parse(ac, av, "fr", &o) || !parse_tail(&o, &r, NULL)) return 1;
    return !packt_clear(
      o.pos[0], 
      o.flag['f'],
      o.flag['r'],
      r.has_i ? &r.i : NULL, 
      r.has_j ? &r.j : NULL
    );
  }
 
  if (!strcmp(cmd, "read")) {
    if (!parse(ac, av, "", &o) || !parse_tail(&o, &r, &file)) return 1;
    return !packt_read(
      o.pos[0], 
      file,
      r.has_i ? &r.i : NULL, 
      r.has_j ? &r.j : NULL
    );
  }
 
  if (!strcmp(cmd, "write")) {
    if (!parse(ac, av, "eq", &o) || !parse_tail(&o, &r, &file)) return 1;
    return !packt_write(
      o.pos[0], 
      file, 
      o.flag['e'], 
      o.flag['q'],
      r.has_i ? &r.i : NULL, 
      r.has_j ? &r.j : NULL
    );
  }
 
  if (!strcmp(cmd, "print")) {
    if (!parse(ac, av, "t", &o) || !parse_tail(&o, &r, NULL)) return 1;
    return !packt_print(
      o.pos[0], 
      o.flag['t'],
      r.has_i ? &r.i : NULL, 
      r.has_j ? &r.j : NULL
    );
  }
 
  fail("unknown command '%s'", cmd);
  return 1;
}
