/* WebVTT parser
   Copyright 2011 Mozilla Foundation

   This Source Code Form is subject to the terms of the Mozilla
   Public License, v. 2.0. If a copy of the MPL was not distributed
   with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "webvtt.h"

#define BUFFER_SIZE 4096

#ifndef MIN
#define MIN(x, y) (((x) < (y)) ? (x) : (y))
#endif

enum state {
BOM = 0,
SIGNATURE = 1,
FIND_CHARACTERS_BEFORE_CUES = 2,
SKIP_CHARACTERS_BEFORE_CUES = 3,
SKIP_LINE_TERMINATORS_BEFORE_CUE = 4,
CUE = 5
};

struct webvtt_parser {
  int parse_state;
  int reached_buffer_end;
  int invalid_buffer;
  int has_BOM;
  char *buffer;
  long offset, length;
};

webvtt_parser *
webvtt_parse_new(void)
{
  webvtt_parser *ctx = malloc(sizeof(*ctx));
  if (ctx) {
    ctx->parse_state = 0;
    ctx->buffer = malloc(BUFFER_SIZE);
    if (ctx->buffer == NULL) {
      free(ctx);
      return NULL;
    }
    ctx->offset = 0;
    ctx->length = 0;
  }
  return ctx;
}

void
webvtt_parse_free(webvtt_parser *ctx)
{
  if (ctx) {
    ctx->parse_state = 0;
    if (ctx->buffer) {
      free(ctx->buffer);
      ctx->buffer = NULL;
    }
    ctx->offset = 0;
    ctx->length = 0;
  }
}

int
webvtt_print_cue(FILE *out, webvtt_cue *cue)
{
  int err;
  int h, m, s, ms;
  long time;

  if (out == NULL || cue == NULL)
    return -1;

  time = cue->start;
  h = time/3600000;
  time %= 3600000;
  m = time/60000;
  time %= 60000;
  s = time/1000;
  ms = time%1000;
  err = fprintf(out, "%02d:%02d:%02d.%03d", h, m, s, ms);

  time = cue->end;
  h = time/3600000;
  time %= 3600000;
  m = time/60000;
  time %= 60000;
  s = time/1000;
  err = fprintf(out, " --> %02d:%02d:%02d.%03d\n", h, m, s, ms);

  err = fprintf(out, "%s\n\n", cue->text);

  return err;
}

webvtt_cue *
webvtt_parse_cue(webvtt_parser *ctx)
{
  webvtt_cue *cue = NULL;

  if (ctx == NULL)
    return NULL;
  if (ctx->buffer == NULL || ctx->length - ctx->offset < 24)
    return NULL;

  char *p = ctx->buffer + ctx->offset;
  while (p - ctx->buffer < ctx->length && isspace(*p))
    p++;

  int smin,ssec,smsec;
  int emin,esec,emsec;
  //TODO: implement timestamp parsing rules
  int items = sscanf(p, "%d:%d.%d --> %d:%d.%d",
                        &smin, &ssec, &smsec, &emin, &esec, &emsec);
  if (items < 6) {
    fprintf(stderr, "Couldn't parse cue timestamps\n");
    return NULL;
  }
  double start_time = smin*60 + ssec + smsec * 1e-3;
  double end_time = emin*60 + esec + emsec * 1e-3;

  while (p - ctx->buffer < ctx->length && *p != '\r' && *p != '\n')
    p++;
  p++;
  char *e = p;
  while (e - ctx->buffer + 4 < ctx->length) {
    if (!memcmp(e, "\n\n", 2))
      break;
    if (!memcmp(e, "\r\n\r\n", 4))
      break;
    if (!memcmp(e, "\r\r", 2))
      break;
    e++;
  }
  // TODO: handle last four bytes properly
  while (e - ctx->buffer < ctx->length && *e != '\n' && *e != '\r')
    e++;

  char *text = malloc(e - p + 1);
  if (text == NULL) {
    fprintf(stderr, "Couldn't allocate cue text buffer\n");
    return NULL;
  }
  cue = malloc(sizeof(*cue));
  if (cue == NULL) {
    fprintf(stderr, "Couldn't allocate cue structure\n");
    free(text);
    return NULL;
  }
  memcpy(text, p, e - p);
  text[e - p] = '\0';

  cue->start = start_time * 1e3;
  cue->end = end_time * 1e3;
  cue->text = text;
  cue->next = NULL;

  ctx->offset = e - ctx->buffer;

  return cue;
}

//TODO: Document where this was found in the standard according to style found elsewhere, probably in the tests?
//Returns 1 if the BOM is present, 0 if not. Also advances the ctx offset by 3 if the BOM is found
int webvtt_parse_byte_order_mark(webvtt_parser *ctx){
  if (ctx->length < 3){
    ctx->reached_buffer_end = 1;
    return -1;
  }else {
    char *p = ctx->buffer;
    //Assuming that this set of hexcodes == U+FEFF BYTE ORDER MARK (BOM)
    if (p[0] == (char)0xef && p[1] == (char)0xbb && p[2] == (char)0xbf){
      ctx->offset += 3;
      ctx->has_BOM = 1;
      return 1;
    }else {
      ctx->has_BOM = 0;
      return 0;
    }
  }
}

int webvtt_parse_signature(webvtt_parser *ctx){
  // Check for signature
  if (!ctx->has_BOM && ctx->length < 6) {
    ctx->reached_buffer_end = 1;
    fprintf(stderr, "Too short. Not capable of parsing signature yet\n");
    return -1;
  }else if (ctx->has_BOM && ctx->length < 9) {
    ctx->reached_buffer_end = 1;
    fprintf(stderr, "Too short. Not capable of parsing signature yet\n");
    return -1;
  }else {
    if (memcmp(ctx->buffer + ctx->offset, "WEBVTT", 6)) {
      ctx->offset += 6;
      return 1;
    }else {
      ctx->invalid_buffer = 1;
      return 0;
    }
  }
}

//Optionally, either a U+0020 SPACE character or a U+0009 CHARACTER TABULATION (tab) character 
//followed by any number of characters that are not U+000A LINE FEED (LF) or 
//U+000D CARRIAGE RETURN (CR) characters.
int webvtt_find_characters_before_cues(webvtt_parser *ctx){
  if (ctx->offset < ctx->length){
    if (ctx->buffer[ctx->offset] == (char)0x20 || ctx->buffer[ctx->offset] == (char)0x9){
      ctx->offset++;
      return 1;
    }else {
      return 0;
    }
  }else {
    ctx->reached_buffer_end = 1;
    return -1;
  }
}

void webvtt_skip_characters_before_cues(webvtt_parser *ctx){
  while (ctx->offset < ctx->length && (ctx->buffer[ctx->offset] != (char)0xA
                                       && ctx->buffer[ctx->offset] != (char)0xD)){
    ctx->offset++;
  }
  if (ctx->offset == ctx->length){
    //Don't retreat the offset or change the parse state if we hit the end during whitespace 
    //just indicate we've reached the end. That way when we run through again with more buffer
    //we'll just run through this method again until the whitespace ends
    ctx->reached_buffer_end = 1;
  }  
}

//A WebVTT line terminator consists of one of the following:

//A U+000D CARRIAGE RETURN U+000A LINE FEED (CRLF) character pair.
//A single U+000A LINE FEED (LF) character.
//A single U+000D CARRIAGE RETURN (CR) character.
void webvtt_skip_line_terminators_before_cue(webvtt_parser *ctx){
  //TODO: Implement this!
  //if (ctx->offset < ctx->length){
//  }else {
//    ctx->reached_buffer_end = 1;
//  }

}

webvtt_cue * webvtt_cue_link(webvtt_cue *to_link, webvtt_cue *to_link_from){
  if (to_link && to_link_from) {
    to_link_from->next = to_link;
    return to_link;
  }else {
    return to_link_from;
  }
}

webvtt_cue *
webvtt_parse(webvtt_parser *ctx, webvtt_cue *first_cue, webvtt_cue* last_cue)
{
  
  if (last_cue == NULL && first_cue != NULL){
    last_cue = first_cue;
    while (last_cue->next != NULL){
      last_cue = last_cue->next;
    }
  }
  webvtt_cue *cue = NULL;
  
  //TODO: Make an enum encoding parse_states
  while(ctx->reached_buffer_end != 1 && ctx->invalid_buffer != 1){
    
    switch(ctx->parse_state){
        //Each case should check for whatever it is going to check for and advance the parsers offset
        //to after the thing it is checking for.
        //If it hits end of buffer unexpectedly then you should reset ctx's offset to as it was at
        //the beginning of the case and set the reached_buffer_end flag to 1. This allows the
        //parser to be re-run once more file is available.
        //If it finds something indicating that the provided file is not valid then it should set the
        //invalid_buffer flag to 1.
      case BOM:
        webvtt_parse_byte_order_mark(ctx);
        if (!ctx->reached_buffer_end){
          ctx->parse_state = SIGNATURE;
        }
        break;
      case SIGNATURE:
        webvtt_parse_signature(ctx);
        if (!ctx->reached_buffer_end && !ctx->invalid_buffer){
          ctx->parse_state = FIND_CHARACTERS_BEFORE_CUES;
        }
        break;
      case FIND_CHARACTERS_BEFORE_CUES:
        //Gives the compiler a scope to put has_characters_before_cues in
        if(1){
          int has_characters_before_cues = webvtt_find_characters_before_cues(ctx);
          if (!ctx->reached_buffer_end){
            if (has_characters_before_cues){
              ctx->parse_state = SKIP_CHARACTERS_BEFORE_CUES;
            }else {
              ctx->parse_state = SKIP_LINE_TERMINATORS_BEFORE_CUE;
            }
          }
        }
        break;
      case SKIP_CHARACTERS_BEFORE_CUES:
        webvtt_skip_characters_before_cues(ctx);
        if (!ctx->reached_buffer_end){
          ctx->parse_state = SKIP_LINE_TERMINATORS_BEFORE_CUE;
        }
        break;
      case SKIP_LINE_TERMINATORS_BEFORE_CUE:
        webvtt_skip_line_terminators_before_cue(ctx);
        if (!ctx->reached_buffer_end && !ctx->invalid_buffer){
          ctx->parse_state = CUE;
        }
        break;
      case CUE:
        cue = webvtt_parse_cue(ctx);
        if (!ctx->reached_buffer_end && !ctx->invalid_buffer){
          if (first_cue == NULL){
            first_cue = cue;
            last_cue = cue;
          }
          last_cue = webvtt_cue_link(cue, last_cue);
        }
        break;
    }
  }
  webvtt_cue *head = cue;
  while (head != NULL) {
    webvtt_print_cue(stderr, head);
    head = head->next;
  }
  return cue;
}

webvtt_cue *
webvtt_parse_buffer(webvtt_parser *ctx, char *buffer, long length)
{
  long bytes = MIN(length, BUFFER_SIZE - ctx->length);

  memcpy(ctx->buffer, buffer, bytes);
  ctx->length += bytes;

  return webvtt_parse(ctx, NULL, NULL);
}

webvtt_cue *
webvtt_parse_file(webvtt_parser *ctx, FILE *in)
{
  ctx->length = fread(ctx->buffer, 1, BUFFER_SIZE, in);
  ctx->offset = 0;

  if (ctx->length >= BUFFER_SIZE)
    fprintf(stderr, "WARNING: truncating input at %d bytes."
                    " This is a bug,\n", BUFFER_SIZE);

  return webvtt_parse(ctx, NULL, NULL);
}

webvtt_cue *
webvtt_parse_filename(webvtt_parser *ctx, const char *filename)
{
  FILE *in = fopen(filename, "r");
  webvtt_cue *cue = NULL;
  
  if (in) {
    cue = webvtt_parse_file(ctx, in);
    fclose(in);
  }
  
  return cue;
}
