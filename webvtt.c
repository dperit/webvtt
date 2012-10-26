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
CUE_IDENTIFIER = 5,
CUE_TIMINGS = 6,
CUE = 7
};

struct webvtt_parser {
  int parse_state;
  int reached_buffer_end;
  int invalid_webvtt;
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
      ctx->invalid_webvtt = 1;
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
  if (ctx->offset >= ctx->length){
    //Don't retreat the offset or change the parse state if we hit the end during whitespace 
    //just indicate we've reached the end. That way when we run through again with more buffer
    //we'll just run through this method again until the whitespace ends
    ctx->offset = ctx->length;
    ctx->reached_buffer_end = 1;
  }
}

//A WebVTT line terminator consists of one of the following:
//A U+000D CARRIAGE RETURN U+000A LINE FEED (CRLF) character pair.
//A single U+000A LINE FEED (LF) character.
//A single U+000D CARRIAGE RETURN (CR) character.
void webvtt_skip_line_terminators_before_cue(webvtt_parser *ctx){
  int starting_offset = ctx->offset;
  //TODO: Implement this!
  int number_of_terminators = 0;
  while(ctx->offset < ctx->length && (ctx->buffer[ctx->offset] == (char)0xD
                                  ||  ctx->buffer[ctx->offset] == (char)0xA)){
    if (ctx->buffer[ctx->offset] == (char)0xD 
        && (ctx->offset + 1) < ctx->length 
        && (ctx->buffer[ctx->offset + 1] == (char)0xA)){
      ctx->offset+=2;
    }else {
      ctx->offset++;
    }
    number_of_terminators++;
  }
  if (ctx->offset >= ctx->length){
    ctx->reached_buffer_end = 1;
    //return to start of the terminator set as we'll need to count them again if we run out of buffer
    ctx->offset = starting_offset;
    return;
    //Make sure there are at least 2 terminators before the cue
  }else if (number_of_terminators < 2){
    ctx->invalid_webvtt = 1;
    return;
  }
}

/*A WebVTT cue identifier is any sequence of one or more characters not containing the 
 substring "-->" (U+002D HYPHEN-MINUS, U+002D HYPHEN-MINUS, U+003E GREATER-THAN SIGN), 
 nor containing any U+000A LINE FEED (LF) characters or U+000D CARRIAGE RETURN (CR) characters.*/
void webvtt_parse_cue_identifier(webvtt_parser *ctx, webvtt_cue *cue){
  int starting_offset = ctx->offset;
  while (ctx->offset < ctx->length && (ctx->buffer[ctx->offset] != (char)0xA
                                       &&  ctx->buffer[ctx->offset] != (char)0xD)){
    //If the offset is at least 3 characters away from the length then test for the substring "-->"
    if ((ctx->length - ctx->offset) >= 3){
      if (ctx->buffer[ctx->offset] == (char)0x2D){
        if (ctx->buffer[ctx->offset+1] == (char)0x2D){
          if (ctx->buffer[ctx->offset+2] == (char)0x3E){
            //If we find it then that means they don't have an identifier- reset offset to the starting
            //position, set identifier string in the cue to NULL, and return
            ctx->offset = starting_offset;
            cue->identifier = NULL;
            return;
          }
        }
      }
    }
    ctx->offset++;
  }
  if (ctx->offset >= ctx->length){
    ctx->reached_buffer_end = 1;
    //Unfortunately we have to set the offset back to the start in this case, as otherwise we'd lose
    //the position where the identifier starts
    ctx->offset = starting_offset;
  }else {
    //if we reach here then we found the identifier!
    //Allocate memory, copy the text into it, assign the identifier in the cue to it
    //TODO: The above when I am less tired
  }
  
}

//I am a helper function to webvtt_parse_cue_timings
int webvtt_advance_through_numbers(webvtt_parser *ctx){
  while (ctx->offset < ctx->length && (ctx->buffer[ctx->offset] >= (char)0x30
                                       &&  ctx->buffer[ctx->offset] <= (char)0x39)){
    ctx->offset++;
  }
  if (ctx->offset >= ctx->length){
    ctx->reached_buffer_end = 1;
    return 0;
  }else {
    return 1;
  }

}

long webvtt_parse_timestamp(webvtt_parser *ctx){
  int hours = 0;
  int minutes = 0;
  int seconds = 0;
  int seconds_fraction = 0;
  
  int has_hours;
  
  int first_part_start_point = ctx->offset;
  
  //Advance through the numbers, get out and reset to start point if we hit the end of the buffer
  //in the process
  if (!webvtt_advance_through_numbers(ctx)){
    ctx->offset = first_part_start_point;
    return -1;
  }
  
  //If there aren't at least two numbers between the start point and the first c
  if (!(ctx->offset - first_part_start_point) >= 2){
    ctx->invalid_webvtt = 1;
    return -1;
  }
  
  //Make sure next char is a colon
  if (ctx->buffer[ctx->offset] == (char)0x3A){
    //We don't have to test that this offset increase will keep the offset within bounds because
    //advance through numbers will catch it and reset it if that is the case
    ctx->offset++;
  }else {
    ctx->invalid_webvtt = 1;
    return -1;
  }

  
  int second_part_start_point = ctx->offset;
  
  if (!webvtt_advance_through_numbers(ctx)){
    ctx->offset = first_part_start_point;
    return -1;
  }
  
  if ((ctx->offset - second_part_start_point) != 2){
    ctx->invalid_webvtt = 1;
    return -1;
  }
  
  if (ctx->buffer[ctx->offset] == (char)0x3A){
    ctx->offset++;
    //We've got hours, minutes, seconds, second fractions
    has_hours = 1;
  }else if (ctx->buffer[ctx->offset] == (char)0x2E) {
    ctx->offset++;
    //We've got minutes, seconds, second fractions
    has_hours = 0;
  }else {
    ctx->invalid_webvtt = 1;
    return -1;
  }
  
  int third_part_start_point = ctx->offset;
  
  if (!webvtt_advance_through_numbers(ctx)){
    ctx->offset = first_part_start_point;
    return -1;
  }
  
  if (has_hours){
    if ((ctx->offset - third_part_start_point) != 2){
      ctx->invalid_webvtt = 1;
      return -1;
    }
    int fourth_part_start_point = ctx->offset;
    if (!webvtt_advance_through_numbers(ctx)){
      ctx->offset = first_part_start_point;
      return -1;
    }
    if (ctx->offset - fourth_part_start_point != 3){
      ctx->invalid_webvtt = 1;
      return -1;
    }
    //Make sure the nobody sticks something clever after the seconds that causes sscanf to blow up
    if (ctx->buffer[ctx->offset + 1] != (char)0x20 && 
        ctx->buffer[ctx->offset + 1] != (char)0x9  &&
        ctx->buffer[ctx->offset + 1] != (char)0xA  &&
        ctx->buffer[ctx->offset + 1] != (char)0xD){
      ctx->invalid_webvtt = 1;
      return -1;
    }
    sscanf(ctx->buffer + first_part_start_point, "%d:%d:%d.%d", &hours, &minutes, &seconds, &seconds_fraction);
  }else {
    if ((ctx->offset - third_part_start_point) < 2){
      ctx->invalid_webvtt = 1;
      return -1;
    }
    //Make sure the nobody sticks something clever after the seconds that causes sscanf to blow up
    if (ctx->buffer[ctx->offset + 1] != (char)0x20 && 
        ctx->buffer[ctx->offset + 1] != (char)0x9  &&
        ctx->buffer[ctx->offset + 1] != (char)0xA  &&
        ctx->buffer[ctx->offset + 1] != (char)0xD){
      ctx->invalid_webvtt = 1;
      return -1;
    }
    
    hours = 0;
    sscanf(ctx->buffer + first_part_start_point, "%d:%d.%d", &minutes, &seconds, &seconds_fraction);
  }
  
  long time = hours*(1000*60*60) + minutes*(1000*60) + seconds * 1000 + seconds_fraction * 1e-3;
  
  return time;
}

void webvtt_advance_through_spaces_and_tabs(webvtt_parser *ctx){
  while (ctx->offset < ctx->length && (ctx->buffer[ctx->offset] == (char)0x20
                                   || ctx->buffer[ctx->offset] == (char)0x9)){
    ctx->offset++;
  }
  if (ctx->offset >= ctx->length){
    ctx->offset = ctx->length;
    ctx->reached_buffer_end = 1;
  }
}

void webvtt_parse_cue_timings(webvtt_parser *ctx, webvtt_cue *cue, webvtt_cue *previous_cue){
  
  //NEXTSTEP: offload much of the functionality here into another method so it can be re-used for part
  //2 of the timestamp.
  //TODO: implement a check to make sure that this cue timing has a start time greater than or equal to
  // the previous cue
  int startingOffset = ctx->offset;
  
  long startTime = webvtt_parse_timestamp(ctx);
  //TODO: Check start time against previous start time, make sure it's greater than
  if (ctx->invalid_webvtt){
    return;
  }else if (ctx->reached_buffer_end){
    ctx->offset = startingOffset;
    return;
  }
  
  webvtt_advance_through_spaces_and_tabs(ctx);
  
  if (ctx->length - ctx->offset > 3){
    if (ctx->buffer[ctx->offset] == (char)0x2D &&
        ctx->buffer[ctx->offset + 1] == ( char)0x2D &&
        ctx->buffer[ctx->offset + 2] == ( char)0x3E){
      ctx->offset += 3;
    }else {
      ctx->invalid_webvtt = 1;
      return;
    }
  }else {
    ctx->offset = startingOffset;
    ctx->reached_buffer_end = 1;
    return;
  }
  
  webvtt_advance_through_spaces_and_tabs(ctx);
  
  long endTime = webvtt_parse_timestamp(ctx);
  //TODO: Check end time against start time, make sure it's greater than
  if (ctx->invalid_webvtt){
    return;
  }else if (ctx->reached_buffer_end){
    ctx->offset = startingOffset;
    return;
  }
  cue->start = startTime;
  cue->end = endTime;
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
  
  //TODO: Get the timings of the previous cue to the one that we are currently working,
  //and then write the code that keeps it updated
  if (last_cue == NULL && first_cue != NULL){
    last_cue = first_cue;
    while (last_cue->next != NULL){
      last_cue = last_cue->next;
    }
  }
  
  //This is the cue that has the previous timings! We'll keep it updated to point to the correct cue
  webvtt_cue *has_previous_timing = last_cue;
  webvtt_cue *cue = NULL;
  
  if (ctx->parse_state >= CUE_IDENTIFIER){
    cue = last_cue;
  }
  
  //TODO: Make an enum encoding parse_states
  while(ctx->reached_buffer_end != 1 && ctx->invalid_webvtt != 1){
    
    switch(ctx->parse_state){
        //Each case should check for whatever it is going to check for and advance the parsers offset
        //to after the thing it is checking for.
        //If it hits end of buffer unexpectedly then you should reset ctx's offset to as it was at
        //the beginning of the case and set the reached_buffer_end flag to 1. This allows the
        //parser to be re-run once more file is available.
        //If it finds something indicating that the provided file is not valid then it should set the
        //invalid_webvtt flag to 1.
      case BOM:
        webvtt_parse_byte_order_mark(ctx);
        if (!ctx->reached_buffer_end){
          ctx->parse_state = SIGNATURE;
        }
        break;
      case SIGNATURE:
        webvtt_parse_signature(ctx);
        if (!ctx->reached_buffer_end && !ctx->invalid_webvtt){
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
        if (!ctx->reached_buffer_end && !ctx->invalid_webvtt){
          ctx->parse_state = CUE_IDENTIFIER;
        }
        break;
      case CUE_IDENTIFIER:
        //This is where we always make the cue, even if no cue identifier exists
        //We won't allocate anything if a cue already exists, which will happen if the buffer terminated
        //partway through the cue identifier and we're continuing from where we stopped
        if (cue == NULL){
          cue = malloc(sizeof(*cue));
          if (first_cue == NULL){
            first_cue = cue;
            last_cue = cue;
          }else {
            last_cue = webvtt_cue_link(cue, last_cue);
          }
        }
        webvtt_parse_cue_identifier(ctx, cue);
        if (!ctx->reached_buffer_end){
          ctx->parse_state = CUE_TIMINGS;
        }
      case CUE_TIMINGS:
        webvtt_parse_cue_timings(ctx, cue, has_previous_timing);
        if (!ctx->reached_buffer_end && !ctx->invalid_webvtt){
          
        }
      case CUE:
        cue = webvtt_parse_cue(ctx);
        if (!ctx->reached_buffer_end && !ctx->invalid_webvtt){
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
