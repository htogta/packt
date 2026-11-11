#define BLOCKS_IMPLEMENTATION
#include "commands.h"
#include <ctype.h>

static void packt_error(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "packt error: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static void packt_warn(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "packt warning: ");
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static bool resolve_range(
  const BlockFile* bf, 
  const uint32_t* i, 
  const uint32_t* j,
  bool open_ended,
  uint32_t* start, 
  uint32_t* end
) {
  if (!i && j) {
    packt_error("expected range from i:j, only i, or no range at all");
    return false;
  }
  
  if (bf->count == 0) {
    packt_error("%s contains no blocks", bf->path);
    return false;
  }
  
  uint32_t last = bf->count - 1;
  if (i && *i > last) {
    packt_error("%u is out of bounds, maximum index is %u", *i, last);
    return false;
  }
  
  if (j && *j > last) {
    packt_error("%u is out of bounds, maximum index is %u", *j, last);
    return false;
  }
  
  *start = i ? *i : 0;
  
  if (j) {
    *end = *j;
  } else if (i && !open_ended){
    *end = *i;
  } else {
    *end = last;
  }
  
  if (*start > *end) {
    packt_error("range start %u is after range end %u", *start, *end);
    return false;
  }
  
  return true;
}

// print help
void packt_help() {
  printf(
    "usage:\n"
    "  packt help\n"
    "  packt version\n"
    "  packt new blockfile cap\n"
    "  packt info blockfile [-s|--states]\n"
    "  packt clear blockfile [-f|--force] [i[:j]]\n"
    "  packt read blockfile [i[:j]] [file]\n"
    "  packt write blockfile [-e|--error] [-q|--quiet] [i[:j]] [file]\n"
    "  packt print blockfile [-t|--text] [i[:j]]\n");
}

// print version info (for blocks and packt)
void packt_version() {
  printf("packt v%s\n", PACKT_VERSION);
  printf("blocks.h v%s\n", BLOCKS_VERSION);
}

// create new blockfile of size=capacity
bool packt_new(const char* bfpath, uint32_t capacity) {
  if (!bfpath) {
    packt_error("path to .blocks file required");
    return false;
  }

  if (capacity == 0) {
    packt_error("capacity must be >0");
    return false;
  }
  
  BlockFile* bf = blockfile_create(bfpath, capacity);
  if (!bf) {
    packt_error("failed to create new .blocks file:");
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }
  
  blockfile_close(bf);
  return true;
}

// get info about blockfile
bool packt_info(const char* bfpath, bool states) {
  if (!bfpath) {
    packt_error("path to .blocks file required");
    return false;
  }

  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s:", bfpath);
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }

  printf("%s: %d blocks, %llu bytes total\n", bf->path, bf->count, 
    (unsigned long long) bf->count * BLOCK_MAX_LENGTH);

  // TODO some way of printing how many empty blocks there are 
  if (states) packt_warn("--states is currently unimplemented");
  
  blockfile_close(bf);
  return true;
}

// zero-out blocks
// if i not null and j is null, zeroes only block i
// if i null and j not null, error (shouldn't even happen tbh)
// if i and j both null, zeroes full blockfile
// if force true, don't prompt first
// transactional- if the clear fails midway, cleared blocks are restored
bool packt_clear(
  const char* bfpath, 
  bool force, 
  uint32_t* i, 
  uint32_t* j
) {
  if (!bfpath) {
    packt_error("path to .blocks file required");
    return false;
  }


  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s:", bfpath);
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }
  
  uint32_t start, end;
  if (!resolve_range(bf, i, j, false, &start, &end)) {
    blockfile_close(bf);
    return false;
  }
  
  if (!force) {
    bool yesno = false; // "no" is default
    char input[16];
    
    if (!i && !j) {
      printf("Are you sure you want to clear ALL blocks? [y/N]: ");
    } else if (i && !j) {
      printf("Are you sure you want to clear block %u? [y/N]: ", *i);
    } else {
      printf("Are you sure you want to clear blocks %u through %u? [y/N]: ",
        *i, *j);
    }
    fflush(stdout);

    if (fgets(input, sizeof(input), stdin)) {
      switch(tolower(input[0])) {
        case 'y': yesno = true; break;
        case 'n':
        case '\n':
        case '\0':
          yesno = false;
          break;
        default:
          yesno = false;
          break;
      }
    }

    if (!yesno) {
      printf("Aborted.\n");
      blockfile_close(bf);
      return true;
    }
  }
  
  // TODO print "cleaning..."? or nah

  // holds the pre-clear contents of cleared blocks so they can be restored
  FILE* journal = tmpfile();
  if (!journal) {
    packt_error("failed to create rollback journal");
    blockfile_close(bf);
    return false;
  }

  uint32_t counter = 0;
  bool success = true;

  for (uint32_t idx = start; idx <= end; idx++) {
    Block* bl = block_read(bf, idx);
    if (!bl) {
      packt_error("failed to read block %u:", idx);
      packt_error("-> %s", blocks_failure_reason());
      success = false;
      break;
    }

    // check if empty
    bool occupied = false;
    for (uint32_t k = 0; k < BLOCK_MAX_LENGTH; k++) {
      if (bl->data[k] != 0) {
        occupied = true;
        break;
      }
    }

    if (occupied) {
      if ( // journal before clearing
        fwrite(&idx, sizeof(idx), 1, journal) != 1 ||
        fwrite(bl->data, 1, BLOCK_MAX_LENGTH, journal) != BLOCK_MAX_LENGTH
      ) {
        packt_error("failed to journal block %u", idx);
        block_free(bl);
        success = false;
        break;
      }
    }

    block_free(bl);

    // already-empty blocks are skipped
    if (occupied && !block_clear(bf, idx)) {
      packt_error("failed to zero block %u:", idx);
      packt_error("-> %s", blocks_failure_reason());
      success = false;
      break;
    }

    counter++;
  }

  if (!success) {
    // roll back- restore the journaled blocks to their pre-clear state
    rewind(journal);

    uint32_t restored = 0;
    bool complete = true;

    for (;;) {
      uint32_t num;
      uint8_t data[BLOCK_MAX_LENGTH];

      if (fread(&num, sizeof(num), 1, journal) != 1) break; // journal exhausted

      if (fread(data, 1, BLOCK_MAX_LENGTH, journal) != BLOCK_MAX_LENGTH) {
        packt_error(
          "failed to read rollback journal, %s may partially cleared",
          bf->path);
        complete = false;
        break;
      }

      Block* bl = block_new(data, BLOCK_MAX_LENGTH);
      if (!bl) {
        packt_error("failed to restore block %u:", num);
        packt_error("-> %s", blocks_failure_reason());
        complete = false;
        break;
      }

      bl->number = num;
      bool ok = block_update(bf, bl);
      block_free(bl);

      if (!ok) {
        packt_error(
          "failed to restore block %u, %s may be left partially cleared",
          num, bf->path);
        complete = false;
        break;
      }

      restored++;
    }
    
    if (complete && restored > 0) {
      fprintf(stderr,"Restored %u blocks\n", restored);
    }
  }

  fclose(journal);

  if (!success) {
    blockfile_close(bf);
    return false;
  }

  printf("Cleared %u blocks\n", counter);

  blockfile_close(bf);
  return true;
}

// read blocks from a blockfile into a file
bool packt_read(
  const char* bfpath, 
  const char* fpath, 
  uint32_t* i, 
  uint32_t* j
) {
  if (!bfpath) {
    packt_error("path to .blocks file required");
    return false;
  }
  
  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s", bfpath);
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }

  uint32_t start, end;
  if (!resolve_range(bf, i, j, false, &start, &end)) {
    blockfile_close(bf);
    return false;
  }
  
  FILE* out = stdout;
  if (fpath) {
    out = fopen(fpath, "wb");
    if (!out) {
      packt_error("failed to open %s for writing", fpath);
      blockfile_close(bf);
      return false;
    }
  }

  for (uint32_t idx = start; idx <= end; idx++) {
    Block* bl = block_read(bf, idx);
    if (!bl) {
      packt_error("failed to read block %u:", idx);
      packt_error("-> %s", blocks_failure_reason());
      if (fpath) fclose(out);
      blockfile_close(bf);
      return false;
    }

    size_t written = fwrite(bl->data, 1, BLOCK_MAX_LENGTH, out);
    block_free(bl);

    if (written != BLOCK_MAX_LENGTH) {
      packt_error("failed to write block %u", idx);
      if (fpath) fclose(out);
      blockfile_close(bf);
      return false;
    }
  }

  fflush(out);
  if (fpath) fclose(out);
  
  blockfile_close(bf);
  return true;
}

// write data from a file as blocks to a blockfile
bool packt_write(
  const char* bfpath, 
  const char* fpath, 
  bool error,
  bool quiet,
  uint32_t* i, 
  uint32_t* j
) {
  if (!bfpath) {
    packt_error("path to .blocks file required");
    return false;
  }

  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s:", bfpath);
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }

  uint32_t start, end;
  if (!resolve_range(bf, i, j, true, &start, &end)) {
    blockfile_close(bf);
    return false;
  }

  if (quiet && error) {
    packt_error("--error and --quiet can't both be used at once");
    blockfile_close(bf);
    return false;
  }

  FILE* in = stdin;
  if (fpath) {
    in = fopen(fpath, "rb");
    if (!in) {
      packt_error("failed to open %s for reading", fpath);
      blockfile_close(bf);
      return false;
    }
  }

  bool input_done = false;
  bool success = true;

  for (uint32_t idx = start; idx <= end; idx++) {
    uint8_t data[BLOCK_MAX_LENGTH] = {0};
    
    if (!input_done) {
      size_t nread = fread(data, 1, BLOCK_MAX_LENGTH, in);
      if (ferror(in)) {
        packt_error("failed to read input");
        success = false;
        break;
      }
      if (nread < BLOCK_MAX_LENGTH) input_done = true;
    }
    
    // read existing block
    Block* old = block_read(bf, idx);
    if (!old) {
      packt_error("failed to read block %u", idx);
      packt_error("-> %s", blocks_failure_reason());
      success = false;
      break;
    }
    
    // check whether the block is empty
    bool occupied = false;
    for (uint32_t k = 0; k < BLOCK_MAX_LENGTH; k++) {
      if (old->data[k] != 0) {
        occupied = true;
        break;
      }
    }
    block_free(old);
    
    if (occupied) {
      if (error) {
        packt_error("block %u already contains data", idx);
        success = false;
        break;
      }

      if (!quiet) packt_warn("block %u already contains data", idx);
    }

    Block* bl = block_new(data, BLOCK_MAX_LENGTH);
    if (!bl) {
      packt_error("failed to create block:");
      packt_error("-> %s", blocks_failure_reason());
      success = false;
      break;
    }

    // write the block
    bl->number = idx;
    bool ok = block_update(bf, bl);
    block_free(bl); 
    
    if (!ok) {
      packt_error("failed to write block %u:", idx);
      packt_error("-> %s", blocks_failure_reason());
      success = false;
      break;
    }
  }

  if (success && !input_done && fgetc(in) != EOF) {
    packt_error("input didn't fit in blocks %u through %u, the rest was dropped", start, end);
    success = false;
  }

  if (fpath) fclose(in);
  blockfile_close(bf);
  return success;
}

// pretty-print blocks
bool packt_print(const char* bfpath, bool text, uint32_t* i, uint32_t* j) {
  if (!bfpath) {
    packt_error("path to .blocks file required");
    return false;
  }

  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s:", bfpath);
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }

  uint32_t start, end;
  if (!resolve_range(bf, i, j, false, &start, &end)) {
    blockfile_close(bf);
    return false;
  }

  // calculate line number width
  uint32_t line_width = 1;
  uint32_t max_lines = BLOCK_MAX_LENGTH;
  while (max_lines >= 10) {
    max_lines /= 10;
    line_width++;
  }
  
  // calculate hex addr width
  uint32_t addr_width = 1;
  uint32_t max_addr = BLOCK_MAX_LENGTH - 1;
  while (max_addr >= 16) {
    max_addr >>= 4;
    addr_width++;
  }
  
  for (uint32_t idx = start; idx <= end; idx++) {
    Block* bl = block_read(bf, idx);
    if (!bl) {
      packt_error("failed to read block %u:", idx);
      packt_error("-> %s", blocks_failure_reason());
      blockfile_close(bf);
      return false;
    }
    printf("BLOCK %u:\n", idx);
    // if text, then do a running line counter
    if (text) {
      // trim trailing null bytes
      uint32_t len = BLOCK_MAX_LENGTH;
      while (len > 0 && bl->data[len - 1] == '\0') len--;

      uint32_t null_count = BLOCK_MAX_LENGTH - len;

      uint32_t line = 1;
      bool line_start = true;

      for (uint32_t pos = 0; pos < len; pos++) {
        unsigned char c = bl->data[pos];

        if (line_start) {
          printf("%*u | ", (int)line_width, line);
          line_start = false;
        }

        if (c == '\n') {
          putchar('\n');
          line++;
          line_start = true;
        } else if (c == '\0') {
          printf("\\0");
        } else if (isprint(c)) {
          putchar(c);
        } else {
          printf("\\x%02X", c);
        }
      }

      if (!line_start) putchar('\n');

      if (null_count > 0) printf("(%u null bytes)\n", null_count);
    } else { // printing raw bytes
      for (uint32_t pos = 0; pos < BLOCK_MAX_LENGTH; pos += 16) {
        printf("%0*X | ", (int)addr_width, pos);

        for (uint32_t k = 0; k < 16 && pos + k < BLOCK_MAX_LENGTH; k++) {
          unsigned char c = bl->data[pos + k];

          if (c >= 0x20 && c <= 0x7e) {
            printf("%c  ", c);
          } else {
            printf("%02X ", c);
          }
        }

        putchar('\n');
      }
    }
    
    block_free(bl);
  }

  blockfile_close(bf);
  fflush(stdout);
  return true;
}

bool packt_archive(
  const char* bfpath, 
  bool bytes, 
  bool dir,
  size_t* i,
  size_t* j,
  size_t num_paths,
  ...
) {
  (void)bfpath; (void)bytes; (void)dir; (void)i; (void)j; (void)num_paths;
  packt_error("archive is as-of-yet unimplemented");
  return true;
}
