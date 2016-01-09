/*********************************************************************
  Blosc - Blocked Shuffling and Compression Library

  Author: Francesc Alted <francesc@blosc.org>
  Creation date: 2015-07-30

  See LICENSES/BLOSC.txt for details about copyright and rights to use.
**********************************************************************/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "blosc.h"
#include "delta.h"

#if defined(_WIN32) && !defined(__MINGW32__)
  #include <windows.h>
  #include <malloc.h>

/* stdint.h only available in VS2010 (VC++ 16.0) and newer */
  #if defined(_MSC_VER) && _MSC_VER < 1600
    #include "win32/stdint-windows.h"
  #else
    #include <stdint.h>
  #endif

#endif  /* _WIN32 */

/* If C11 is supported, use it's built-in aligned allocation. */
#if __STDC_VERSION__ >= 201112L
  #include <stdalign.h>
#endif


/* Encode filters in a 16 bit int type */
uint16_t encode_filters(schunk_params* params) {
  int i;
  uint16_t enc_filters = 0;

  /* Encode the BLOSC_MAX_FILTERS filters (3-bit encoded) in 16 bit */
  for (i = 0; i < BLOSC_MAX_FILTERS; i++) {
    enc_filters += params->filters[i] << (i * 3);
  }
  return enc_filters;
}


/* Decode filters.  The returned array must be freed after use.  */
uint8_t* decode_filters(uint16_t enc_filters) {
  int i;
  uint8_t* filters = malloc(BLOSC_MAX_FILTERS);

  /* Decode the BLOSC_MAX_FILTERS filters (3-bit encoded) in 16 bit */
  for (i = 0; i < BLOSC_MAX_FILTERS; i++) {
    filters[i] = (uint8_t)(enc_filters & 0x3);
    enc_filters >>= 3;
  }
  return filters;
}


/* Create a new super-chunk */
schunk_header* blosc2_new_schunk(schunk_params* params) {
  schunk_header* sc_header = calloc(1, sizeof(schunk_header));

  sc_header->version = 0;     /* pre-first version */
  sc_header->filters = encode_filters(params);
  sc_header->filters_meta = params->filters_meta;
  sc_header->compressor = params->compressor;
  sc_header->clevel = params->clevel;
  sc_header->cbytes = sizeof(schunk_header);
  /* The rest of the structure will remain zeroed */

  return sc_header;
}


/* Append an existing chunk into a super-chunk. */
size_t blosc2_append_chunk(schunk_header* sc_header, void* chunk, int copy) {
  int64_t nchunks = sc_header->nchunks;
  /* The uncompressed and compressed sizes start at byte 4 and 12 */
  int32_t nbytes = *(int32_t*)((uint8_t*)chunk + 4);
  int32_t cbytes = *(int32_t*)((uint8_t*)chunk + 12);
  void* chunk_copy;

  /* By copying the chunk we will always be able to free it later on */
  if (copy) {
    chunk_copy = malloc((size_t)cbytes);
    memcpy(chunk_copy, chunk, (size_t)cbytes);
    chunk = chunk_copy;
  }

  /* Make space for appending a new chunk and do it */
  sc_header->data = realloc(sc_header->data, (nchunks + 1) * sizeof(void*));
  sc_header->data[nchunks] = chunk;
  /* Update counters */
  sc_header->nchunks = nchunks + 1;
  sc_header->nbytes += nbytes;
  sc_header->cbytes += cbytes + sizeof(void*);
  /* printf("Compression chunk #%lld: %d -> %d (%.1fx)\n", */
  /*         nchunks, nbytes, cbytes, (1.*nbytes) / cbytes); */

  return (size_t)nchunks + 1;
}


/* Set a delta reference for the super-chunk */
int blosc2_set_delta_ref(schunk_header* sc_header, size_t nbytes, void* ref) {
  int cbytes;
  void* filters_chunk;
  uint8_t* dec_filters = decode_filters(sc_header->filters);

  if (dec_filters[0] == BLOSC_DELTA) {
    if (sc_header->filters_chunk != NULL) {
      sc_header->cbytes -= *(uint32_t*)(sc_header->filters_chunk + 4);
      free(sc_header->filters_chunk);
    }
  }
  else {
    printf("You cannot set a delta reference if delta filter is not set\n");
    return(-1);
  }
  free(dec_filters);

  filters_chunk = malloc(nbytes + BLOSC_MAX_OVERHEAD);
  cbytes = blosc_compress(0, 0, 1, nbytes, ref, filters_chunk,
                          nbytes + BLOSC_MAX_OVERHEAD);
  if (cbytes < 0) {
    free(filters_chunk);
    return cbytes;
  }
  sc_header->filters_chunk = filters_chunk;
  sc_header->cbytes += cbytes;
  return cbytes;
}


/* Append a data buffer to a super-chunk. */
size_t blosc2_append_buffer(schunk_header* sc_header, size_t typesize,
                            size_t nbytes, void* src) {
  int cbytes;
  void* chunk = malloc(nbytes + BLOSC_MAX_OVERHEAD);
  uint8_t* dec_filters = decode_filters(sc_header->filters);
  int clevel = sc_header->clevel;
  char* compname;
  int doshuffle, ret;

  /* Apply filters prior to compress */
  if (dec_filters[0] == BLOSC_DELTA) {
    doshuffle = dec_filters[1];
    if (sc_header->filters_chunk == NULL) {
      ret = blosc2_set_delta_ref(sc_header, nbytes, src);
      if (ret < 0) {
        return((size_t)ret);
      }
    }
  }
  else {
    doshuffle = dec_filters[0];
  }
  free(dec_filters);

  /* Compress the src buffer using super-chunk defaults */
  blosc_compcode_to_compname(sc_header->compressor, &compname);
  blosc_set_compressor(compname);
  blosc_set_schunk(sc_header);
  cbytes = blosc_compress(clevel, doshuffle, typesize, nbytes, src, chunk,
                          nbytes + BLOSC_MAX_OVERHEAD);
  if (cbytes < 0) {
    free(chunk);
    return cbytes;
  }

  /* Append the chunk (no copy required here) */
  return blosc2_append_chunk(sc_header, chunk, 0);
}


/* Decompress and return a chunk that is part of a super-chunk. */
int blosc2_decompress_chunk(schunk_header* sc_header, int64_t nchunk, void* dest, int nbytes) {
  int64_t nchunks = sc_header->nchunks;
  void* src;
  int chunksize;
  int nbytes_;
  uint8_t* filters = decode_filters(sc_header->filters);

  if (nchunk >= nchunks) {
    printf("specified nchunk ('%ld') exceeds the number of chunks ('%ld') in super-chunk\n", (long)nchunk, (long)nchunks);
    return -10;
  }

  /* Grab the address of the chunk */
  src = sc_header->data[nchunk];
  /* Create a buffer for destination */
  nbytes_ = *(int32_t*)((uint8_t*)src + 4);

  if (nbytes < nbytes_) {
    printf("Buffer size is too small for the decompressed buffer ('%d' bytes, but '%d' are needed)\n", nbytes, nbytes_);
    return -11;
  }

  /* Put the super-chunk address in the global context for Blosc1 */
  blosc_set_schunk(sc_header);

  /* And decompress the chunk */
  chunksize = blosc_decompress(src, dest, (size_t)nbytes);

  free(filters);

  return chunksize;
}


/* Free all memory from a super-chunk. */
int blosc2_destroy_schunk(schunk_header* sc_header) {
  int i;

  if (sc_header->filters_chunk != NULL)
    free(sc_header->filters_chunk);
  if (sc_header->codec_chunk != NULL)
    free(sc_header->codec_chunk);
  if (sc_header->metadata_chunk != NULL)
    free(sc_header->metadata_chunk);
  if (sc_header->userdata_chunk != NULL)
    free(sc_header->userdata_chunk);
  if (sc_header->data != NULL) {
    for (i = 0; i < sc_header->nchunks; i++) {
      free(sc_header->data[i]);
    }
    free(sc_header->data);
  }
  free(sc_header);
  return 0;
}


/* Compute the final length of a packed super-chunk */
int64_t blosc2_get_packed_length(schunk_header* sc_header) {
  int i;
  int64_t length = BLOSC_HEADER_PACKED_LENGTH;

  if (sc_header->filters_chunk != NULL)
    length += *(int32_t*)(sc_header->filters_chunk + 12);
  if (sc_header->codec_chunk != NULL)
    length += *(int32_t*)(sc_header->codec_chunk + 12);
  if (sc_header->metadata_chunk != NULL)
    length += *(int32_t*)(sc_header->metadata_chunk + 12);
  if (sc_header->userdata_chunk != NULL)
    length += *(int32_t*)(sc_header->userdata_chunk + 12);
  if (sc_header->data != NULL) {
    for (i = 0; i < sc_header->nchunks; i++) {
      length += sizeof(int64_t);
      length += *(int32_t*)(sc_header->data[i] + 12);
    }
  }
  return length;
}

/* Copy a chunk into a packed super-chunk */
void pack_copy_chunk(void* chunk, void* packed, int offset, int64_t* cbytes, int64_t* nbytes) {
  int32_t cbytes_, nbytes_;

  if (chunk != NULL) {
    nbytes_ = *(int32_t*)((uint8_t*)chunk + 4);
    cbytes_ = *(int32_t*)((uint8_t*)chunk + 12);
    memcpy((uint8_t*)packed + (size_t)*cbytes, chunk, (size_t)cbytes_);
    *(int64_t*)((uint8_t*)packed + offset) = *cbytes;
    *nbytes += nbytes_;
    *cbytes += cbytes_;
  }
  else {
    /* No data in chunk */
    *(int64_t*)((uint8_t*)packed + offset) = 0;
  }
}


/* Create a packed super-chunk */
void* blosc2_pack_schunk(schunk_header* sc_header) {
  int64_t cbytes = BLOSC_HEADER_PACKED_LENGTH;
  int64_t nbytes = BLOSC_HEADER_PACKED_LENGTH;
  int64_t nchunks = sc_header->nchunks;
  void* packed;
  void* data_chunk;
  uint64_t* data_pointers;
  uint64_t data_offsets_len;
  int32_t chunk_cbytes, chunk_nbytes;
  int64_t packed_len;
  int i;

  packed_len = blosc2_get_packed_length(sc_header);
  packed = malloc((size_t)packed_len);

  /* Fill the header */
  memcpy(packed, sc_header, 40);    /* copy until cbytes */

  /* Fill the ancillary chunks info */
  pack_copy_chunk(sc_header->filters_chunk,  packed, 40, &cbytes, &nbytes);
  pack_copy_chunk(sc_header->codec_chunk,    packed, 48, &cbytes, &nbytes);
  pack_copy_chunk(sc_header->metadata_chunk, packed, 56, &cbytes, &nbytes);
  pack_copy_chunk(sc_header->userdata_chunk, packed, 64, &cbytes, &nbytes);

  /* Finally, setup the data pointers section */
  data_offsets_len = nchunks * sizeof(int64_t);
  data_pointers = (uint64_t*)((uint8_t*)packed + packed_len - data_offsets_len);
  *(uint64_t*)((uint8_t*)packed + 72) = packed_len - data_offsets_len;

  /* And fill the actual data chunks */
  if (sc_header->data != NULL) {
    for (i = 0; i < nchunks; i++) {
      data_chunk = sc_header->data[i];
      chunk_nbytes = *(int32_t*)((uint8_t*)data_chunk + 4);
      chunk_cbytes = *(int32_t*)((uint8_t*)data_chunk + 12);
      memcpy((uint8_t*)packed + cbytes, data_chunk, (size_t)chunk_cbytes);
      data_pointers[i] = cbytes;
      cbytes += chunk_cbytes;
      nbytes += chunk_nbytes;
    }
  }

  /* Add the length for the data chunk offsets */
  cbytes += data_offsets_len;
  nbytes += data_offsets_len;
  assert (cbytes == packed_len);
  *(int64_t*)((uint8_t*)packed + 16) = nchunks;
  *(int64_t*)((uint8_t*)packed + 24) = nbytes;
  *(int64_t*)((uint8_t*)packed + 32) = cbytes;

  return packed;
}


/* Copy a chunk into a packed super-chunk */
void* unpack_copy_chunk(uint8_t* packed, int offset, schunk_header* sc_header,
                        int64_t *nbytes, int64_t *cbytes) {
  int32_t nbytes_, cbytes_;
  uint8_t *chunk, *dst_chunk = NULL;

  if (*(int64_t*)(packed + offset) != 0) {
    chunk = packed + *(int64_t*)(packed + offset);
    nbytes_ = *(int32_t*)(chunk + 4);
    cbytes_ = *(int32_t*)(chunk + 12);
    /* Create a copy of the chunk */
    dst_chunk = malloc((size_t)cbytes_);
    memcpy(dst_chunk, chunk, (size_t)cbytes_);
    /* Update counters */
    sc_header->nbytes += nbytes_;
    sc_header->cbytes += cbytes_;
    *cbytes += cbytes_;
    *nbytes += nbytes_;
  }
  return dst_chunk;
}


/* Unpack a packed super-chunk */
schunk_header* blosc2_unpack_schunk(void* packed) {
  schunk_header* sc_header = calloc(1, sizeof(schunk_header));
  int64_t nbytes = BLOSC_HEADER_PACKED_LENGTH;
  int64_t cbytes = BLOSC_HEADER_PACKED_LENGTH;
  uint8_t* data_chunk;
  void* new_chunk;
  int64_t* data;
  int64_t nchunks;
  int32_t chunk_size;
  int i;

  /* Fill the header */
  memcpy(sc_header, packed, 40); /* Copy until cbytes */

  /* Fill the ancillary chunks info */
  sc_header->filters_chunk = unpack_copy_chunk(packed, 40, sc_header, &nbytes, &cbytes);
  sc_header->codec_chunk = unpack_copy_chunk(packed, 48, sc_header, &nbytes, &cbytes);
  sc_header->metadata_chunk = unpack_copy_chunk(packed, 56, sc_header, &nbytes, &cbytes);
  sc_header->userdata_chunk = unpack_copy_chunk(packed, 64, sc_header, &nbytes, &cbytes);

  /* Finally, fill the data pointers section */
  data = (int64_t*)((uint8_t*)packed + *(int64_t*)((uint8_t*)packed + 72));
  nchunks = *(int64_t*)((uint8_t*)packed + 16);
  sc_header->data = malloc(nchunks * sizeof(void*));
  nbytes += nchunks * sizeof(int64_t);
  cbytes += nchunks * sizeof(int64_t);

  /* And create the actual data chunks */
  if (data != NULL) {
    for (i = 0; i < nchunks; i++) {
      data_chunk = (uint8_t*)packed + data[i];
      chunk_size = *(int32_t*)(data_chunk + 12);
      new_chunk = malloc((size_t)chunk_size);
      memcpy(new_chunk, data_chunk, (size_t)chunk_size);
      sc_header->data[i] = new_chunk;
      cbytes += chunk_size;
      nbytes += *(int32_t*)(data_chunk + 4);
    }
  }
  sc_header->nbytes = nbytes;
  sc_header->cbytes = cbytes;

  assert(*(int64_t*)((uint8_t*)packed + 24) == nbytes);
  assert(*(int64_t*)((uint8_t*)packed + 32) == cbytes);

  return sc_header;
}


/* Append an existing chunk into a *packed* super-chunk. */
void* blosc2_packed_append_chunk(void* packed, void* chunk) {
  int64_t nchunks = *(int64_t*)((uint8_t*)packed + 16);
  int64_t packed_len = *(int64_t*)((uint8_t*)packed + 32);
  int64_t data_offsets = *(int64_t*)((uint8_t*)packed + 72);
  uint64_t chunk_offset = packed_len - nchunks * sizeof(int64_t);
  /* The uncompressed and compressed sizes start at byte 4 and 12 */
  int32_t nbytes = *(int32_t*)((uint8_t*)chunk + 4);
  int32_t cbytes = *(int32_t*)((uint8_t*)chunk + 12);
  /* The current and new data areas */
  uint8_t* data;
  uint8_t* new_data;

  /* Make space for the new chunk and copy it */
  packed = realloc(packed, packed_len + cbytes + sizeof(int64_t));
  data = (uint8_t*)packed + data_offsets;
  new_data = data + cbytes;
  /* Move the data offsets to the end */
  memmove(new_data, data, (size_t)(nchunks * sizeof(int64_t)));
  ((uint64_t*)new_data)[nchunks] = chunk_offset;
  /* Copy the chunk */
  memcpy((uint8_t*)packed + chunk_offset, chunk, (size_t)cbytes);
  /* Update counters */
  *(int64_t*)((uint8_t*)packed + 16) += 1;
  *(uint64_t*)((uint8_t*)packed + 24) += nbytes + sizeof(uint64_t);
  *(uint64_t*)((uint8_t*)packed + 32) += cbytes + sizeof(uint64_t);
  *(uint64_t*)((uint8_t*)packed + 72) += cbytes;
  /* printf("Compression chunk #%lld: %d -> %d (%.1fx)\n",
          nchunks, nbytes, cbytes, (1.*nbytes) / cbytes); */

  return packed;
}


/* Append a data buffer to a *packed* super-chunk. */
void* blosc2_packed_append_buffer(void* packed, size_t typesize, size_t nbytes, void* src) {
  int cname = *(int16_t*)((uint8_t*)packed + 4);
  int clevel = *(int16_t*)((uint8_t*)packed + 6);
  void* filters_chunk = (uint8_t*)packed + *(uint64_t*)((uint8_t*)packed + 40);
  uint8_t* filters = decode_filters(*(uint16_t*)((uint8_t*)packed + 8));
  int cbytes;
  void* chunk = malloc(nbytes + BLOSC_MAX_OVERHEAD);
  void* dest = malloc(nbytes);
  char* compname;
  int doshuffle;
  void* new_packed;

  /* Apply filters prior to compress */
  if (filters[0] == BLOSC_DELTA) {
    doshuffle = filters[1];
    if (filters_chunk == NULL) {
      /* For packed super-buffers, the filters schunk should exist */
      return NULL;
    }
    delta_encoder8(filters_chunk, 0, (int)nbytes, src, dest);
    /* memcpy(dest, src, nbytes); */
    src = dest;
  }
  else {
    doshuffle = filters[0];
  }

  /* Compress the src buffer using super-chunk defaults */
  blosc_compcode_to_compname(cname, &compname);
  blosc_set_compressor(compname);
  cbytes = blosc_compress(clevel, doshuffle, typesize, nbytes, src, chunk,
                          nbytes + BLOSC_MAX_OVERHEAD);
  if (cbytes < 0) {
    free(chunk);
    free(dest);
    free(filters);
    return NULL;
  }

  /* We don't need dest and filters anymore */
  free(dest);
  free(filters);

  /* Append the chunk and free it */
  new_packed = blosc2_packed_append_chunk(packed, chunk);
  free(chunk);

  return new_packed;
}


/* Decompress and return a chunk that is part of a *packed* super-chunk. */
int blosc2_packed_decompress_chunk(void* packed, int nchunk, void** dest) {
  int64_t nchunks = *(int64_t*)((uint8_t*)packed + 16);
  uint8_t* filters = decode_filters(*(uint16_t*)((uint8_t*)packed + 8));
  uint8_t* filters_chunk = (uint8_t*)packed + *(uint64_t*)((uint8_t*)packed + 40);
  int64_t* data = (int64_t*)((uint8_t*)packed + *(int64_t*)((uint8_t*)packed + 72));
  void* src;
  int chunksize;
  int32_t nbytes;

  if (nchunk >= nchunks) {
    return -10;
  }

  /* Grab the address of the chunk */
  src = (uint8_t*)packed + data[nchunk];
  /* Create a buffer for destination */
  nbytes = *(int32_t*)((uint8_t*)src + 4);
  *dest = malloc((size_t)nbytes);

  /* And decompress it */
  chunksize = blosc_decompress(src, *dest, (size_t)nbytes);
  if (chunksize < 0) {
    return chunksize;
  }
  if (chunksize != nbytes) {
    return -11;
  }

  /* Apply filters after de-compress */
  if (filters[0] == BLOSC_DELTA) {
    delta_decoder8(filters_chunk, 0, nbytes, *dest);
  }

  free(filters);
  return chunksize;
}
