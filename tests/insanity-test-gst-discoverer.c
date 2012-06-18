/* Insanity QA system

 Copyright (c) 2012, Collabora Ltd
 Author: Vivia Nikolaidou <vivia.nikolaidou@collabora.co.uk>

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU Lesser General Public
 License as published by the Free Software Foundation; either
 version 2.1 of the License, or (at your option) any later version.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 Lesser General Public License for more details.

 You should have received a copy of the GNU Lesser General Public
 License along with this program; if not, write to the
 Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 Boston, MA 02111-1307, USA.
*/

#include <stdio.h>
#include <glib.h>
#include <glib-object.h>
#include <gst/gst.h>
#include <gst/pbutils/pbutils.h>
#include <stdlib.h>
#include <insanity-gst/insanity-gst.h>

static GstDiscoverer *dc;
static gint timeout = 10;
static GMainLoop *ml;
static GstDiscovererInfo *info;
static gboolean async = TRUE;
static gboolean skip_compare = FALSE;
static InsanityTest *gstest;
static gchar *uri;
static gchar **lines;

typedef struct
{
  enum Types
  { TYPE_CONTAINER, TYPE_AUDIO, TYPE_VIDEO, TYPE_SUBTITLES, TYPE_UNKNOWN } type;

  GstCaps *caps;
  GstTagList *tags;
  GstStructure *additional_info;
  gint channels;
  gint sample_rate;
  gint depth;
  gint bitrate;
  gint max_bitrate;
  gint width;
  gint height;
  gint framerate_num;
  gint framerate_denom;
  gint aspectratio_num;
  gint aspectratio_denom;
  gboolean interlaced;
  gchar *language;
  GList *contained_topologies;
  gchar *unknown;
  gboolean checked;

} Topology;

typedef struct
{
  GstClockTime duration;
  gboolean seekable;
  GHashTable *tags;
} Properties;

static Topology *topology;
static Properties *properties;

static Topology *read_topology_wrapped (Topology * local_topology, int depth);

static Topology *
read_unknown (Topology * local_topology, int depth)
{
  gchar *line;
  int indent;
  Topology *bottomology;

  local_topology->type = TYPE_UNKNOWN;

  /*Just parse unknown fields and then return */
  while (*lines != NULL) {
    line = *lines;
    if (g_str_equal (line, "")) {
      /*Found empty line, get next */
      if (*lines != NULL)
        lines++;
      continue;
    }
    indent = 0;
    while (g_ascii_isspace (line[indent])) {
      indent++;
    }
    if (indent < depth) {
      /*Oops - this line belongs to the previous topology */
      lines--;
      return local_topology;
    } else {
      line += indent;
    }
    if (g_str_has_prefix (line, "audio") || g_str_has_prefix (line, "video")
        || g_str_has_prefix (line, "subtitles")
        || g_str_has_prefix (line, "unknown")) {
      /*We contain another topology */
      /* lines--; */
      bottomology = read_topology_wrapped (bottomology, depth);
      local_topology->contained_topologies = NULL;
      if (bottomology != NULL) {
        local_topology->contained_topologies =
            g_list_append (local_topology->contained_topologies, bottomology);
      } else {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE,
            "Found contained topology in unknown stream, unable to parse it\n");
        return NULL;
      }
    }
    /*if (local_topology->unknown == NULL) {
       local_topology->unknown = line;
       } else {
       local_topology->unknown = g_strconcat(local_topology->unknown, "\n", line, (char *)NULL); 
       } */
    if (*lines != NULL)
      lines++;
  }
  return local_topology;

}

static Topology *
read_video (Topology * local_topology, int depth)
{
  gchar *line;
  gchar **splitted, **splitted2, *endptr;
  int indent;
  GstTagList *tags;
  Topology *bottomology;

  local_topology->type = TYPE_VIDEO;
  splitted = NULL;
  splitted2 = NULL;

  while (*lines != NULL) {
    line = *lines;
    indent = 0;
    while (g_ascii_isspace (line[indent])) {
      indent++;
    }
    if (indent < depth) {
      /*Oops - this line belongs to the previous topology */
      lines--;
      return local_topology;
    } else {
      line += indent;
    }
    if (g_str_equal (line, "")) {
      /*Found empty line, get next */
      if (*lines != NULL)
        lines++;
      continue;
    }
    /*Line should now be trimmed. Find the label */
    splitted = g_strsplit (line, ": ", 2);
    if (g_str_has_prefix (splitted[0], "Codec")) {
      /* Ignore: duplicate */
      if (*lines != NULL)
        lines++;
      /*g_io_channel_read_line_string(gch, buffer, NULL, err);
         line = buffer->str;
         caps = gst_caps_from_string(line);
         bottomology = g_list_append(bottomology, splitted[0]);
         bottomology = g_list_append(bottomology, caps); */
    } else if (g_str_has_prefix (splitted[0], "Additional info")) {
      if (*lines != NULL)
        lines++;
      /* Ignore for now... */
      /*if (cur_line>=0) cur_line++;
         line = lines[cur_line];
         //Manually trim
         line += depth;
         local_topology->additional_info = gst_structure_from_string(line, NULL);  */
    } else if (g_str_has_prefix (splitted[0], "Tags")) {
      if (*lines != NULL)
        lines++;
      line = *lines;
      while (g_ascii_isspace (line[0])) {
        line++;
      }
      if (!g_str_has_prefix (line, "None")) {
        tags = gst_tag_list_new_from_string (line);
        if (tags == NULL) {
          insanity_test_validate_checklist_item (gstest,
              "comparison-file-parsed", FALSE, "Erroneous value in tags field");
          g_strfreev (splitted);
          return NULL;
        }
        local_topology->tags = tags;
      }
    } else if (g_str_has_prefix (splitted[0], "Width")) {
      local_topology->width = g_ascii_strtoll (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in width field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Height")) {
      local_topology->height = g_ascii_strtoll (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in height field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Depth")) {
      local_topology->depth = g_ascii_strtoll (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in depth field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Bitrate")) {
      local_topology->bitrate = g_ascii_strtoull (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in bitrate field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Max bitrate")) {
      local_topology->max_bitrate = g_ascii_strtoull (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in max bitrate field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Interlaced")) {
      if (g_str_has_prefix (splitted[1], "true")) {
        local_topology->interlaced = TRUE;
      } else if (g_str_has_prefix (splitted[1], "false")) {
        local_topology->interlaced = FALSE;
      } else {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in interlaced field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Frame rate")) {
      splitted2 = g_strsplit (splitted[1], "/", 2);
      local_topology->framerate_num =
          g_ascii_strtoull (splitted2[0], &endptr, 0);
      if (endptr == splitted2[0] && local_topology->framerate_num == 0) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in framerate field!");
        g_strfreev (splitted2);
        g_strfreev (splitted);
        return NULL;
      }
      local_topology->framerate_denom =
          g_ascii_strtoull (splitted2[1], &endptr, 0);
      if (local_topology->framerate_denom == 0) {       /* Shouldn't have a 0 denominator anyway!! */
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in framerate field!");
        g_strfreev (splitted2);
        g_strfreev (splitted);
        return NULL;
      }
      g_strfreev (splitted2);
    } else if (g_str_has_prefix (splitted[0], "Pixel aspect ratio")) {
      splitted2 = g_strsplit (splitted[1], "/", 2);
      local_topology->aspectratio_num =
          g_ascii_strtoull (splitted2[0], NULL, 0);
      if (local_topology->aspectratio_num == 0) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in aspect ratio field!");
        g_strfreev (splitted2);
        g_strfreev (splitted);
        return NULL;
      }
      local_topology->aspectratio_denom =
          g_ascii_strtoull (splitted2[1], NULL, 0);
      if (local_topology->aspectratio_denom == 0) {     /* Shouldn't have a 0 denominator anyway!! */
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in aspect ratio field!");
        g_strfreev (splitted2);
        g_strfreev (splitted);
        return NULL;
      }
      g_strfreev (splitted2);
    } else if (g_str_has_prefix (line, "audio")
        || g_str_has_prefix (line, "video")
        || g_str_has_prefix (line, "subtitles")
        || g_str_has_prefix (line, "unknown")) {
      /*We contain another topology */
      lines--;
      bottomology = read_topology_wrapped (bottomology, depth);
      local_topology->contained_topologies = NULL;
      if (bottomology != NULL)
        local_topology->contained_topologies =
            g_list_append (local_topology->contained_topologies, bottomology);
    } else {
      insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
          FALSE,
          "Found contained topology in video stream, unable to parse it\n");
      return NULL;
    }
    g_strfreev (splitted);
    if (*lines != NULL)
      lines++;
  }
  return local_topology;

}

static Topology *
read_audio (Topology * local_topology, int depth)
{
  gchar *line;
  gchar **splitted, *endptr;
  int indent;
  GstTagList *tags;
  Topology *bottomology;


  splitted = NULL;
  local_topology->type = TYPE_AUDIO;

  while (*lines != NULL) {
    line = *lines;
    indent = 0;
    while (g_ascii_isspace (line[indent])) {
      indent++;
    }
    if (indent < depth) {
      /*Oops - this line belongs to the previous topology */
      lines--;
      return local_topology;
    } else {
      line += indent;
    }
    if (g_str_equal (line, "")) {
      /*Found empty line, get next */
      if (*lines != NULL)
        lines++;
      continue;
    }
    /*Line should now be trimmed. Find the label */
    splitted = g_strsplit (line, ": ", 2);
    if (g_str_has_prefix (splitted[0], "Codec")) {
      if (*lines != NULL)
        lines++;
      /* Ignore: duplicate */
      /*g_io_channel_read_line_string(gch, buffer, NULL, err);
         line = buffer->str;
         caps = gst_caps_from_string(line);
         bottomology = g_list_append(bottomology, splitted[0]);
         bottomology = g_list_append(bottomology, caps); */
    } else if (g_str_has_prefix (splitted[0], "Additional info")) {
      if (*lines != NULL)
        lines++;
      /*line = lines[cur_line];
         //Manually trim
         line += depth;
         local_topology->additional_info = gst_structure_from_string(line, NULL); */
    } else if (g_str_has_prefix (splitted[0], "Tags")) {
      if (*lines != NULL)
        lines++;
      line = *lines;
      while (g_ascii_isspace (line[0])) {
        line++;
      }
      if (!g_str_has_prefix (line, "None")) {
        tags = gst_tag_list_new_from_string (line);
        if (tags == NULL) {
          insanity_test_validate_checklist_item (gstest,
              "comparison-file-parsed", FALSE, "Erroneous value in tags field");
          g_strfreev (splitted);
          return NULL;
        }
        local_topology->tags = tags;
      }
    } else if (g_str_has_prefix (splitted[0], "Language")) {
      local_topology->language =
          (g_str_has_prefix (splitted[1],
              "<unknown>")) ? NULL : g_strdup (splitted[1]);
    } else if (g_str_has_prefix (splitted[0], "Channels")) {
      local_topology->channels = g_ascii_strtoull (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        g_strfreev (splitted);
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in channels field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Sample rate")) {
      local_topology->sample_rate = g_ascii_strtoull (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in sample rate field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Depth")) {
      local_topology->depth = g_ascii_strtoll (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in depth field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Bitrate")) {
      local_topology->bitrate = g_ascii_strtoull (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        g_strfreev (splitted);
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in bitrate field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Max bitrate")) {
      local_topology->max_bitrate = g_ascii_strtoull (splitted[1], &endptr, 0);
      if (endptr == splitted[1]) {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in max bitrate field");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (line, "audio")
        || g_str_has_prefix (line, "video")
        || g_str_has_prefix (line, "subtitles")
        || g_str_has_prefix (line, "unknown")) {
      /*We contain another topology */
      lines--;
      bottomology = read_topology_wrapped (bottomology, depth);
      local_topology->contained_topologies = NULL;
      if (bottomology != NULL) {
        local_topology->contained_topologies =
            g_list_append (local_topology->contained_topologies, bottomology);
      } else {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE,
            "Found contained topology in audio stream, unable to parse it\n");
        g_strfreev (splitted);
        return NULL;
      }
    }
    g_strfreev (splitted);

    if (*lines != NULL)
      lines++;
  }
  return local_topology;
}


static Topology *
read_subtitles (Topology * local_topology, int depth)
{
  gchar *line;
  gchar **splitted;
  int indent;
  GstTagList *tags;
  Topology *bottomology;

  splitted = NULL;
  local_topology->type = TYPE_SUBTITLES;

  while (*lines != NULL) {
    line = *lines;
    indent = 0;
    while (g_ascii_isspace (line[indent])) {
      indent++;
    }
    if (indent < depth) {
      /*Oops - this line belongs to the previous topology */
      lines--;
      return local_topology;
    } else {
      line += indent;
    }
    if (g_str_equal (line, "")) {
      /*Found empty line, get next */
      if (*lines != NULL)
        lines++;
      continue;
    }
    /*Line should now be trimmed. Find the label */
    splitted = g_strsplit (line, ": ", 2);
    if (g_str_has_prefix (splitted[0], "Codec")) {
      if (*lines != NULL)
        lines++;
      /* Ignore: duplicate */
      /*g_io_channel_read_line_string(gch, buffer, NULL, err);
         line = buffer->str;
         caps = gst_caps_from_string(line);
         bottomology = g_list_append(bottomology, splitted[0]);
         bottomology = g_list_append(bottomology, caps); */
    } else if (g_str_has_prefix (splitted[0], "Additional info")) {
      if (*lines != NULL)
        lines++;
      /*line = lines[cur_line];
         //Manually trim
         line += depth;
         local_topology->additional_info = gst_structure_from_string(line, NULL); */
    } else if (g_str_has_prefix (splitted[0], "Tags")) {
      if (*lines != NULL)
        lines++;
      line = *lines;
      while (g_ascii_isspace (line[0])) {
        line++;
      }
      if (!g_str_has_prefix (line, "None")) {
        tags = gst_tag_list_new_from_string (line);
        if (tags == NULL) {
          insanity_test_validate_checklist_item (gstest,
              "comparison-file-parsed", FALSE, "Erroneous value in tags field");
          g_strfreev (splitted);
          return NULL;
        }
        local_topology->tags = tags;
      }
    } else if (g_str_has_prefix (splitted[0], "Language")) {
      local_topology->language =
          (g_str_has_prefix (splitted[1],
              "<unknown>")) ? NULL : g_strdup (splitted[1]);
    } else if (g_str_has_prefix (line, "audio")
        || g_str_has_prefix (line, "video")
        || g_str_has_prefix (line, "subtitles")
        || g_str_has_prefix (line, "unknown")) {
      /*We contain another topology */
      lines--;
      bottomology = read_topology_wrapped (bottomology, depth);
      local_topology->contained_topologies = NULL;
      if (bottomology != NULL)
        local_topology->contained_topologies =
            g_list_append (local_topology->contained_topologies, bottomology);
    } else {
      insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
          FALSE,
          "Found contained topology in subtitle stream, unable to parse it\n");
      g_strfreev (splitted);
      return NULL;
      /* local_topology->unknown = g_strdup(line); */
    }
    g_strfreev (splitted);
    if (*lines != NULL)
      lines++;
  }
  return local_topology;
}

static Topology *
read_topology_wrapped (Topology * local_topology, int depth)
{
  gchar *line;
  gchar **splitted = NULL;
  int indent;
  Topology *bottomology;

  local_topology = g_new0 (Topology, 1);
  local_topology->checked = FALSE;
  local_topology->contained_topologies = NULL;

  while (*lines != NULL) {
    line = *lines;
    indent = 0;
    while (g_ascii_isspace (line[indent])) {
      indent++;
    }
    if (indent < depth) {
      /*Oops - this line belongs to the previous topology */
      lines--;
      return local_topology;
    } else if (indent == depth + 2) {
      /*Oops - bottomology */
      bottomology = read_topology_wrapped (bottomology, depth + 2);
      if (bottomology == NULL) {
        /*Reason given in advance */
        return NULL;
      }
      local_topology->contained_topologies =
          g_list_append (local_topology->contained_topologies, bottomology);
      if (*lines != NULL)
        lines++;
      continue;
    } else {
      line += indent;
    }
    if (g_str_equal (line, "")) {
      /*Found empty line, get next */
      if (*lines != NULL)
        lines++;
      continue;
    }
    /*Line should now be trimmed. Find the label */
    splitted = g_strsplit (line, ": ", 2);
    local_topology->caps = gst_caps_from_string (splitted[1]);
    if (local_topology->caps == NULL) {
      insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
          FALSE, "Erroneous value in caps field");
      g_strfreev (splitted);
      return NULL;
    }
    if (g_str_has_prefix (splitted[0], "container")) {
      local_topology->type = TYPE_CONTAINER;
      if (*lines != NULL)
        lines++;
      bottomology = read_topology_wrapped (bottomology, depth + 2);
      if (bottomology == NULL) {
        /*Reason given in advance */
        g_strfreev (splitted);
        return NULL;
      }
      local_topology->contained_topologies =
          g_list_append (local_topology->contained_topologies, bottomology);
    } else if (g_str_has_prefix (splitted[0], "subtitles")) {
      if (*lines != NULL)
        lines++;
      local_topology = read_subtitles (local_topology, depth + 2);
      g_strfreev (splitted);
      return local_topology;
    } else if (g_str_has_prefix (splitted[0], "audio")) {
      if (*lines != NULL)
        lines++;
      local_topology = read_audio (local_topology, depth + 2);
      g_strfreev (splitted);
      return local_topology;
    } else if (g_str_has_prefix (splitted[0], "video")) {
      if (*lines != NULL)
        lines++;
      local_topology = read_video (local_topology, depth + 2);
      g_strfreev (splitted);
      return local_topology;
    } else if (g_str_has_prefix (splitted[0], "unknown")) {
      if (*lines != NULL)
        lines++;
      local_topology = read_unknown (local_topology, depth + 2);
      g_strfreev (splitted);
      return local_topology;
    }
    g_strfreev (splitted);
    if (*lines != NULL)
      lines++;
  }
  return local_topology;

}

static Topology *
read_topology (void)
{
  return read_topology_wrapped (topology, 2);
}


static Properties *
read_properties (void)
{
  gchar *line;
  gchar **splitted = NULL;
  gchar **splitted2 = NULL;
  int indent;

  guint64 hours, mins, secs, nsecs;
  Properties *local_properties = g_new0 (Properties, 1);

  local_properties->tags =
      g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);

  /*Label the list */
  while (*lines != NULL) {
    line = *lines;
    indent = 0;
    while (g_ascii_isspace (line[0])) {
      indent++;
      line++;
    }
    if (g_str_equal (line, "")) {
      return local_properties;
    }
    /*Line should now be trimmed. Find the label */
    splitted = g_strsplit (line, ": ", 2);
    if (g_str_has_prefix (splitted[0], "Tags")) {
      /*The rest of the file is tags */
      if (*lines != NULL)
        lines++;
      while (*lines != NULL) {
        line = *lines;
        indent = 0;
        while (g_ascii_isspace (line[indent])) {
          indent++;
        }
        line += indent;
        if (g_str_equal (line, "")) {
          g_strfreev (splitted);
          return local_properties;
        }
        /*Line should now be trimmed. Find the label */
        splitted2 = g_strsplit (line, ": ", 2);
        g_hash_table_insert (local_properties->tags,
            g_strdup (splitted2[0]), g_strdup (splitted2[1]));
        g_strfreev (splitted2);
        if (*lines != NULL)
          lines++;
      }
      /*The file should contain nothing more after the tags */
      g_strfreev (splitted);
      return local_properties;
    } else if (g_str_has_prefix (splitted[0], "Duration")) {
      if (sscanf (splitted[1],
              "%" G_GUINT64_FORMAT ":%02" G_GINT64_MODIFIER "u:%02"
              G_GINT64_MODIFIER "u.%09" G_GINT64_MODIFIER "u", &hours, &mins,
              &secs, &nsecs) == 4) {
        GstClockTime duration =
            (hours * 3600 + mins * 60 + secs) * GST_SECOND +
            nsecs * GST_NSECOND;
        local_properties->duration = duration;
      } else {
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Erroneous value in duration field!");
        g_strfreev (splitted);
        return NULL;
      }
    } else if (g_str_has_prefix (splitted[0], "Seekable")) {
      if (g_str_has_prefix (splitted[1], "yes")) {
        local_properties->seekable = TRUE;
      } else if (g_str_has_prefix (splitted[1], "no")) {
        local_properties->seekable = FALSE;
      } else {
        /*Unknown value found */
        insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
            FALSE, "Properties: Seekable value is not a boolean");
        g_strfreev (splitted);
        return NULL;
      }
    }
    if (*lines != NULL)
      lines++;
    g_strfreev (splitted);
  }
  return local_properties;
}

static gboolean
read_expected_file (gchar * filename)
{
  int indent;
  gchar *line;
  gchar **splitted = NULL;
  gchar *contents;
  gchar **plines;
  GError *err = NULL;

  if (!g_file_get_contents (filename, &contents, NULL, &err)) {
    contents = g_strdup_printf ("Cannot read expected file: %s", err->message);
    insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
        FALSE, contents);
    g_free (contents);
    g_clear_error (&err);
    return FALSE;
  }

  plines = g_strsplit (contents, "\n", 0);
  lines = plines;
  g_free (contents);

  while (*lines != NULL) {
    line = *lines;
    indent = 0;
    while (g_ascii_isspace (line[indent])) {
      indent++;
    }
    line += indent;
    /*Silently ignore empty lines */
    if (g_str_equal (line, "")) {
      if (*lines != NULL)
        lines++;
      continue;
    }
    /*Line should now be trimmed. Find the label */
    splitted = g_strsplit (line, ": ", 1);
    if (g_str_has_prefix (splitted[0], "Topology")) {
      /*Don't re-read this line */
      if (*lines != NULL)
        lines++;
      topology = read_topology ();
      if (topology == NULL) {
        g_strfreev (splitted);
        g_strfreev (plines);
        return FALSE;           /*First thing read_topology does is to malloc it.. so it means it exploded */
      }
    } else if (g_str_has_prefix (splitted[0], "Properties")) {
      if (*lines != NULL)
        lines++;
      properties = read_properties ();
      if (properties == NULL) {
        g_strfreev (splitted);
        g_strfreev (plines);
        return FALSE;           /*First thing read_topology does is to malloc it.. so it means it exploded */
      }
    }                           /*Discard all other lines */
    g_strfreev (splitted);
    if (*lines != NULL)
      lines++;
  }                             /*Either EOF or error */

  g_strfreev (plines);

  if (topology == NULL || properties == NULL) {
    insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
        FALSE, "Expected file contains no useful data");
    return FALSE;
  }

  return TRUE;
}

static gboolean
_run_async (GstDiscoverer * dc)
{
  gst_discoverer_discover_uri_async (dc, uri);

  return FALSE;
}

static gchar *
compare_video (GstDiscovererStreamInfo * info, Topology * local_topology)
{

  GstDiscovererVideoInfo *video_info;
  gint num, denom;
  gchar *ret1, *ret2, *ret3;
  const GstTagList *tags;

  video_info = (GstDiscovererVideoInfo *) info;

  if (gst_discoverer_video_info_get_width (video_info) != local_topology->width) {
    return g_strdup_printf ("Error in video width: found: %u, expected: %u",
        gst_discoverer_video_info_get_width (video_info),
        local_topology->width);
  }

  if (gst_discoverer_video_info_get_height (video_info) !=
      local_topology->height) {
    return g_strdup_printf ("Error in video height: found: %u, expected: %u",
        gst_discoverer_video_info_get_height (video_info),
        local_topology->height);
  }

  if (gst_discoverer_video_info_get_depth (video_info) != local_topology->depth) {
    return g_strdup_printf ("Error in video depth: found: %u, expected: %u",
        gst_discoverer_video_info_get_depth (video_info),
        local_topology->depth);
  }

  num = gst_discoverer_video_info_get_framerate_num (video_info);
  denom = gst_discoverer_video_info_get_framerate_denom (video_info);

  if (num != local_topology->framerate_num
      || denom != local_topology->framerate_denom) {
    return
        g_strdup_printf
        ("Error in video frame rate: found: %u/%u, expected: %u/%u", num, denom,
        local_topology->framerate_num, local_topology->framerate_denom);
  }

  num = gst_discoverer_video_info_get_par_num (video_info);
  denom = gst_discoverer_video_info_get_par_denom (video_info);

  if (num != local_topology->aspectratio_num
      || denom != local_topology->aspectratio_denom) {
    return
        g_strdup_printf
        ("Error in video aspect ratio: found: %u/%u, expected: %u/%u", num,
        denom, local_topology->aspectratio_num,
        local_topology->aspectratio_denom);
  }

  if (gst_discoverer_video_info_is_interlaced (video_info) !=
      local_topology->interlaced) {
    return
        g_strdup_printf ("Error in video interlaced: found: %s, expected: %s",
        gst_discoverer_video_info_is_interlaced (video_info) ? "true" : "false",
        local_topology->interlaced ? "true" : "false");
  }

  if (gst_discoverer_video_info_get_bitrate (video_info) !=
      local_topology->bitrate) {
    return g_strdup_printf ("Error in video bitrate: found: %u, expected: %u",
        gst_discoverer_video_info_get_bitrate (video_info),
        local_topology->bitrate);
  }

  if (gst_discoverer_video_info_get_max_bitrate (video_info) !=
      local_topology->max_bitrate) {
    return
        g_strdup_printf ("Error in video max_bitrate: found: %u, expected: %u",
        gst_discoverer_video_info_get_max_bitrate (video_info),
        local_topology->max_bitrate);
  }

  tags = gst_discoverer_stream_info_get_tags (info);
  if (tags == NULL || local_topology->tags == NULL) {
    if (tags != NULL || local_topology->tags != NULL) {
      ret1 =
          tags ? gst_structure_to_string ((GstStructure *) tags) :
          g_strdup ("NULL");
      ret2 = local_topology->tags ? gst_structure_to_string ((GstStructure *)
          local_topology->tags) : g_strdup ("NULL");
      ret3 =
          g_strdup_printf ("Video tags mismatch: found:\n%s, expected:\n%s",
          ret1, ret2);
      g_free (ret1);
      g_free (ret2);
      return ret3;
    }
  } else if (!gst_tag_list_is_equal (tags, local_topology->tags)) {
    ret1 = gst_structure_to_string ((GstStructure *) tags);
    ret2 = gst_structure_to_string ((GstStructure *) local_topology->tags);
    ret3 = g_strdup_printf ("Video tags mismatch: found:\n%s, expected:\n%s",
        ret1, ret2);
    g_free (ret1);
    g_free (ret2);
    return ret3;
  }

  return NULL;

}

static gchar *
compare_audio (GstDiscovererStreamInfo * info, Topology * local_topology)
{

  GstDiscovererAudioInfo *audio_info;
  const gchar *tmp;
  const GstTagList *tags;
  gchar *ret1, *ret2, *ret3;

  audio_info = (GstDiscovererAudioInfo *) info;

  tmp = gst_discoverer_audio_info_get_language (audio_info);
  /*if both expected and discovered language are NULL, fine...
     if only one is NULL, wrong...
     if both are non-NULL, compare them */
  if (tmp == NULL || local_topology->language == NULL) {
    if ((tmp != NULL) || (local_topology->language != NULL))
      return
          g_strdup_printf ("Error in audio language: found: %s, expected: %s",
          tmp, local_topology->language);
  } else if (!g_str_equal (tmp, local_topology->language)) {
    return g_strdup_printf ("Error in audio language: found: %s, expected: %s",
        tmp, local_topology->language);
  }

  if (gst_discoverer_audio_info_get_channels (audio_info) !=
      local_topology->channels) {
    return g_strdup_printf ("Error in audio channels: found: %u, expected: %u",
        gst_discoverer_audio_info_get_channels (audio_info),
        local_topology->channels);
  }

  if (gst_discoverer_audio_info_get_sample_rate (audio_info) !=
      local_topology->sample_rate) {
    return
        g_strdup_printf ("Error in audio sample rate: found: %u, expected: %u",
        gst_discoverer_audio_info_get_sample_rate (audio_info),
        local_topology->sample_rate);
  }

  if (gst_discoverer_audio_info_get_depth (audio_info) != local_topology->depth) {
    return g_strdup_printf ("Error in audio depth: found: %u, expected: %u",
        gst_discoverer_audio_info_get_depth (audio_info),
        local_topology->depth);
  }

  if (gst_discoverer_audio_info_get_bitrate (audio_info) !=
      local_topology->bitrate) {
    return g_strdup_printf ("Error in audio bitrate: found: %u, expected: %u",
        gst_discoverer_audio_info_get_bitrate (audio_info),
        local_topology->bitrate);
  }

  if (gst_discoverer_audio_info_get_max_bitrate (audio_info) !=
      local_topology->max_bitrate) {
    return
        g_strdup_printf ("Error in audio max_bitrate: found: %u, expected: %u",
        gst_discoverer_audio_info_get_max_bitrate (audio_info),
        local_topology->max_bitrate);
  }

  tags = gst_discoverer_stream_info_get_tags (info);
  if (tags == NULL || local_topology->tags == NULL) {
    if (tags != NULL || local_topology->tags != NULL) {
      ret1 = gst_structure_to_string ((GstStructure *) tags);
      ret2 = gst_structure_to_string ((GstStructure *) local_topology->tags);
      ret3 = g_strdup_printf ("Audio tags mismatch: found:\n%s, expected:\n%s",
          ret1, ret2);
      g_free (ret1);
      g_free (ret2);
      return ret3;
    }
  } else if (!gst_tag_list_is_equal (tags, local_topology->tags)) {
    ret1 = gst_structure_to_string ((GstStructure *) tags);
    ret2 = gst_structure_to_string ((GstStructure *) local_topology->tags);
    ret3 = g_strdup_printf ("Audio tags mismatch: found:\n%s, expected:\n%s",
        ret1, ret2);
    g_free (ret1);
    g_free (ret2);
    return ret3;
  }

  return NULL;

}

static gchar *
compare_subtitles (GstDiscovererStreamInfo * info, Topology * local_topology)
{

  GstDiscovererSubtitleInfo *subtitle_info;
  const gchar *tmp;
  const GstTagList *tags;
  gchar *ret1, *ret2, *ret3;

  subtitle_info = (GstDiscovererSubtitleInfo *) info;
  tmp = gst_discoverer_subtitle_info_get_language (subtitle_info);

  /*if both expected and discovered language are NULL, fine...
     if only one is NULL, wrong...
     if both are non-NULL, compare them */
  if (tmp == NULL || local_topology->language == NULL) {
    if ((tmp != NULL) || (local_topology->language != NULL))
      return
          g_strdup_printf
          ("Error in subtitles language: found: %s, expected: %s", tmp,
          local_topology->language);
  } else if (!g_str_equal (tmp, local_topology->language)) {
    return
        g_strdup_printf
        ("Error in subtitles language: found: %s, expected: %s\n", tmp,
        local_topology->language);
  }

  tags = gst_discoverer_stream_info_get_tags (info);
  if (tags == NULL || local_topology->tags == NULL) {
    if (tags != NULL || local_topology->tags != NULL) {
      ret1 =
          tags == NULL ?
          g_strdup ("NULL") : gst_structure_to_string ((GstStructure *) tags);
      ret2 =
          local_topology->tags == NULL ?
          g_strdup ("NULL") : gst_structure_to_string ((GstStructure *)
          local_topology->tags);
      ret3 =
          g_strdup_printf ("Subtitle tags mismatch: found:\n%s, expected:\n%s",
          ret1, ret2);
      g_free (ret1);
      g_free (ret2);
      return ret3;
    }
  } else if (!gst_tag_list_is_equal (tags, local_topology->tags)) {
    ret1 = gst_structure_to_string ((GstStructure *) tags);
    ret2 = gst_structure_to_string ((GstStructure *) local_topology->tags);
    ret3 = g_strdup_printf ("Subtitle tags mismatch: found:\n%s, expected:\n%s",
        ret1, ret2);
    g_free (ret1);
    g_free (ret2);
    return ret3;
  }

  return NULL;

}

static void
uncheck_all (Topology * local_topology)
{

  GList *tmp1, *streams1;
  local_topology->checked = FALSE;

  streams1 = local_topology->contained_topologies;

  for (tmp1 = streams1; tmp1; tmp1 = tmp1->next) {
    Topology *tmpinf = (Topology *) tmp1->data;
    uncheck_all (tmpinf);
  }
}

static gchar *
compare_topology (GstDiscovererStreamInfo * info, Topology * local_topology)
{

  GstDiscovererStreamInfo *next;
  gchar *temp = NULL;
  gchar *ret1, *ret2, *ret3;
  GstCaps *caps;

  caps = gst_discoverer_stream_info_get_caps (info);
  if (!gst_caps_is_equal (caps, local_topology->caps)) {
    ret1 = gst_caps_to_string (caps);
    ret2 = gst_caps_to_string (local_topology->caps);
    ret3 = g_strdup_printf ("Error in caps: found:\n%s\nexpected:\n%s\n",
        ret1, ret2);
    g_free (ret1);
    g_free (ret2);
    gst_caps_unref (caps);
    return ret3;
  }

  gst_caps_unref (caps);

  if (local_topology->type == TYPE_CONTAINER
      && GST_IS_DISCOVERER_CONTAINER_INFO (info)) {
    GList *tmp1, *streams1, *tmp2, *streams2;

    streams1 =
        gst_discoverer_container_info_get_streams (GST_DISCOVERER_CONTAINER_INFO
        (info));

    streams2 = local_topology->contained_topologies;

    for (tmp1 = streams1; tmp1; tmp1 = tmp1->next) {
      GstDiscovererStreamInfo *tmpinf = (GstDiscovererStreamInfo *) tmp1->data;
      for (tmp2 = streams2; tmp2; tmp2 = tmp2->next) {
        Topology *tmpinf2 = (Topology *) tmp2->data;
        ret1 = compare_topology (tmpinf, tmpinf2);
        if (ret1 == NULL) {
          /*Streams match */
          tmpinf2->checked = TRUE;
        } else {
          insanity_test_printf (gstest, "Possible error: %s\n", ret1);
          g_free (ret1);
        }
      }
    }

    for (tmp2 = streams2; tmp2; tmp2 = tmp2->next) {
      Topology *tmpinf2 = (Topology *) tmp2->data;
      if (!(tmpinf2->checked)) {
        gchar *temp2;
        temp = gst_caps_to_string (tmpinf2->caps);
        temp2 = g_strdup_printf ("Expected stream not found: %s", temp);
        g_free (temp);
        uncheck_all (local_topology);
        gst_discoverer_stream_info_list_free (streams1);
        return temp2;
      }
    }

    if (g_list_length (streams1) != g_list_length (streams2)) {
      /*Don't really know which one it was */
      gchar *temp2;
      temp2 =
          g_strdup_printf ("Found unexpected stream: %u vs %u\n",
          g_list_length (streams1), g_list_length (streams2));
      uncheck_all (local_topology);
      gst_discoverer_stream_info_list_free (streams1);
      return temp2;
    }

    gst_discoverer_stream_info_list_free (streams1);

  } else {
    if (GST_IS_DISCOVERER_AUDIO_INFO (info)
        && local_topology->type == TYPE_AUDIO) {
      temp = compare_audio (info, local_topology);
    } else if (GST_IS_DISCOVERER_VIDEO_INFO (info)
        && local_topology->type == TYPE_VIDEO) {
      temp = compare_video (info, local_topology);
    } else if (GST_IS_DISCOVERER_SUBTITLE_INFO (info)
        && local_topology->type == TYPE_SUBTITLES) {
      temp = compare_subtitles (info, local_topology);
    } else if (local_topology->type == TYPE_UNKNOWN) {
      /*Ignore... */
    } else {
      const gchar *temp3 = NULL;
      if (local_topology->type == TYPE_AUDIO)
        temp3 = "audio";
      else if (local_topology->type == TYPE_VIDEO)
        temp3 = "video";
      else if (local_topology->type == TYPE_SUBTITLES)
        temp3 = "subtitles";
      else if (local_topology->type == TYPE_UNKNOWN)
        temp3 = "unknown";
      else if (local_topology->type == TYPE_CONTAINER)
        temp3 = "container";
      temp = g_strdup_printf ("Stream type mismatch: found: %s, expected: %s",
          gst_discoverer_stream_info_get_stream_type_nick (info), temp3);
    }
    if (temp != NULL) {
      insanity_test_printf (gstest, "Possible error: %s", temp);
      return temp;
    }
    /*Compare substream, if any */
    next = gst_discoverer_stream_info_get_next (info);
    if (next) {
      gchar *ret;
      if (g_list_first (local_topology->contained_topologies)->data == NULL) {
        ret = g_strdup_printf ("Error: stream not found: %s",
            gst_discoverer_stream_info_get_stream_type_nick (next));
        return ret;
      }
      ret = compare_topology (next,
          (Topology *) (g_list_first (local_topology->
                  contained_topologies)->data));
      gst_discoverer_stream_info_unref (next);
      return ret;
    }
  }

  return NULL;
}

static gboolean
search_tag (GQuark field_id, const GValue * value, gpointer user_data)
{

  const gchar *search_string;
  gchar *ser, *tmp, *lookup_result;

  search_string = gst_tag_get_nick (g_quark_to_string (field_id));
  lookup_result =
      (gchar *) g_hash_table_lookup (properties->tags, search_string);

  if (G_VALUE_HOLDS_STRING (value))
    ser = g_value_dup_string (value);
  else if (GST_VALUE_HOLDS_BUFFER (value)) {
    GstBuffer *buf = gst_value_get_buffer (value);
    ser = g_strdup_printf ("<GstBuffer [%d bytes]>", GST_BUFFER_SIZE (buf));
  } else
    ser = gst_value_serialize (value);

  if (lookup_result == NULL) {
    tmp =
        g_strdup_printf ("Error in Properties Tags: NOT found: %s",
        search_string);
    insanity_test_validate_checklist_item (gstest, "discoverer-correct", FALSE,
        tmp);
    g_free (tmp);
    return FALSE;
  } else if (!g_str_equal (lookup_result, ser)) {
    tmp =
        g_strdup_printf ("Error in Properties Tag %s: found: %s, expected: %s",
        search_string, ser, lookup_result);

    insanity_test_validate_checklist_item (gstest, "discoverer-correct", FALSE,
        tmp);
    g_hash_table_remove (properties->tags, search_string);
    g_free (ser);
    g_free (tmp);
    return FALSE;
  }

  g_hash_table_remove (properties->tags, search_string);
  g_free (ser);
  return TRUE;

}

static gboolean
compare_properties (GstDiscovererInfo * dcinfo)
{

  gchar *tmp;
  const GstTagList *tags;

  if (gst_discoverer_info_get_duration (info) != properties->duration) {
    tmp = g_strdup_printf ("Error in Duration: found: %" GST_TIME_FORMAT
        ", expected: %" GST_TIME_FORMAT "",
        GST_TIME_ARGS (gst_discoverer_info_get_duration (info)),
        GST_TIME_ARGS (properties->duration));

    insanity_test_validate_checklist_item (gstest, "discoverer-correct", FALSE,
        tmp);
    g_free (tmp);
    return FALSE;
  }

  if (gst_discoverer_info_get_seekable (info) != properties->seekable) {
    tmp = g_strdup_printf ("Error in Seekable: found: %s, expected: %s",
        gst_discoverer_info_get_seekable (info) ? "true" : "false",
        properties->seekable ? "true" : "false");

    insanity_test_validate_checklist_item (gstest, "discoverer-correct", FALSE,
        tmp);
    g_free (tmp);
    return FALSE;
  }

  if ((tags = gst_discoverer_info_get_tags (info))) {
    if (!gst_structure_foreach ((const GstStructure *) tags, search_tag, NULL)) {
      return FALSE;
    }
  }

  return TRUE;

}


static void
_new_discovered_uri (GstDiscoverer * dc, GstDiscovererInfo * dcinfo,
    GError * dcerr)
{
  gchar *expected_uri;
  gchar *expected_file;
  gchar *compare_result;
  gchar *tmp;
  gboolean status;
  GstDiscovererResult result;
  GstDiscovererStreamInfo *sinfo;
  GError *err = NULL;

  info = dcinfo;

  insanity_test_ping (gstest);

  insanity_test_printf (gstest, "Analyzing done!\n");
  if (info == NULL) {
    insanity_test_validate_checklist_item (gstest,
        "discoverer-returned-results", FALSE, "Discoverer went missing!");
    return;
  }

  insanity_test_printf (gstest, "discoverer_test_test\n");

  result = gst_discoverer_info_get_result (dcinfo);

  switch (result) {
    case GST_DISCOVERER_OK:
    {
      insanity_test_printf (gstest, "Discoverer returned OK\n");
      break;
    }
    case GST_DISCOVERER_URI_INVALID:
    {
      insanity_test_validate_checklist_item (gstest,
          "discoverer-returned-results", FALSE, "URI is not valid");
      return;
    }
    case GST_DISCOVERER_ERROR:
    {
      compare_result =
          g_strconcat ("An error was encountered while discovering the file: ",
          dcerr->message, (char *) NULL);
      insanity_test_validate_checklist_item (gstest,
          "discoverer-returned-results", FALSE, compare_result);
      g_free (compare_result);
      return;
    }
    case GST_DISCOVERER_TIMEOUT:
    {
      insanity_test_validate_checklist_item (gstest,
          "discoverer-returned-results", FALSE, "Analyzing URI timed out");
      return;
    }
    case GST_DISCOVERER_BUSY:
    {
      insanity_test_validate_checklist_item (gstest,
          "discoverer-returned-results", FALSE, "Discoverer was busy");
      return;
    }
    case GST_DISCOVERER_MISSING_PLUGINS:
    {
      tmp = gst_structure_to_string (gst_discoverer_info_get_misc (info));
      compare_result = g_strconcat ("Missing plugins", tmp, NULL);
      g_free (tmp);
      insanity_test_validate_checklist_item (gstest,
          "discoverer-returned-results", FALSE, compare_result);
      g_free (compare_result);
      return;
    }
  }

  insanity_test_validate_checklist_item (gstest, "discoverer-returned-results",
      TRUE, "Discoverer returned");

  expected_uri = g_strconcat (uri, ".discoverer-expected", (char *) NULL);

  expected_file = g_filename_from_uri (expected_uri, NULL, &err);

  if (expected_file == NULL) {
    compare_result =
        g_strconcat ("Cannot figure out expected filename: ", err->message,
        (char *) NULL);
    insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
        FALSE, compare_result);
    g_clear_error (&err);
    g_free (compare_result);
    return;
  }

  if (skip_compare) {
    insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
        TRUE, NULL);
    insanity_test_validate_checklist_item (gstest, "discoverer-correct", TRUE,
        NULL);
    return;
  }

  status = read_expected_file (expected_file);

  if (!status) {
    return;                     /*test already invalidated before, reason given */
  }

  insanity_test_validate_checklist_item (gstest, "comparison-file-parsed", TRUE,
      "Comparison file parsed successfully");

  if (!compare_properties (dcinfo)) {
    return;
  }

  sinfo = gst_discoverer_info_get_stream_info (info);

  compare_result = compare_topology (sinfo, topology);

  g_free (expected_uri);
  g_free (expected_file);
  gst_discoverer_stream_info_unref (sinfo);

  if (compare_result != NULL) {
    insanity_test_validate_checklist_item (gstest, "discoverer-correct", FALSE,
        compare_result);
    g_free (compare_result);
    return;
  } else {
    g_free (compare_result);
    insanity_test_validate_checklist_item (gstest, "discoverer-correct", TRUE,
        "Discoverer returned the right results");
    return;
  }
}

static void
_discoverer_finished (GstDiscoverer * dc, gpointer user_data)
{
  insanity_test_printf (gstest, "Discoverer finished!\n");

  g_main_loop_quit (ml);
}

static gboolean
discoverer_test_setup (InsanityTest * test)
{
  GError *err = NULL;

  dc = gst_discoverer_new (timeout * GST_SECOND, &err);
  if (G_UNLIKELY (dc == NULL)) {
    printf ("Error initializing: %s\n", err->message);
    g_clear_error (&err);
    return FALSE;
  }

  /* connect signals */
  g_signal_connect (dc, "discovered", G_CALLBACK (_new_discovered_uri), NULL);
  g_signal_connect (dc, "finished", G_CALLBACK (_discoverer_finished), NULL);

  return TRUE;
}

static gboolean
discoverer_test_start (InsanityTest * test)
{
  gstest = test;
  (void) test;
  return TRUE;
}

static void
free_topology (Topology * local_topology)
{
  GList *tmp, *topologies;

  topologies = local_topology->contained_topologies;

  for (tmp = topologies; tmp; tmp = tmp->next) {
    Topology *tmptop = (Topology *) tmp->data;
    free_topology (tmptop);
  }

  gst_caps_unref (local_topology->caps);
  if (local_topology->tags != NULL)
    gst_tag_list_free (local_topology->tags);
  g_free (local_topology->language);
  g_list_free (local_topology->contained_topologies);
  g_free (local_topology->unknown);

  g_free (local_topology);
}

static void
free_properties (Properties * local_properties)
{
  g_hash_table_destroy (local_properties->tags);
  g_free (local_properties);
}

static void
discoverer_test_stop (InsanityTest * test)
{
  if (async) {
    gst_discoverer_stop (dc);
    if (ml != NULL) {
      g_main_loop_quit (ml);
      while (g_main_loop_is_running (ml))
        g_usleep (20000);
      g_main_loop_unref (ml);
      ml = NULL;
    }
  }

  if (properties != NULL) {
    free_properties (properties);
    properties = NULL;
  }
  if (topology != NULL) {
    free_topology (topology);
    topology = NULL;
  }
  lines = NULL;
  uri = NULL;

  (void) test;
}

static void
discoverer_test_teardown (InsanityTest * test)
{
  g_object_unref (dc);
  dc = NULL;

  (void) test;
}

static void
discoverer_test_test (InsanityTest * test)
{
  insanity_test_get_string_argument (test, "uri", &uri);

  if (g_str_has_suffix (uri, ".discoverer-expected")) {
    /* Not examining expected files... */
    g_free (uri);
    uri = NULL;

    insanity_test_validate_checklist_item (gstest,
        "discoverer-returned-results", TRUE, NULL);
    insanity_test_validate_checklist_item (gstest, "comparison-file-parsed",
        TRUE, NULL);
    insanity_test_validate_checklist_item (gstest, "discoverer-correct", TRUE,
        NULL);

    insanity_test_done (test);
    (void) test;
    return;
  }

  insanity_test_get_boolean_argument (test, "skip-compare", &skip_compare);

  insanity_test_printf (test, "Analyzing URI: %s\n", uri);

  if (!async) {
    GError *err = NULL;

    info = gst_discoverer_discover_uri (dc, uri, &err);
    g_clear_error (&err);
    gst_discoverer_info_unref (info);
  } else {
    g_assert (ml == NULL);

    ml = g_main_loop_new (NULL, FALSE);
    gst_discoverer_start (dc);

    /* adding uris will be started when the mainloop runs */
    g_idle_add ((GSourceFunc) _run_async, dc);

    /* run mainloop */
    g_main_loop_run (ml);
  }

  g_free (uri);
  uri = NULL;

  insanity_test_done (test);
  (void) test;
}

int
main (int argc, char **argv)
{
  InsanityTest *test;
  gboolean ret;

  g_type_init ();

  test =
      INSANITY_TEST (insanity_gst_test_new ("discoverer-test",
          "Check gst-discoverer output",
          "Check gst-discoverer's output against previously stored expected results"));

  insanity_test_add_checklist_item (test, "discoverer-returned-results",
      "Discoverer returned something", "Discoverer returned nothing", FALSE);
  insanity_test_add_checklist_item (test, "comparison-file-parsed",
      "File with expected values parsed",
      "Unable to parse file with expected values", FALSE);
  insanity_test_add_checklist_item (test, "discoverer-correct",
      "Discoverer returned correct results",
      "Discoverer returned something wrong", FALSE);

  insanity_test_add_string_argument (test, "uri", "Input file",
      "URI of file to process", TRUE, "file:///home/user/video.avi");

  insanity_test_add_boolean_argument (test, "skip-compare",
      "Skip comparing results",
      "Just check whether discoverer returns something without comparing it",
      TRUE, TRUE);

  g_signal_connect_after (test, "setup", G_CALLBACK (&discoverer_test_setup),
      0);
  g_signal_connect (test, "start", G_CALLBACK (&discoverer_test_start), 0);
  g_signal_connect (test, "stop", G_CALLBACK (&discoverer_test_stop), 0);
  g_signal_connect (test, "teardown", G_CALLBACK (&discoverer_test_teardown),
      0);
  g_signal_connect (test, "test", G_CALLBACK (&discoverer_test_test), 0);


  ret = insanity_test_run (test, &argc, &argv);

  g_object_unref (test);

  return ret ? 0 : 1;
}
