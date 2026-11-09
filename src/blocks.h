#ifndef BLOCKS_H
#define BLOCKS_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#define BLOCKS_VERSION "1.0.0-beta"

// BlockFiles are expected to have a 8-byte header. The first 3 bytes are "bl"
// and then a newline, the next 4 bytes (BIG ENDIAN!) are the number of blocks
// in the file, followed by another newline. Each block is 1024 bytes.
typedef struct {
  char* path;
  uint32_t count; // number of blocks that fit in the file (capacity)
  FILE* fp;
} BlockFile;

#define BLOCK_MAX_LENGTH (1024)

typedef struct {
  uint32_t number;
  uint8_t data[BLOCK_MAX_LENGTH];
} Block;

// Returns a human-readable string describing the last failure, from any of the
// following API functions. Note that it isn't thread-safe.
const char* blocks_failure_reason(void);

// Creates a new BlockFile at "path" with a fixed capacity of "capacity" 
// blocks, all implicitly zeroed. The file is sparse; its logical size 
// immediately reflects the full capacity, but no disk space is actually used
// until a given Block is written to.
// Returns NULL if a file already exists at "path", or if another error occurs.
BlockFile* blockfile_create(const char* path, uint32_t capacity);

// Opens an (existing) file as a BlockFile (heap-allocated).
// Returns null on failure.
BlockFile* blockfile_open(const char* path);

// Reads the contents of a file at "path" and writes it into "dest", starting
// at block "start". If the file doesn't fit evenly into blocks, the last block
// of the file's contents will be padded with zeroes at the end.
// Fails if the data doesn't fit within dest's remaining capacity, or if any 
// target block doesn't already read as entirely zero (so we don't overwrite
// existing data).
// Returns truthy on success, falsy on failure.
int blockfile_load_file(BlockFile* dest, uint32_t start, const char* path);

// Closes (and frees) a blockfile.
void blockfile_close(BlockFile* bf);

// Creates a new block (with default number 0), returning null on failure. You
// can create an empty block with block_new(NULL, 0). The block is allocated on
// the heap.
Block* block_new(const uint8_t* data, size_t length);

// Frees a block from memory.
void block_free(Block* bl);

// Allocates memory for a block, and reads its data from a blockfile at a
// specific location (number). number must be < bf->count. A block that's never
// been written to reads back as all zeroes.
// Returns null on failure.
Block* block_read(const BlockFile* bf, uint32_t number);

// Overwrites a block at a specific location in a blockfile. The location is
// determined by new_block->number, which must be < bf->count.
// Returns truthy upon success and falsy upon failure.
int block_update(BlockFile* bf, Block* new_block);

// Overwrites a block with zeroes at a specific location in a blockfile.
// Returns truthy upon success and falsy upon failure.
int block_clear(BlockFile* bf, uint32_t number);

#ifdef BLOCKS_IMPLEMENTATION

#include <stdlib.h>
#include <string.h>

// most recent error stored here
static const char* blocks_last_error = NULL;

const char* blocks_failure_reason(void) {
  return blocks_last_error;
}

// records a reason, returns 0 - for int-returning functions
static int blocks_fail(const char* reason) {
  blocks_last_error = reason;
  return 0;
}

// records a reason, returns NULL - for pointer-returning functions
static void* blocks_fail_ptr(const char* reason) {
  blocks_last_error = reason;
  return NULL;
}

// calculating a block's offset in a blockfile from its number
static inline long block_offset(uint32_t number) {
  return 8 + (long)number * BLOCK_MAX_LENGTH;
}
// ^ TODO this has a chance of overflowing if the blockfile's capacity is
// really high, specifically on a system where sizeof(long) is less than 8

// since strdup isn't actually in the C99 standard
static char* blocks_strdup(const char* s) {
  size_t len = strlen(s) + 1;
  char* copy = malloc(len);
  if (!copy) return NULL;
  memcpy(copy, s, len);
  return copy;
}

// helper for writing the count to the header
static int blockfile_write_count(BlockFile* bf) {
  uint8_t count_bytes[4] = {
    (uint8_t)(bf->count >> 24), (uint8_t)(bf->count >> 16),
    (uint8_t)(bf->count >> 8),  (uint8_t)(bf->count)
  };
  
  if (fseek(bf->fp, 3, SEEK_SET) != 0) return 0;
  
  if (fwrite(count_bytes, 1, 4, bf->fp) != 4) return 0;
  
  fflush(bf->fp);
  return 1;
}

// helper for checking that a block is empty
static int block_is_empty(const Block* bl) {
  for (size_t i = 0; i < BLOCK_MAX_LENGTH; i++) {
    if (bl->data[i] != 0) return 0;
  }
  return 1;
}

BlockFile* blockfile_create(const char* path, uint32_t capacity) {
  if (!path) return blocks_fail_ptr("blockfile_create: path is NULL");
  
  FILE* existing = fopen(path, "rb");
  if (existing) {
    fclose(existing);
    return blocks_fail_ptr("blockfile_create: file already exists");
  }
  
  FILE* fp = fopen(path, "w+b");
  if (!fp) return blocks_fail_ptr("blockfile_create: failed to create file");
  
  BlockFile* bf = malloc(sizeof(BlockFile));
  if (!bf) {
    fclose(fp);
    remove(path);
    return blocks_fail_ptr(
      "blockfile_create: failed to allocate memory for BlockFile");
  }
  
  bf->path = blocks_strdup(path);
  if (!bf->path) {
    fclose(fp);
    free(bf);
    remove(path);
    return blocks_fail_ptr(
      "blockfile_create: failed to allocate memory for path");
  }
  
  bf->fp = fp;
  bf->count = capacity;

  // big-endian
  uint8_t header[8] = {
    'b', 'l', '\n',
    (uint8_t)(capacity >> 24), (uint8_t)(capacity >> 16),
    (uint8_t)(capacity >> 8),  (uint8_t)(capacity),
    '\n'
  };
  
  if (fwrite(header, 1, 8, fp) != 8) {
    fclose(fp);
    free(bf->path);
    free(bf);
    remove(path);
    return blocks_fail_ptr("blockfile_create: failed to write header");
  }
  
  if (capacity > 0) {
    // establish the file's full logical size without allocating disk space 
    // seek to the very last byte the capacity implies
    long last_byte = 8 + (long)capacity * BLOCK_MAX_LENGTH - 1;
    if (fseek(fp, last_byte, SEEK_SET) != 0) {
      fclose(fp);
      free(bf->path);
      free(bf);
      remove(path);
      return blocks_fail_ptr(
        "blockfile_create: failed to seek while sizing BlockFile");
    }
    
    // write one byte there
    if (fputc(0, fp) == EOF) {
      fclose(fp);
      free(bf->path);
      free(bf);
      remove(path);
      return blocks_fail_ptr("blockfile_create: failed to size BlockFile");
    }

    // On mainstream filesystems, this leaves everything before it as a "hole"
    // rather than actually allocating the space. TODO Even though it's safe
    // to assume this is the case, it'd be nice if we didn't have to.
  }

  fflush(fp);
  return bf;
}

BlockFile* blockfile_open(const char* path) {
  if (!path) return blocks_fail_ptr("blockfile_open: path is NULL");
  
  FILE* fp = fopen(path, "r+b");
  if (!fp) return blocks_fail_ptr("blockfile_open: failed to create new file");
  
  BlockFile* bf = malloc(sizeof(BlockFile));
  if (!bf) {
    fclose(fp);
    return blocks_fail_ptr(
      "blockfile_open: failed to allocate memory for BlockFile");
  }
  
  bf->path = blocks_strdup(path);
  if (!bf->path) {
    fclose(fp);
    free(bf);
    return blocks_fail_ptr(
      "blockfile_open: failed to allocate memory for path");
  }
  
  bf->fp = fp;

  uint8_t header[8];
  rewind(fp);
  if (
    fread(header, 1, 8, fp) != 8 || header[0] != 'b' || header[1] != 'l' ||
    header[2] != '\n' || header[7] != '\n'
  ) {
    fclose(fp);
    free(bf->path);
    free(bf);
    return blocks_fail_ptr("blockfile_open: malformed header");
  }
  
  // remember, BIG ENDIAN
  bf->count = ((uint32_t)header[3] << 24) | ((uint32_t)header[4] << 16) |
              ((uint32_t)header[5] << 8)  |  (uint32_t)header[6];
  
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    free(bf->path);
    free(bf);
    return blocks_fail_ptr(
      "blockfile_open: failed to seek to end of BlockFile");
  }
  
  long file_size = ftell(fp);
  if (file_size < 0) {
    fclose(fp);
    free(bf->path);
    free(bf);
    return blocks_fail_ptr(
      "blockfile_open: failed to determine BlockFile size");
  }
  
  long expected_size = 8 + (long)bf->count * BLOCK_MAX_LENGTH;
  if (file_size != expected_size) {
    fclose(fp);
    free(bf->path);
    free(bf);
    return blocks_fail_ptr(
      "blockfile_open: file size doesn't match its declared capacity");
  }
  
  return bf;
}

int blockfile_load_file(BlockFile* dest, uint32_t start, const char* path) {
  if (!dest) return blocks_fail("blockfile_load_file: dest is NULL");
  
  if (!dest->fp) {
    return blocks_fail("blockfile_load_file: dest's file pointer is NULL");
  }
  
  if (!path) return blocks_fail("blockfile_load_file: path is NULL");
  
  if (start > dest->count) {
    return blocks_fail("blockfile_load_file: start is out of range");
  }
  
  FILE* src = fopen(path, "rb");
  if (!src) {
    return blocks_fail("blockfile_load_file: failed to open file at path");
  }
  
  if (fseek(src, 0, SEEK_END) != 0) {
    fclose(src);
    return blocks_fail(
      "blockfile_load_file: failed to seek to end of file at path");
  }
  
  long src_size = ftell(src);
  if (src_size < 0) {
    fclose(src);
    return blocks_fail(
      "blockfile_load_file: failed to determine size of file at path");
  }
  rewind(src);
  
  // how many blocks the source needs (rounds up)
  uint32_t needed =
    (uint32_t)((src_size + BLOCK_MAX_LENGTH - 1) / BLOCK_MAX_LENGTH);
  
  uint32_t remaining_capacity = dest->count - start;
  if (needed > remaining_capacity) {
    fclose(src);
    return blocks_fail(
    "blockfile_load_file: file at path doesn't fit in dest's remaining capacity"
    );
  }
  
  // don't overwrite existing data (make sure blocks read as all zeroes)
  for (uint32_t i = 0; i < needed; i++) {
    Block* existing = block_read(dest, start + i);
    if (!existing) {
      fclose(src);
      return 0; // reason set by block_read
    }
    
    int empty = block_is_empty(existing);
    block_free(existing);
    
    if (!empty) {
      fclose(src);
      return blocks_fail(
        "blockfile_load_file: a target block already has data in it");
    }
  }
  
  uint32_t number = start;
  uint8_t buffer[BLOCK_MAX_LENGTH];
  size_t read_bytes;
  while ((read_bytes = fread(buffer, 1, BLOCK_MAX_LENGTH, src)) > 0) {
    Block* bl = block_new(buffer, read_bytes);
    if (!bl) {
      fclose(src);
      return 0; // reason set by block_new
    }
    
    bl->number = number;
    int ok = block_update(dest, bl);
    block_free(bl);
    
    if (!ok) {
      fclose(src);
      return 0; // reason set by block_update
    }
    
    number++;
  }
  
  if (ferror(src)) {
    fclose(src);
    return blocks_fail("blockfile_load_file: I/O error reading file at path");
  }
  
  fclose(src);
  return 1;
}

void blockfile_close(BlockFile* bf) {
  if (!bf) {
    blocks_fail("blockfile_close: BlockFile is NULL");
    return;
  }
  
  if (bf->fp) fclose(bf->fp);
  free(bf->path);
  free(bf);
}

Block* block_new(const uint8_t* data, size_t length) {
  if (length > BLOCK_MAX_LENGTH) {
    return blocks_fail_ptr("block_new: length > BLOCK_MAX_LENGTH");
  }

  Block* bl = malloc(sizeof(Block));
  if (!bl) {
    return blocks_fail_ptr("block_new: failed to allocate memory for Block");
  }
  
  bl->number = 0; // new blocks are initialized with number 0

  if (length == 0 && !data) { // creates empty block
    memset(bl->data, 0, BLOCK_MAX_LENGTH);
    return bl;
  }

  if (!data) { // otherwise, if length != 0 and data is null, we have a problem
    free(bl);
    return blocks_fail_ptr(
      "block_new: Block length is nonzero but data is NULL");
  }
  
  memcpy(bl->data, data, length); // copy data into the block
  memset(bl->data + length, 0, BLOCK_MAX_LENGTH - length); // zero what's left

  return bl;
}

void block_free(Block* bl) {
  if (!bl) return;
  free(bl);
}

Block* block_read(const BlockFile* bf, uint32_t number) {
  if (!bf) return blocks_fail_ptr("block_read: BlockFile is NULL");
  
  if (!bf->fp) {
    return blocks_fail_ptr("block_read: BlockFile file pointer is NULL");
  }

  if (number >= bf->count) {
    return blocks_fail_ptr("block_read: Block number is out of bounds");
  } 
  
  long offset = block_offset(number);
  if (fseek(bf->fp, offset, SEEK_SET) != 0) {
    return blocks_fail_ptr(
      "block_read: failed to seek to Block position in BlockFile");
  }
  
  Block* bl = malloc(sizeof(Block));
  if (!bl) return blocks_fail_ptr("block_read: failed to allocate Block");
  
  memset(bl->data, 0, BLOCK_MAX_LENGTH);
  
  size_t read_bytes = fread(bl->data, 1, BLOCK_MAX_LENGTH, bf->fp);
  if (read_bytes < BLOCK_MAX_LENGTH && ferror(bf->fp)) {
    free(bl);
    return blocks_fail_ptr("block_read: I/O error when reading Block");
  }
  
  // shouldn't happen with the guard above, but might as well
  if (read_bytes == 0) {
    free(bl);
    return blocks_fail_ptr("block_read: I/O error when reading Block");
  }
  
  bl->number = number;
  return bl;
}

int block_update(BlockFile* bf, Block* new_block) {
  if (!bf) return blocks_fail("block_update: BlockFile is NULL");
  
  if (!bf->fp) { 
    return blocks_fail("block_update: BlockFile file pointer is NULL");
  }
  
  if (!new_block) return blocks_fail("block_update: new Block is NULL");
  
  if (new_block->number >= bf->count) {
    return blocks_fail(
      "block_update: Block doesn't exist in BlockFile (out of bounds)");
  }
  
  long offset = block_offset(new_block->number);
  if (fseek(bf->fp, offset, SEEK_SET) != 0) {
    return blocks_fail(
      "block_update: failed to seek to Block position in BlockFile"
    );
  }
  
  if (
    fwrite(new_block->data, 1, BLOCK_MAX_LENGTH, bf->fp) != BLOCK_MAX_LENGTH
  ) return blocks_fail("block_update: failed to write Block data to BlockFile");
  
  fflush(bf->fp);
  return 1;
}

int block_clear(BlockFile* bf, uint32_t number) {
  if (!bf) return blocks_fail("block_clear: BlockFile is NULL");

  if (number >= bf->count) {
    return blocks_fail("block_clear: Block number is out of bounds");
  }

  Block* empty_block = block_new(NULL, 0);
  if (!empty_block) return 0; // reason set by block_new

  empty_block->number = number;
  int ok = block_update(bf, empty_block);
  block_free(empty_block);

  return ok; // reason set by block_update
}

#endif // BLOCKS_IMPLEMENTATION

#endif // BLOCKS_H
