#ifndef PACKT_COMMANDS_H
#define PACKT_COMMANDS_H

#include "blocks.h"
#include <stdbool.h>
#include <stdarg.h>

#define PACKT_VERSION ("0.1.0")

// print help
void packt_help();

// print version info (for blocks and packt)
void packt_version();

// create new blockfile of size=capacity
bool packt_new(const char* bfpath, uint32_t capacity);

// get info about blockfile
bool packt_info(const char* bfpath, bool states);

// zero-out blocks
bool packt_clear(
  const char* bfpath, 
  bool force, 
  bool rollback,
  uint32_t* i, 
  uint32_t* j
);

// read a file into a blockfile
bool packt_read(
  const char* bfpath, 
  const char* fpath, 
  uint32_t* i, 
  uint32_t* j
);

// write blocks to a file
bool packt_write(
  const char* bfpath, 
  const char* fpath,  
  bool error,
  bool quiet,
  uint32_t* i, 
  uint32_t* j
);

// pretty-print blocks
bool packt_print(const char* bfpath, bool text, uint32_t* i, uint32_t* j);

// archive a bunch of files into a blockfile
bool packt_archive(
  const char* bfpath, 
  bool bytes, 
  bool dir,
  size_t* i,
  size_t* j,
  size_t num_paths,
  ...
);

#endif // PACKT_COMMANDS_H
