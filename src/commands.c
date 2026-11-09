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

// print help
void packt_help() {
  // TODO
}

// print version info (for blocks and packt)
void packt_version() {
  printf("packt v0.1.0\n");
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

  printf("%s: %d blocks, %u bytes total\n", bf->path, bf->count, bf->count * 1024);

  if (!states) {
    blockfile_close(bf);
    return true;
  }
  
  // TODO some way of printing how many empty blocks there are
  
  blockfile_close(bf);
  return true;
}

// zero-out blocks
// if i not null and j is null, zeroes only block i
// if i null and j not null, error (shouldn't even happen tbh)
// if i and j both null, zeroes full blockfile
// if force true, don't prompt first
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

  // TODO rework? maybe i: could mean "clear all blocks starting from i to end"
  if (!i && j) {
    packt_error("expected range from i:j, only i, or no range at all");
    return false;
  }

  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s:", bfpath);
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }

  if (i) {
    if (*i >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *i, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
  }

  if (j) {
    if (*j >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *j, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
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
  uint32_t counter = 0;
  for (uint32_t idx = i ? (*i) : 0; idx <= (j ? (*j) : bf->count - 1); idx++) {
    int ok = block_clear(bf, idx);
    if (!ok) {
      packt_error("failed to zero block %u:", idx);
      packt_error("-> %s", blocks_failure_reason());
      blockfile_close(bf);
      return false;
    }
    counter++;
  }
  // TODO make it so that if this fails in the middle, the blockfile's state 
  // before the clear operation started is restored

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

  if (!i && j) {
    packt_error("expected range from i:j, only i, or no range at all");
    return false;
  }

  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s", bfpath);
    packt_error("-> ", blocks_failure_reason());
    return false;
  }

  if (i) {
    if (*i >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *i, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
  }

  if (j) {
    if (*j >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *j, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
  }

  uint32_t block_start = i ? (*i) : 0;
  uint32_t block_end = j ? (*j) : bf->count - 1;

  FILE* out = stdout;
  if (fpath) {
    out = fopen(fpath, "wb");
    if (!out) {
      packt_error("failed to open %s for writing", fpath);
      blockfile_close(bf);
      return false;
    }
  }

  for (uint32_t idx = block_start; idx <= block_end; idx++) {
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
  // in theory I *could* just use blockfile_load_file, but that fails if any of
  // the blocks it uses aren't empty. 
  if (!i && j) {
    packt_error("expected range from i:j, only i, or no range at all");
    return false;
  }

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

  if (i) {
    if (*i >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *i, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
  }

  if (j) {
    if (*j >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *j, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
  }

  if (quiet && error) {
    packt_error("--error and --quiet can't both be used at once");
    blockfile_close(bf);
    return false;
  }
  
  uint32_t block_start = i ? *i : 0;
  uint32_t block_end = j ? *j : bf->count - 1;

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

  for (uint32_t idx = block_start; idx <= block_end; idx++) {
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
    
    uint8_t data[BLOCK_MAX_LENGTH] = {0};
    size_t nread = 0;
    if (!input_done) {
      nread = fread(data, 1, BLOCK_MAX_LENGTH, in);

      if (ferror(in)) {
        packt_error("failed to read input");
        success = false;
        break;
      }

      if (nread < BLOCK_MAX_LENGTH) input_done = true;
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
    if (!block_update(bf, bl)) {
      packt_error("failed to write block %u:", idx);
      packt_error("-> %s", blocks_failure_reason());
      block_free(bl);
      success = false;
      break;
    }

    block_free(bl);
  }
  
  blockfile_close(bf);
  return success;
}

// pretty-print blocks
bool packt_print(const char* bfpath, bool text, uint32_t* i, uint32_t* j) {
  if (!bfpath) {
    packt_error("path to .blocks file required");
    return false;
  }

  if (!i && j) {
    packt_error("expected range from i:j, only i, or no range at all");
    return false;
  }

  BlockFile* bf = blockfile_open(bfpath);
  if (!bf) {
    packt_error("failed to open %s:", bfpath);
    packt_error("-> %s", blocks_failure_reason());
    return false;
  }

  if (i) {
    if (*i >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *i, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
  }

  if (j) {
    if (*j >= bf->count) {
      packt_error("%u is out of bounds, maximum index is %u", *j, bf->count - 1);
      blockfile_close(bf);
      return false;
    }
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
  
  uint32_t block_start = i ? (*i) : 0;
  uint32_t block_end = j ? (*j) : bf->count - 1;
  for (uint32_t idx = block_start; idx <= block_end; idx++) {
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
