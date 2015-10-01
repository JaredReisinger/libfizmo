
/* history.c

 * This file is part of fizmo.
 *
 * Copyright (c) 2009-2014 Christoph Ender.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *
 * This file implements a stream- or window-history which stores the output
 * of the Z-Machine for windows. It may be used to implement a scrollback
 * buffer, to refresh the contents of a screen or to redraw the screen as
 * a response on a window resize event.
 *
 * The history is implemented as a circular buffer. Once the hardcoded or
 * user-given buffer maximum size is reached, the oldest data is overwritten
 * with the newest. The buffer start is stored in "z_history_buffer_start",
 * the end at "z_history_buffer_end". "z_history_buffer_front_index" points
 * to the front of the buffer, meaning to the point where the next character
 * will be stored. "history_buffer_back_index_background" points to the oldest
 * stored character. "wrapped_around" indicates whether the buffer is in a
 * wrap-around situation -- meaning that the front- is behind the back-index.
 * In case front == back, the buffer is empty when "wrapped_around" is false,
 * or completely filled in case "wrapped_around" is true.
 *
 * Metadata -- font, style and colour attributes -- is directly written
 * into this buffer. In order to distinguish metadata from regular buffer
 * contents, metadata is prefixed with a HISTORY_METADATA_ESCAPE character
 * which is 0. Since 0 is used as a string-terminator, regular buffer contents
 * can never contain a plain 0. Thus, when writing output into the buffer, 0
 * characters don't have to be explicitely escaped since these are never
 * written.
 *
 * In order to use the history one can create a *history_output using
 * "init_history_output" which will point to the current end of the history.
 * (... "output_rewind_paragraph", "output_repeat_paragraphs")
 *
 * For greater output flexibility, a repeated output's paragraph is never
 * followed by a newline char. This, however, introduces the ambiguity that
 * for the very last paragraph it's not known whether it has been
 * terminated with a newline or not, which is relevant when redrawing the
 * screen. To solve this, the "output_rewind_paragraph" will set the flag
 * "rewound_paragraph_was_newline_terminated" to false in case the very
 * last paragraph in the buffer is not yet followed by a newline char.
 *
 * Please note: The buffer size must have at least the size of the largest
 * metadata entry, which is 4 z_ucs-chars.
 */


#ifndef history_c_INCLUDED
#define history_c_INCLUDED

#include <stdlib.h>
#include <stdarg.h>

#include "../tools/tracelog.h"
#include "../tools/types.h"
#include "../tools/z_ucs.h"
#include "../tools/i18n.h"
#include "../locales/libfizmo_locales.h"
#include "history.h"
#include "fizmo.h"
#include "config.h"


#define REPEAT_PARAGRAPH_BUF_SIZE 1280


outputhistory_ptr outputhistory[]
 = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };


OUTPUTHISTORY *create_outputhistory(int window_number,
    size_t maximum_buffer_size, size_t buffer_increment_size,
    z_colour foreground_colour, z_colour background_color, z_font font,
    z_style style)
{
  OUTPUTHISTORY *result;

  if ((result = malloc(sizeof(OUTPUTHISTORY))) == NULL)
    return NULL;

  result->window_number = window_number;

  result->z_history_buffer_size = 0;

  result->z_history_maximum_buffer_size = maximum_buffer_size;

  result->z_history_buffer_increment_size = buffer_increment_size;

  result->z_history_buffer_start = NULL;
  result->z_history_buffer_end = NULL;
  result->z_history_buffer_front_index = NULL;
  result->z_history_buffer_back_index = NULL;
  result->nof_wraparounds = 0;
  result->last_metadata_block_index = 0;
  result->next_newline_after_buffer_back = NULL;

  result->history_buffer_back_index_font = font;
  result->history_buffer_back_index_style = style;
  result->history_buffer_back_index_foreground = foreground_colour;
  result->history_buffer_back_index_background = background_color;

  result->history_buffer_front_index_font = font;
  result->history_buffer_front_index_style = style;
  result->history_buffer_front_index_foreground = foreground_colour;
  result->history_buffer_front_index_background = background_color;

  return result;
}


void destroy_outputhistory(OUTPUTHISTORY *h)
{
  free(h->z_history_buffer_start);
  free(h);
}


static size_t get_buffer_space_used(OUTPUTHISTORY *h)
{
  if (h->z_history_buffer_size == 0)
    return 0;
  else if (h->nof_wraparounds == 0)
    return h->z_history_buffer_front_index - h->z_history_buffer_back_index + 1;
  else
    return h->z_history_buffer_size
      - (h->z_history_buffer_back_index - h->z_history_buffer_front_index);
}


static void process_buffer_back(OUTPUTHISTORY *h, long nof_zucs_chars)
{
  z_ucs *current_index = h->z_history_buffer_front_index;
  z_ucs buf1, buf2;

  TRACE_LOG("Advancing buffer end from %p by %ld chars.\n",
      current_index,
      (long int)nof_zucs_chars);

  do {
    TRACE_LOG("current-index: %p.\n", current_index);

    // Check if we have caught up to the last processed paragraph-attribute
    // position.
    if (h->next_newline_after_buffer_back == current_index) {
      h->next_newline_after_buffer_back = NULL;
    }

    if (*current_index == HISTORY_METADATA_ESCAPE)
    {
      // We've found an escape code. Advance index to escape code type.
      current_index
        = current_index == h->z_history_buffer_end
        ? h->z_history_buffer_start
        : current_index + 1;
      nof_zucs_chars--;

      if (*current_index == HISTORY_METADATA_ESCAPE)
      {
        // All other cases (only 0 is legal) indicate normal data, thus we
        // can ignore it.
      }
      else if (*current_index == HISTORY_METADATA_TYPE_FONT)
      {
        // Font metadata found, advance to font data.
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        nof_zucs_chars--;

        h->history_buffer_back_index_font = (z_font)*current_index;
      }
      else if (*current_index == HISTORY_METADATA_TYPE_STYLE)
      {
        // Style metadata found, advance to style data.
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        nof_zucs_chars--;

        h->history_buffer_back_index_style = (z_style)*current_index;
      }
      else if (*current_index == HISTORY_METADATA_TYPE_COLOUR)
      {
        // Colour metadata found, advance to foreground data.
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        nof_zucs_chars--;

        h->history_buffer_back_index_foreground
          = (z_colour)*current_index;

        // Advance to background data.
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        nof_zucs_chars--;

        h->history_buffer_back_index_background
          = (z_colour)*current_index;
      }
      else if (*current_index == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE) {
        // do nothing but catch the case so we're not running into the
        // error-else below.
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        nof_zucs_chars--;
        buf1 = *current_index;

        // Advance to second parameter
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        nof_zucs_chars--;

        if ( (paragraph_removal_function != NULL)
            && (h->next_newline_after_buffer_back == NULL) ) {
          paragraph_removal_function(
              buf1 - HISTORY_METADATA_DATA_OFFSET,
              *current_index - HISTORY_METADATA_DATA_OFFSET);
        }
      }
      else
      {
        TRACE_LOG("Inconsistent history metadata.\n");
        i18n_translate_and_exit(
            libfizmo_module_name,
            i18n_libfizmo_INVALID_PARAMETER_TYPE_P0S,
            -1,
            "metadata");
      }
    }

    // After processing, move to next z_ucs.
    current_index
      = current_index == h->z_history_buffer_end
      ? h->z_history_buffer_start
      : current_index + 1;
    nof_zucs_chars--;
  }
  while (nof_zucs_chars > 0);
  // It's possible that we're advancing more chars than requested, in case
  // a metadata sequence occupies the end of the processing range. Since the
  // history bugger is specified to have minimum buffer size of a complete
  // metadata entry though, this will work without problems.

  if ( (paragraph_removal_function != NULL)
      && (h->next_newline_after_buffer_back == NULL) ) {
    //printf("passed next_newline_after_buffer_back.\n");
    // Either we have never run "process_buffer_back" before, or we have
    // passed the last evaluated paragraph-attribute position in the loop
    // above. Either way, we now have to look for the next paragraph in
    // order to keep the to keep a consistent paragraph attribute situation
    // (meaning: Only whole paragraphs in the history count).

    h->next_newline_after_buffer_back = current_index;

    while (*current_index != Z_UCS_NEWLINE) {

      if (current_index == h->z_history_buffer_front_index) {
        // Only a single block of text in the buffer? Well, okay.
        //printf("XXXX\n");
        break;
      }

      // Advance 1 char.
      current_index
        = current_index == h->z_history_buffer_end
        ? h->z_history_buffer_start
        : current_index + 1;

      // In case there's some metadata at this point, process it.
      if (*current_index == HISTORY_METADATA_ESCAPE) {

        // Advance to metadata type.
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        // Remember this in buf1.
        buf1 = *current_index;

        // Advance to first parameter.
        current_index
          = current_index == h->z_history_buffer_end
          ? h->z_history_buffer_start
          : current_index + 1;
        // Remember this in buf2.
        buf2 = *current_index;

        if ( (buf1 == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE)
            || (buf1 == HISTORY_METADATA_TYPE_COLOUR) ) {

          // Advance to second parameter.
          current_index
            = current_index == h->z_history_buffer_end
            ? h->z_history_buffer_start
            : current_index + 1;
          nof_zucs_chars--;

          if (buf1 == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE) {
            paragraph_removal_function(
                buf2 - HISTORY_METADATA_DATA_OFFSET,
                *current_index - HISTORY_METADATA_DATA_OFFSET);
          }
        }
      }
    }

    //printf("bufferback: next_newline_after_buffer_back %p.\n",
    //    h->next_newline_after_buffer_back);

    h->next_newline_after_buffer_back = current_index;
  }

  //result->next_newline_after_buffer_back = NULL;

  TRACE_LOG("Processed chars up to before %p.\n", current_index);
}


static size_t get_buffer_space_available(OUTPUTHISTORY *h)
{
  if (h->z_history_buffer_size == 0)
    return 0;
  else if (h->nof_wraparounds == 0)
    return h->z_history_buffer_end - h->z_history_buffer_front_index + 1;
  else
    return h->z_history_buffer_back_index - h->z_history_buffer_front_index;
}


static void try_to_enlarge_buffer(OUTPUTHISTORY *h, size_t desired_z_ucs_size)
{
  z_ucs *ptr;

  TRACE_LOG("Trying to enlarge history buffer to %ld bytes.\n",
      (long int)(sizeof(z_ucs) * desired_z_ucs_size));

  if ((ptr = realloc(
          h->z_history_buffer_start,
          sizeof(z_ucs) * (desired_z_ucs_size + 1))) != NULL)
          //sizeof(z_ucs) * desired_z_ucs_size)) != NULL)
  {
    h->z_history_buffer_size
      = desired_z_ucs_size;

    h->z_history_buffer_end
      = ptr + h->z_history_buffer_size - 1;

    *(h->z_history_buffer_end + 1) = 0;

    h->z_history_buffer_front_index
      = ptr + (h->z_history_buffer_front_index - h->z_history_buffer_start);

    h->z_history_buffer_back_index
      = ptr + (h->z_history_buffer_back_index - h->z_history_buffer_start);

    h->z_history_buffer_start
      = ptr;
  }
}


// This method helps to ensure that we have at least one entry for each
// metadata type which is required to display on the screen (style, font
// and color) every Z_HISTORY_METADATA_STATE_BLOCK_SIZE bytes. In case
// we wouldn't be doing this, it would mean that games which never write a
// certain metadata type (games without color support would for example never
// write color metadata) we'd have to walk back the entire buffer to be able
// to evaluate the color of the current char we're pointing at.
static void write_metadata_state_block_if_necessary(OUTPUTHISTORY *h)
{
  long int metadata_block_index;
  long int buffer_index
    = h->z_history_buffer_front_index - h->z_history_buffer_start;

  metadata_block_index
    = buffer_index - (buffer_index % (Z_HISTORY_METADATA_STATE_BLOCK_SIZE));

  TRACE_LOG("block_index %ld(%ld), last block %ld.\n",
      metadata_block_index, buffer_index, h->last_metadata_block_index);

  if (metadata_block_index != h->last_metadata_block_index) {
    // We've now crossed a metadata block state boundary and thus we'll now
    // write the current state block.

    TRACE_LOG("Storing metadata state block %ld.\n", metadata_block_index);

    store_metadata_in_history(h, HISTORY_METADATA_TYPE_FONT,
        h->history_buffer_back_index_font);

    store_metadata_in_history(h, HISTORY_METADATA_TYPE_STYLE,
        h->history_buffer_back_index_style);

    store_metadata_in_history(h, HISTORY_METADATA_TYPE_COLOUR,
        h->history_buffer_back_index_foreground,
        h->history_buffer_back_index_background);
  }

  h->last_metadata_block_index = metadata_block_index;
}


void store_data_in_history(OUTPUTHISTORY *h, z_ucs *data, size_t len,
    bool evaluate_state_block)
{
  size_t space_available, missing_space, nof_increments, new_size;
  size_t desired_size, len_to_write;

  if (data == NULL)
    return;

  TRACE_LOG("Trying to store %ld z_ucs-chars in history.\n", (long int)len);
  /*  Not usable, since data doesn't have to be null-terminated.
  TRACE_LOG("store_history: \"");
  TRACE_LOG_Z_UCS(data);
  TRACE_LOG("\".\n");
  */

  if (len >= h->z_history_maximum_buffer_size)
  {
    // In this case the input to store is so large -- or our maximum buffer
    // size so tiny -- that the whole input will allocate all available
    // space.

    // Before we're trying to store any new data, we apply all metadata
    // changes to our buffer back-index.
    TRACE_LOG("buffer space used: %ld.\n",
        (long int)get_buffer_space_used(h));
    process_buffer_back(
        h,
        get_buffer_space_used(h));

    // In case the buffer has not yet been extended to it's full allowed
    // size, try to do so.
    if (h->z_history_buffer_size < h->z_history_maximum_buffer_size)
      try_to_enlarge_buffer(h, h->z_history_maximum_buffer_size);

    TRACE_LOG("Doing single-block-store to %p.\n", h->z_history_buffer_start);

    // The buffer is now as large as possible. We'll now copy as much into
    // it as will fit.
    memcpy(
        h->z_history_buffer_start,
        data,
        len - h->z_history_maximum_buffer_size);

    h->z_history_buffer_front_index
      = h->z_history_buffer_start;

    h->z_history_buffer_back_index
      = h->z_history_buffer_end;

    // At this point, we're already done.
  }
  else
  {
    // In case the input we've received is smaller than the maximum allowed
    // size of the history buffer, we can process the input in the regular
    // manner.

    if ((space_available = get_buffer_space_available(h)) < len)
    {
      // Currently there's not enough space available to store all of the
      // input, so we'll try to enlarge the buffer.

      missing_space = len - space_available;

      nof_increments = (missing_space / h->z_history_buffer_increment_size) + 1;

      new_size
        = h->z_history_buffer_size
        + (nof_increments * h->z_history_buffer_increment_size);

      TRACE_LOG("new calculated history size %ld z_ucs, max: %ld.\n",
          (long int)new_size,
          (long int)h->z_history_maximum_buffer_size);

      desired_size
        = new_size > h->z_history_maximum_buffer_size
        ? h->z_history_maximum_buffer_size
        : new_size;

      if (desired_size > h->z_history_buffer_size)
        try_to_enlarge_buffer(h, desired_size);
    }

    if (h->z_history_buffer_size < len)
    {
      // We couldn't allocate enough space to store the whole input. Thus,
      // we'll store as much as will currently fit.
      data += len - h->z_history_buffer_size;
      len = h->z_history_buffer_size;
    }

    TRACE_LOG("Adjusted len: %ld.\n", (long int)len);

    TRACE_LOG("Space in history: %ld z_ucs, buffer size: %ld z_ucs.\n",
        (long int)get_buffer_space_available(h),
        (long int)h->z_history_buffer_size);

    if (h->nof_wraparounds == 0)
    {
      TRACE_LOG("Not in wrap-around mode.\n");

      // We're not in a wrap-around situation and thus have space until the
      // end of the buffer.
      space_available
        = h->z_history_buffer_end - h->z_history_buffer_front_index + 1;

      len_to_write
        = space_available > len
        ? len
        : space_available;

      if (len_to_write > 0)
      {
        TRACE_LOG("Writing %ld z_ucs chars to %p.\n",
            (long int)len_to_write,
            h->z_history_buffer_front_index);

        z_ucs_ncpy(h->z_history_buffer_front_index, data, len_to_write);
        h->z_history_buffer_front_index += len_to_write;
      }

      data += len_to_write;

      if (len_to_write == len)
      {
        /*
        TRACE_LOG("history:\n---\n");
        TRACE_LOG_Z_UCS(h->z_history_buffer_start);
        TRACE_LOG("|\n---\nhistory end.\n");
        */

        TRACE_LOG("history-start: %p, end: %p.\n",
            h->z_history_buffer_start,
            h->z_history_buffer_end);

        TRACE_LOG("history-frontindex: %p, backindex: %p.\n",
            h->z_history_buffer_front_index,
            h->z_history_buffer_back_index);

        if (evaluate_state_block == true)
          write_metadata_state_block_if_necessary(h);

        return;
      }

      TRACE_LOG("Entering wrap-around mode.\n");

      // We couldn't write everything into the buffer. Since we're not
      // in wrap-around yet, we'll start this now.
      if (++h->nof_wraparounds == 0)
        h->nof_wraparounds = 1;
      h->z_history_buffer_front_index = h->z_history_buffer_start;
    }

    // If we arrive at this point, it's either due to h->nof_wraparounds
    // was > 0 above, or since we were in h->nof_wraparounds == 0
    // and len was still > 0 when writing up to the end of the buffer.
    // In both cases, we now are in wrap-around situation and have to
    // "throw away" chars at the back of the buffer to make room for more.

    while (len > 0)
    {
      if (h->z_history_buffer_front_index + len - 1 > h->z_history_buffer_end)
        len_to_write
          = (h->z_history_buffer_end - h->z_history_buffer_front_index) + 1;
      else
        len_to_write = len;

      process_buffer_back(
          h,
          len_to_write);

      TRACE_LOG("Writing %ld z_ucs chars from %p to %p.\n",
          (long int)len_to_write,
          data,
          h->z_history_buffer_front_index);

      z_ucs_ncpy(h->z_history_buffer_front_index, data, len_to_write);

      h->z_history_buffer_front_index += len_to_write;
      if (h->z_history_buffer_front_index == h->z_history_buffer_end + 1)
        h->z_history_buffer_front_index = h->z_history_buffer_start;

      len -= len_to_write;
      data += len_to_write;
      h->z_history_buffer_back_index = h->z_history_buffer_front_index;
    }

    /*
    TRACE_LOG("history:\n---\n");
    TRACE_LOG_Z_UCS(h->z_history_buffer_start);
    TRACE_LOG("|\n---\nhistory end.\n");
    */
  }

  TRACE_LOG("history-start: %p, end: %p.\n",
      h->z_history_buffer_start,
      h->z_history_buffer_end);

  TRACE_LOG("history-frontindex: %p, backindex: %p.\n",
      h->z_history_buffer_front_index,
      h->z_history_buffer_back_index);

  if (evaluate_state_block == true)
    write_metadata_state_block_if_necessary(h);
}


void store_z_ucs_output_in_history(OUTPUTHISTORY *h, z_ucs *z_ucs_output)
{
  size_t len;
/*
#ifdef ENABLE_TRACING
  z_ucs *cl;
#endif // ENABLE_TRACING
*/

  if (z_ucs_output == NULL)
    return;

  if ((len = z_ucs_len(z_ucs_output)) == 0)
    return;

  store_data_in_history(h, z_ucs_output, len, true);

/*
#ifdef ENABLE_TRACING
  cl = get_current_line(h);
  TRACE_LOG("currentline:|");
  if (cl != NULL)
  {
    TRACE_LOG_Z_UCS(cl);
    free(cl);
  }
  TRACE_LOG("|line end.\n");
#endif // ENABLE_TRACING
  */
}


// All "..." values must be int16_t.
int store_metadata_in_history(OUTPUTHISTORY *h, int metadata_type, ...)
{
  z_ucs output_buffer[4];
  va_list ap;
  int16_t parameter;
  size_t len;

  if (
      (metadata_type != HISTORY_METADATA_TYPE_FONT)
      &&
      (metadata_type != HISTORY_METADATA_TYPE_STYLE)
      &&
      (metadata_type != HISTORY_METADATA_TYPE_COLOUR)
      &&
      (metadata_type != HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE)
     )
    return -1;

  TRACE_LOG("Storing metadata type %d.\n", metadata_type);

  output_buffer[0] = HISTORY_METADATA_ESCAPE;
  output_buffer[1] = metadata_type;

  va_start(ap, metadata_type);

  // Compiler warning:
  // warning: ‘int16_t’ is promoted to ‘int’ when passed through ‘...’
  // (so you should pass ‘int’ not ‘int16_t’ to ‘va_arg’)
  parameter = va_arg(ap, int);

  // TODO: Add verifications for other metadata-types.
  if ( (metadata_type == HISTORY_METADATA_TYPE_COLOUR)
           && ( (parameter < -2) || (parameter > 15 ) ) )   {
    // -2 is the lowest allowed value for Z_COLOUR_UNDEFINED, 15 the maximum
    // for all combinations of Z_STYLE.
    TRACE_LOG("Parameter value %d outside valid range.\n", parameter);
    i18n_translate_and_exit(
        libfizmo_module_name,
        i18n_libfizmo_INVALID_PARAMETER_TYPE_P0S,
        -1,
        "parameter");
  }

  if (metadata_type == HISTORY_METADATA_TYPE_FONT)
  {
    h->history_buffer_front_index_font = parameter;
    TRACE_LOG("storing font.\n");
  }
  else if (metadata_type == HISTORY_METADATA_TYPE_STYLE)
  {
    h->history_buffer_front_index_style = parameter;
    TRACE_LOG("storing style.\n");
  }
  else if (metadata_type == HISTORY_METADATA_TYPE_COLOUR)
  {
    h->history_buffer_front_index_foreground = parameter;
    TRACE_LOG("storing colour.\n");
  }
  else if (metadata_type == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE)
  {
    TRACE_LOG("storing paragraph attribute.\n");
  }

  // All parameter values are offset by +13. This is necessary to avoid
  // having LF characters in the buffer, which makes searching for paragraph
  // starts much simpler.
  output_buffer[2] = (z_ucs)(parameter + HISTORY_METADATA_DATA_OFFSET);
  TRACE_LOG("param1: %d.\n", parameter);

  if ( (metadata_type == HISTORY_METADATA_TYPE_COLOUR)
          || (metadata_type == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE) ) {
    // Read second parameter for all metadatatypes which require it.
    parameter = va_arg(ap, int);
    TRACE_LOG("param2: %d.\n", parameter);
    if ( (metadata_type == HISTORY_METADATA_TYPE_COLOUR)
          && ( (parameter < -2) || (parameter > 15 ) ) ) {
      TRACE_LOG("Parameter value %d outside valid range.\n", parameter);
      i18n_translate_and_exit(
          libfizmo_module_name,
          i18n_libfizmo_INVALID_PARAMETER_TYPE_P0S,
          -1,
          "parameter");
      h->history_buffer_front_index_background = parameter;
    }
    output_buffer[3] = (z_ucs)(parameter + HISTORY_METADATA_DATA_OFFSET);
    len = 4;
  }
  else
  {
    len = 3;
  }

  va_end(ap);

  store_data_in_history(h, output_buffer, len, false);

  return 0;
}


/*
void repeat_paragraphs_from_history(OUTPUTHISTORY *history,
    int start_paragraph_number, int end_paragraph_number,
    bool allow_adjust_start_paragraph,
    struct history_repetition_target *target)
{
}


int get_paragraph_y_positions(OUTPUTHISTORY *history, int screen_width,
    int bottom_y, int *bottom_y_pos, int top_y, int *top_y_pos,
    int left_padding);
*/


// This function will really only decrement the pointer. "Only" means that
// even after a successful decrement the pointer is not guaraneteed to
// point at text, it may also point at the end of a metadata-entry.
z_ucs *decrement_buffer_pointer(OUTPUTHISTORY *h, z_ucs *ptr,
    unsigned int *nof_wraparounds)
{
  if (
      (ptr == h->z_history_buffer_back_index)
      && (ptr == h->z_history_buffer_front_index)
      && (*nof_wraparounds > 0)) {
    TRACE_LOG("History index already at buffer back.\n");
    return NULL;
  }

  ptr--;

  if (ptr < h->z_history_buffer_start) {
    if (h->nof_wraparounds == 0) {
      TRACE_LOG("History index at front of non-wrapped buffer.\n");
      return NULL;
    }
    else {
      TRACE_LOG("History index at front, wrapping around.\n");
      ptr = h->z_history_buffer_end;
      (*nof_wraparounds)--;
    }
  }

  return ptr;
}


// Used to remove preloaded input:
int remove_chars_from_history(OUTPUTHISTORY *history, int nof_chars)
{
  z_ucs *ptr = history->z_history_buffer_front_index;
  unsigned int nof_wraparounds = history->nof_wraparounds;
  z_ucs last_data = 0;

  TRACE_LOG("Removing %d chars from history at %p.\n", nof_chars, ptr);

  while (nof_chars > 0)
  {
    if ((ptr = decrement_buffer_pointer(
            history, ptr, &nof_wraparounds)) == NULL)
      // Can't rewind any more. Don't change current pointer.
      return -1;

    if ( (*ptr == HISTORY_METADATA_ESCAPE) && (last_data != 0) )
    {
      nof_chars
        += ( (last_data == HISTORY_METADATA_TYPE_COLOUR)
            || (last_data == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE) )
        ? 4
        : 3;
    }
    else
    {
      last_data = *ptr;
      nof_chars --;
    }
  }

  history->z_history_buffer_front_index = ptr;
  history->nof_wraparounds = nof_wraparounds;

  TRACE_LOG("History went to %p.\n", ptr);

  return 0;
}


// Search from index in direction of the back index and return the position
// of the next encountered newline char.
static z_ucs* find_older_paragraph(OUTPUTHISTORY *h, z_ucs *index)
{
  unsigned int nof_wraparounds = h->nof_wraparounds;

  if ( (h == NULL) || (h->z_history_buffer_size == 0) )
    return NULL;

  while (*index != '\n')
    if ((index = decrement_buffer_pointer(h, index, &nof_wraparounds)) == NULL)
      return NULL;

  return index;
}


/*
// The "get_current_line" function is deprecated. The problem is that the
// conecpt of getting the current line/prompt in a single string cannot
// work once metadata is encountered.
//
z_ucs *get_current_line(OUTPUTHISTORY *h)
{
  z_ucs *last_stored_char_index, *last_newline_index, *result;
  size_t len, backlen=0, frontlen=0;
  bool wrap_encountered;
  unsigned int nof_wraparounds = 0;

  // Since the front_index points to the position where the next(!) character
  // should be written to, we've got to step back by one char position.
  if ((last_stored_char_index = decrement_buffer_pointer(
          h,
          h->z_history_buffer_front_index,
          &nof_wraparounds)) == NULL)
  {
    TRACE_LOG("no current line: can't dec ptr.\n");
    return NULL;
  }

  last_newline_index = find_older_paragraph(h, last_stored_char_index);

  if (last_newline_index == NULL)
  {
    TRACE_LOG("already at last paragraph.\n");
    return NULL;
  }

  wrap_encountered
    = last_newline_index > h->z_history_buffer_front_index
    ? true
    : false;

  if (wrap_encountered == false)
    len = h->z_history_buffer_front_index - last_newline_index;
  else
  {
    backlen = h->z_history_buffer_end - last_newline_index;
    frontlen = h->z_history_buffer_front_index - h->z_history_buffer_start + 1;
    len = backlen + frontlen;
  }

  TRACE_LOG("cl-len: %ld (%ld + %ld), wrap: %d\n",
      (long int)len, (long int)backlen, (long int)frontlen, wrap_encountered);

  result = fizmo_malloc(sizeof(z_ucs) * (len + 1));

  if (wrap_encountered == false)
  {
    z_ucs_ncpy(result, last_newline_index, len);
  }
  else
  {
    z_ucs_ncpy(result, last_newline_index + 1, backlen);
    z_ucs_ncpy(result+backlen, h->z_history_buffer_start, frontlen);
  }

  result[len] = 0;

  return result;
}
*/


void destroy_history_output(history_output *output)
{
  free(output);
}


// The OUTPUTHISTORY object will ony be valid as long as nothing new is
// stored in the history while using it.
history_output *init_history_output(OUTPUTHISTORY *h, history_output_target *t,
    int output_init_flags)
{
  history_output *result;

  if ( (h == NULL) || (h->z_history_buffer_size == 0) ) {
    return NULL;
  }

  result = fizmo_malloc(sizeof(history_output));

  result->history = h;
  result->validity_wraparounds = h->nof_wraparounds;
  result->validity_frontindex = h->z_history_buffer_front_index;
  result->target = t;
  result->rewound_paragraph_was_newline_terminated = false;
  result->validation_disabled
    = (output_init_flags & Z_HISTORY_OUTPUT_WITHOUT_VALIDATION) == 0
    ? false : true;
  result->last_rewinded_paragraphs_block_index = -1;
  result->last_used_metadata_state_font = -1;
  result->last_used_metadata_state_style = -1;
  result->last_used_metadata_state_foreground = Z_COLOUR_UNDEFINED;
  result->last_used_metadata_state_background = Z_COLOUR_UNDEFINED;
  result->last_paragraph_attribute_index = NULL;
  result->dont_skip_newline = false;

  if ((output_init_flags & Z_HISTORY_OUTPUT_FROM_BUFFERBACK) == 0) {
    TRACE_LOG("Init from buffer front.\n");
    result->current_paragraph_index = h->z_history_buffer_front_index;
    result->font_at_index = h->history_buffer_front_index_font;
    result->style_at_index = h->history_buffer_front_index_style;
    result->foreground_at_index = h->history_buffer_front_index_foreground;
    result->background_at_index = h->history_buffer_front_index_background;
    result->found_end_of_buffer = false;
    result->nof_wraparounds = 0;
    result->first_iteration_done = false;
 
    // Since "z_history_buffer_front_index" always points to the place where
    // the next char will be stored, we actually have to go back one char
    // in order to find the last paragraph's stored char.
    if ((result->current_paragraph_index = decrement_buffer_pointer(
            result->history,
            result->current_paragraph_index,
            &result->nof_wraparounds)) == NULL) {
      free(result);
      return NULL;
    }
  }
  else {
    TRACE_LOG("Init from buffer back.\n");
    result->current_paragraph_index = h->z_history_buffer_back_index;
    result->font_at_index = h->history_buffer_back_index_font;
    result->style_at_index = h->history_buffer_back_index_style;
    result->foreground_at_index = h->history_buffer_back_index_foreground;
    result->background_at_index = h->history_buffer_back_index_background;
    result->found_end_of_buffer = true;
    result->nof_wraparounds
      = h->nof_wraparounds > 0 ? h->nof_wraparounds - 1 : 0;
    result->first_iteration_done = true;
  }

  return result;
}


static void validate_outputhistory(history_output *output) {
  if ( (output != NULL)
      && (output->history->nof_wraparounds == output->validity_wraparounds)
      && (output->validity_frontindex
        == output->history->z_history_buffer_front_index) )
    return;
  else
    i18n_translate_and_exit(
        libfizmo_module_name,
        i18n_libfizmo_HISTORYOUTPUT_NO_LONGER_VALID,
        -1);
}


static void evaluate_metadata_for_paragraph(history_output *output)
{
  OUTPUTHISTORY *h = output->history;
  long int metadata_block_index;
  unsigned int nof_wraparounds;
  long int buffer_index
    = output->current_paragraph_index - h->z_history_buffer_start;
  z_ucs *index = output->current_paragraph_index;
  z_ucs *i2 = NULL, *i3 = NULL, *i4 = NULL;
  int metadata_type, parameter;

  if (output->validation_disabled == false) {
    validate_outputhistory(output);
  }

  TRACE_LOG("Evaluating metadata for current paragraph from %p.\n",
      output->current_paragraph_index);

  if (output->metadata_at_index_evaluated == true)
  {
    TRACE_LOG("Already evaluated, returning.\n");
    return;
  }

  metadata_block_index
    = buffer_index - (buffer_index % (Z_HISTORY_METADATA_STATE_BLOCK_SIZE));

  TRACE_LOG("metadata_block_index: %ld.\n", metadata_block_index);

  if (
      (output->last_rewinded_paragraphs_block_index == metadata_block_index)
      &&
      (output->last_used_metadata_state_font != -1)
      &&
      (output->last_used_metadata_state_style != -1)
      &&
      (output->last_used_metadata_state_foreground != Z_COLOUR_UNDEFINED)
      &&
      (output->last_used_metadata_state_background != -Z_COLOUR_UNDEFINED)
     )
  {
    TRACE_LOG("Re-using metadata state block.\n");
    output->font_at_index = output->last_used_metadata_state_font;
    output->style_at_index = output->last_used_metadata_state_style;
    TRACE_LOG("sai: #1\n");
    output->foreground_at_index = output->last_used_metadata_state_foreground;
    output->background_at_index = output->last_used_metadata_state_background;
  }
  else
  {
    TRACE_LOG("Searching for metadata.\n");

    output->font_at_index = -1;
    output->style_at_index = -1;
    TRACE_LOG("sai: #2\n");
    output->foreground_at_index = Z_COLOUR_UNDEFINED;
    output->background_at_index = Z_COLOUR_UNDEFINED;

    nof_wraparounds = output->nof_wraparounds;

    while (
        (output->font_at_index == -1)
        ||
        (output->style_at_index == -1)
        ||
        (output->foreground_at_index == Z_COLOUR_UNDEFINED)
        ||
        (output->background_at_index == Z_COLOUR_UNDEFINED)
        )
    {
      TRACE_LOG("search-ptr: %p (%d, %d, %d, %d).\n",
          index,
          output->font_at_index,
          output->style_at_index,
          output->foreground_at_index,
          output->background_at_index);

      i4 = i3;
      i3 = i2;
      i2 = index;
      if ((index = decrement_buffer_pointer(
              output->history,
              index,
              &nof_wraparounds)) == NULL)
      {
        TRACE_LOG("Hit end of buffer. Using back values to fill in.\n");

        if (output->font_at_index == -1) {
          output->font_at_index = h->history_buffer_back_index_font;
        }

        if (output->style_at_index == -1) {
          output->style_at_index = h->history_buffer_back_index_style;
          TRACE_LOG("sai: #3\n");
        }

        if (output->foreground_at_index == Z_COLOUR_UNDEFINED) {
          output->foreground_at_index=h->history_buffer_front_index_foreground;
        }

        if (output->background_at_index == Z_COLOUR_UNDEFINED) {
          output->background_at_index=h->history_buffer_front_index_background;
        }

        break;
      }
      else if (*index == HISTORY_METADATA_ESCAPE)
      {
        TRACE_LOG("Metadata found at %p.\n", index);

        metadata_type = *i2;
        parameter = (int)*i3 - HISTORY_METADATA_DATA_OFFSET;

        if (
            (metadata_type == HISTORY_METADATA_TYPE_FONT)
            &&
            (output->font_at_index == -1)
           )
          output->font_at_index = parameter;
        else if (
            (metadata_type == HISTORY_METADATA_TYPE_STYLE)
            &&
            (output->style_at_index == -1)
            )
          {
            output->style_at_index = parameter;
            TRACE_LOG("sai: #5\n");
          }
        else if (
            (metadata_type == HISTORY_METADATA_TYPE_COLOUR)
            &&
            (
             (output->foreground_at_index == Z_COLOUR_UNDEFINED)
             ||
             (output->background_at_index == Z_COLOUR_UNDEFINED)
            )
            )
        {
          output->foreground_at_index = parameter;
          parameter = (int)*i4- HISTORY_METADATA_DATA_OFFSET;
          output->background_at_index = parameter;
        }
      }
    }
  }

  TRACE_LOG("Done: %d, %d, %d, %d.\n",
      output->font_at_index,
      output->style_at_index,
      output->foreground_at_index,
      output->background_at_index);

  output->metadata_at_index_evaluated = true;
}


// After executing this function, the current paragraph index should always
// point to the first char of the last paragraph -- not the newline before --
// or the buffer start. In case a previous paragraph could be found the return
// value is 0, in case the buffer back was encountered 1 and a negative value
// in case of an error. In case char_count is non-null, the number of
// non-metadata chars in this paragraph is stored at this reference.
int output_rewind_paragraph(history_output *output, long *char_count,
    int *paragraph_attr1, int *paragraph_attr2)
{
  z_ucs *index, *last_index, *last_index2, *last_index3;
  int nof_chars = 0;
  unsigned int nof_wraparounds, last_nof_wraparounds;

  TRACE_LOG("Rewinding output history by one paragraph from %p.\n",
      output->current_paragraph_index);

  if (output->validation_disabled == false) {
    validate_outputhistory(output);
  }

  if ( (output == NULL)
      || (output->history == NULL)
      || (output->history->z_history_buffer_size == 0) )
    return -1;

  TRACE_LOG("found_end_of_buffer: %d.\n", output->found_end_of_buffer);
  if (output->found_end_of_buffer == true)
    return 1;

  // We're not changing the index directly in case we're hitting a non-full
  // paragraph at the front. Instead, we're working on local variables and
  // only modify the history index in case we can find a newline.
  index = output->current_paragraph_index;
  nof_wraparounds = output->nof_wraparounds;

  // Rewind to last paragraph's newline since the index should always
  // point to the first char of a paragraph. We only have to do this if
  // we're not at the buffer end and have already finished at least the
  // first rewind iteration.
  if (output->first_iteration_done == true) {
    TRACE_LOG("Skipping over last paragraph's newline.\n");
    // In case we're not at the end of the history, paragraphs are
    // always newline-terminated.
    output->rewound_paragraph_was_newline_terminated = true;

    // It's possible that we're already on the newline in case the very
    // first paragraph didn't contain anything but a newline, so we'll
    // only rewind further in case we're not yet there.
    if (output->dont_skip_newline == false) {
      TRACE_LOG("Rewinding from %p.\n", index);
      if ((index = decrement_buffer_pointer(
              output->history,
              index,
              &nof_wraparounds)) == NULL) {
        TRACE_LOG("Couldn't execute initial index decrement.\n");
        return -3;
      }
    }
    else {
      output->dont_skip_newline = false;
    }

    TRACE_LOG("Looking at %p.\n", index);
    if (*index != '\n') {
      TRACE_LOG("Internal error rewinding.\n");
      return -4;
    }

    last_index = index;
    last_nof_wraparounds = nof_wraparounds;

    // Rewind to last paragraph's last content char.
    if ((index = decrement_buffer_pointer(
            output->history,
            index,
            &nof_wraparounds)) == NULL) {
      // Here we've hit the start of the buffer, so this empty paragraph
      // (since we've just skipped the newline above) will be the last
      // we can deliver.
      TRACE_LOG("Couldn't execute second stop of inital index decrement.\n");
      output->found_end_of_buffer = true;
      TRACE_LOG("found_end_of_buffer: %d.\n", output->found_end_of_buffer);
      //output->current_paragraph_index = index;
      //output->nof_wraparounds = nof_wraparounds;
      output->current_paragraph_index = last_index;
      output->nof_wraparounds = last_nof_wraparounds;
      return 0;
    }
    else if (*index == '\n') {
      // In case the next paragraph is empty, quit right away.
      output->current_paragraph_index = last_index;
      output->nof_wraparounds = last_nof_wraparounds;
      return 0;
    }
  }
  else {
    if (*index == '\n') {
      output->dont_skip_newline = true;
      TRACE_LOG("Last output char is newline, returning from 1st iteration.\n");
      output->first_iteration_done = true;
      output->metadata_at_index_evaluated = false;
      if (char_count != NULL) {
        *char_count = 0;
      }
      //evaluate_metadata_for_paragraph(output);
      output->rewound_paragraph_was_newline_terminated = true;
      return 0;
    }
    else {
      output->dont_skip_newline = false;
      output->rewound_paragraph_was_newline_terminated = false;
    }

    //last_index = index;
  }

  output->first_iteration_done = true;

  TRACE_LOG("Index pointing at '%c' / %p.\n", *index, index);

  // The index is now pointing at the last char of the paragraph we want
  // to rewind over. We're rewinding until we find the newline or the
  // buffer start. In the latter case we've got a non-full paragraph which
  // we won't return.
  last_index = NULL;
  last_index2 = NULL;
  do {
    // In this loop we're remembering the last index position for two
    // reasons: One to be able to read the last z_ucs we've iterated over
    // for easier metadata evaluation (see below), second to make
    // skipping the newline we might find a bit easier. We're also
    // remebering the second- and third-last-index for paragraph attribute
    // envaluation.
    last_index3 = last_index2;
    last_index2 = last_index;
    last_index = index;
    last_nof_wraparounds = nof_wraparounds;

    if ((index = decrement_buffer_pointer(
            output->history,
            index,
            &nof_wraparounds)) == NULL) {
      // In case we can't move back any more we've hit the buffer start.
      TRACE_LOG("Couldn't decrement history index.\n");
      output->found_end_of_buffer = true;
      return 1;

      /*
         if (output->current_paragraph_index != last_index) {
         output->current_paragraph_index = last_index;
         output->metadata_at_index_evaluated = false;
      //output->found_end_of_buffer = true;
      //evaluate_metadata_for_paragraph(output);
      if (char_count != NULL) {
       *char_count = nof_chars;
       }
       return 0;
       }
       else
       return -1;
       */
    }

    nof_chars++;

    if (*index == HISTORY_METADATA_ESCAPE) {
      if (*last_index == HISTORY_METADATA_TYPE_COLOUR) {
        nof_chars -= 4;
      }
      else if (*last_index == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE) {

        if (paragraph_attr1 != NULL) {
          *paragraph_attr1 = *last_index2 - HISTORY_METADATA_DATA_OFFSET;
        }

        if (paragraph_attr2 != NULL) {
          *paragraph_attr2 = *last_index3 - HISTORY_METADATA_DATA_OFFSET;
        }

        nof_chars -= 4;
      }
      else {
        nof_chars -= 3;
      }
    }

    TRACE_LOG("Index pointing at '%c' / %p.\n", *index, index);
  }
  while (*index != '\n');

  output->current_paragraph_index = last_index;
  output->nof_wraparounds = last_nof_wraparounds;
  output->metadata_at_index_evaluated = false;
  evaluate_metadata_for_paragraph(output);

  if (char_count != NULL) {
    *char_count = nof_chars;
  }

  return 0;

  /*
  // We'll only consider "full" paragraphs when reading the history, which
  // means that in case the buffer front is not on a newline, we'll just
  // return 1.
  if (*index == '\n') {
    // paragraph found
    TRACE_LOG("Start of new paragraph found at %p.\n", last_index);
    output->current_paragraph_index = last_index;
    return 0;
  }
  else {
    // start of buffer found
    TRACE_LOG("Buffer front encountered at %p.\n", index);
    output->current_paragraph_index = index;
    output->found_end_of_buffer = true;
    return 1;
  }
  */
}


int alter_last_paragraph_attributes(history_output *output,
    int paragraph_attr1, int paragraph_attr2) {
  z_ucs *index;

  validate_outputhistory(output);

  if (output->last_paragraph_attribute_index == NULL) {
    TRACE_LOG("Not altering paragraph attributes, pointer is NULL.\n");
    return -1;
  }

  TRACE_LOG("Altering paragraph attributes to %d and %d.\n",
      paragraph_attr1, paragraph_attr2);

  index = output->last_paragraph_attribute_index;

  *index = (z_ucs)(paragraph_attr1 + HISTORY_METADATA_DATA_OFFSET);
  if (++index > output->history->z_history_buffer_end) {
    index = output->history->z_history_buffer_start;
  }

  *index = (z_ucs)(paragraph_attr2 + HISTORY_METADATA_DATA_OFFSET);

  return 0;
}


bool is_output_at_frontindex(history_output *output) {

  if (output->validation_disabled == false) {
    validate_outputhistory(output);
  }

  if (output->current_paragraph_index
      == output->history->z_history_buffer_front_index) {
    return true;
  }
  else {
    return false;
  }
}


int output_repeat_paragraphs(history_output *output, int n,
    bool include_metadata, bool advance_history_pointer)
{
  z_ucs output_buf[REPEAT_PARAGRAPH_BUF_SIZE];
  z_ucs *output_ptr = output->current_paragraph_index;
  int buf_index;
  int metadata_type = -1, parameter, parameter2;

  if (output->validation_disabled == false) {
    validate_outputhistory(output);
  }

  if (include_metadata == true)
    evaluate_metadata_for_paragraph(output);

  TRACE_LOG("Repeating output history from %p.\n", output_ptr);

  /*
  // If we're already at the front index, quit.
  if (output_ptr == output->history->z_history_buffer_front_index)
    return 0;
    */

  output->target->set_font(
      output->font_at_index);

  output->target->set_text_style(
      output->style_at_index);

  output->target->set_colour(
      output->foreground_at_index,
      output->background_at_index,
      -1);

  if (advance_history_pointer == true)
    output->found_end_of_buffer = false;

  buf_index = 0;

  if (output_ptr == output->history->z_history_buffer_front_index) {
    TRACE_LOG("Already at buffer front.\n");
    n = -1;
  }
  else {
    while (n > 0)
    {
      TRACE_LOG("Looking at %p.\n", output_ptr);

      if (*output_ptr == '\n') {
        n--;
      }

      if ( (buf_index == REPEAT_PARAGRAPH_BUF_SIZE - 1)
          || (n < 1)
          || (*output_ptr == HISTORY_METADATA_ESCAPE)
          || (output_ptr == output->history->z_history_buffer_front_index) ) {
        output_buf[buf_index] = 0;
        TRACE_LOG("Sending %d char(s) of output.\n", buf_index);
        output->target->z_ucs_output(output_buf);

        if (output_ptr == output->history->z_history_buffer_front_index) {
          TRACE_LOG("Buffer front encountered.\n");
          break;
        }

        if (n < 1)
        {
          TRACE_LOG("n < 1.\n");
          break;
        }

        buf_index = 0;

        if (*output_ptr == HISTORY_METADATA_ESCAPE) {

          TRACE_LOG("Metadata found at %p in output.\n", output_ptr);

          if (++output_ptr > output->history->z_history_buffer_end)
            output_ptr = output->history->z_history_buffer_start;
          metadata_type = *output_ptr;
          if (++output_ptr > output->history->z_history_buffer_end)
            output_ptr = output->history->z_history_buffer_start;
          parameter = (int)*output_ptr - HISTORY_METADATA_DATA_OFFSET;

          if (metadata_type == HISTORY_METADATA_TYPE_FONT)
          {
            output->font_at_index = parameter;
            if (include_metadata == true)
              output->target->set_font(parameter);
          }
          else if (metadata_type == HISTORY_METADATA_TYPE_STYLE)
          {
            output->style_at_index = parameter;
            if (include_metadata == true)
              output->target->set_text_style(parameter);
          }
          else if (metadata_type == HISTORY_METADATA_TYPE_COLOUR)
          {
            if (++output_ptr > output->history->z_history_buffer_end)
              output_ptr = output->history->z_history_buffer_start;
            parameter2 = (int)*output_ptr - HISTORY_METADATA_DATA_OFFSET;
            output->foreground_at_index = parameter;
            output->background_at_index = parameter2;
            if (include_metadata == true)
              output->target->set_colour(parameter, parameter2, -1);
          }
          else if (metadata_type == HISTORY_METADATA_TYPE_PARAGRAPHATTRIBUTE)
          {
            output->last_paragraph_attribute_index = output_ptr;

            // Don't do anything but catch the case in order that we're
            // not running into the error-else below.
            if (++output_ptr > output->history->z_history_buffer_end)
              output_ptr = output->history->z_history_buffer_start;
            //parameter2 = (int)*output_ptr - HISTORY_METADATA_DATA_OFFSET;
          }
          else
          {
            TRACE_LOG("Invalid metadata type %d\n", metadata_type);
            i18n_translate_and_exit(
                libfizmo_module_name,
                i18n_libfizmo_INVALID_PARAMETER_TYPE_P0S,
                -1,
                "metadata");
          }
        }
      }

      if (metadata_type == -1) {
        output_buf[buf_index++] = *output_ptr;
      }
      else {
        metadata_type = -1;
      }

      if ((++output_ptr) > output->history->z_history_buffer_end)
        output_ptr = output->history->z_history_buffer_start;
    }
  }

  TRACE_LOG("n: %d.\n", n);

  if (advance_history_pointer == true)
  {
    output->current_paragraph_index = output_ptr;
    if (output->current_paragraph_index
        != output->history->z_history_buffer_front_index) {
      output->current_paragraph_index += 1;
    }
    else {
      output->first_iteration_done = false;
      if (*(output->current_paragraph_index) == '\n') {
        output->rewound_paragraph_was_newline_terminated = true;
      }
      else {
        output->rewound_paragraph_was_newline_terminated = false;
      }

      output->first_iteration_done = false;

      TRACE_LOG("first_iteration_done: %d.\n", output->first_iteration_done);
    }

    // There might be more metadata blocks after this newline. These
    // also have to be evaluated if the metadata should be correct
    // after advancing the pointer.

    if (*output_ptr == HISTORY_METADATA_ESCAPE)
    {
      TRACE_LOG("Found metadata-escape.\n");
    }
  }

  TRACE_LOG("Repeated output, last included output char: %p.\n",
      output_ptr);

  return n;
}


void remember_history_output_position(history_output *output)
{
  if (output->validation_disabled == false) {
    validate_outputhistory(output);
  }

  output->saved_current_paragraph_index =
    output->current_paragraph_index;
  output->saved_nof_wraparounds =
    output->nof_wraparounds;
  output->saved_found_end_of_buffer =
    output->found_end_of_buffer;
  output->saved_first_iteration_done =
    output->first_iteration_done;
  output->saved_rewound_paragraph_was_newline_terminated =
    output->rewound_paragraph_was_newline_terminated;
  output->saved_metadata_at_index_evaluated =
    output->metadata_at_index_evaluated;
  output->saved_font_at_index =
    output->font_at_index;
  output->saved_style_at_index =
    output->style_at_index;
  output->saved_foreground_at_index =
    output->foreground_at_index;
  output->saved_background_at_index =
    output->background_at_index;
  output->saved_last_rewinded_paragraphs_block_index =
    output->last_rewinded_paragraphs_block_index;
  output->saved_last_used_metadata_state_font =
    output->last_used_metadata_state_font;
  output->saved_last_used_metadata_state_style =
    output->last_used_metadata_state_style;
  output->saved_last_used_metadata_state_foreground =
    output->last_used_metadata_state_foreground;
  output->saved_last_used_metadata_state_background =
    output->last_used_metadata_state_background;
}


void restore_history_output_position(history_output *output)
{
  if (output->validation_disabled == false) {
    validate_outputhistory(output);
  }

  output->current_paragraph_index =
    output->saved_current_paragraph_index;
  output->nof_wraparounds =
    output->nof_wraparounds;
  output->found_end_of_buffer =
    output->saved_found_end_of_buffer;
  output->rewound_paragraph_was_newline_terminated =
    output->saved_rewound_paragraph_was_newline_terminated;
  output->first_iteration_done =
    output->saved_first_iteration_done;
  output->metadata_at_index_evaluated =
    output->saved_metadata_at_index_evaluated;
  output->font_at_index =
    output->saved_font_at_index;
  output->style_at_index =
    output->saved_style_at_index;
  TRACE_LOG("sai: #4\n");
  output->foreground_at_index =
    output->saved_foreground_at_index;
  output->background_at_index =
    output->saved_background_at_index;
  output->last_rewinded_paragraphs_block_index =
    output->saved_last_rewinded_paragraphs_block_index;
  output->last_used_metadata_state_font =
    output->saved_last_used_metadata_state_font;
  output->last_used_metadata_state_style =
    output->saved_last_used_metadata_state_style;
  output->last_used_metadata_state_foreground =
    output->saved_last_used_metadata_state_foreground;
  output->last_used_metadata_state_background =
    output->saved_last_used_metadata_state_background;
}


size_t get_allocated_text_history_size(OUTPUTHISTORY *h)
{
  return h->z_history_buffer_size;
}

#endif /* history_c_INCLUDED */

