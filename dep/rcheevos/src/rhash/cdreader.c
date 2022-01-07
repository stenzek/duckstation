#include "rc_hash.h"

#include "../rcheevos/rc_compat.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

/* internal helper functions in hash.c */
extern void* rc_file_open(const char* path);
extern void rc_file_seek(void* file_handle, int64_t offset, int origin);
extern int64_t rc_file_tell(void* file_handle);
extern size_t rc_file_read(void* file_handle, void* buffer, int requested_bytes);
extern void rc_file_close(void* file_handle);
extern int rc_hash_error(const char* message);
extern const char* rc_path_get_filename(const char* path);
extern int rc_path_compare_extension(const char* path, const char* ext);
extern rc_hash_message_callback verbose_message_callback;

struct cdrom_t
{
  void* file_handle;
  int sector_size;
  int sector_header_size;
  int first_sector;
  int64_t first_sector_offset;
};

static void cdreader_determine_sector_size(struct cdrom_t* cdrom)
{
  /* Attempt to determine the sector and header sizes. The CUE file may be lying.
   * Look for the sync pattern using each of the supported sector sizes.
   * Then check for the presence of "CD001", which is gauranteed to be in either the
   * boot record or primary volume descriptor, one of which is always at sector 16.
   */
  const unsigned char sync_pattern[] = {
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00
  };

  unsigned char header[32];
  const int64_t toc_sector = 16;

  cdrom->sector_size = 0;
  cdrom->sector_header_size = 0;

  rc_file_seek(cdrom->file_handle, toc_sector * 2352 + cdrom->first_sector_offset, SEEK_SET);
  if (rc_file_read(cdrom->file_handle, header, sizeof(header)) < sizeof(header))
    return;

  if (memcmp(header, sync_pattern, 12) == 0)
  {
    cdrom->sector_size = 2352;

    if (memcmp(&header[25], "CD001", 5) == 0)
      cdrom->sector_header_size = 24;
    else
      cdrom->sector_header_size = 16;
  }
  else
  {
    rc_file_seek(cdrom->file_handle, toc_sector * 2336 + cdrom->first_sector_offset, SEEK_SET);
    rc_file_read(cdrom->file_handle, header, sizeof(header));

    if (memcmp(header, sync_pattern, 12) == 0)
    {
      cdrom->sector_size = 2336;

      if (memcmp(&header[25], "CD001", 5) == 0)
        cdrom->sector_header_size = 24;
      else
        cdrom->sector_header_size = 16;
    }
    else
    {
      rc_file_seek(cdrom->file_handle, toc_sector * 2048 + cdrom->first_sector_offset, SEEK_SET);
      rc_file_read(cdrom->file_handle, header, sizeof(header));

      if (memcmp(&header[1], "CD001", 5) == 0)
      {
        cdrom->sector_size = 2048;
        cdrom->sector_header_size = 0;
      }
    }
  }
}

static void* cdreader_open_bin_track(const char* path, uint32_t track)
{
  void* file_handle;
  struct cdrom_t* cdrom;

  if (track > 1)
  {
    if (verbose_message_callback)
      verbose_message_callback("Cannot locate secondary tracks without a cue sheet");

    return NULL;
  }

  file_handle = rc_file_open(path);
  if (!file_handle)
    return NULL;

  cdrom = (struct cdrom_t*)calloc(1, sizeof(*cdrom));
  cdrom->file_handle = file_handle;

  cdreader_determine_sector_size(cdrom);

  if (cdrom->sector_size == 0)
  {
    size_t size;

    rc_file_seek(cdrom->file_handle, 0, SEEK_END);
    size = ftell(cdrom->file_handle);

    if ((size % 2352) == 0)
    {
      /* raw tracks use all 2352 bytes and have a 24 byte header */
      cdrom->sector_size = 2352;
      cdrom->sector_header_size = 24;
    }
    else if ((size % 2048) == 0)
    {
      /* cooked tracks eliminate all header/footer data */
      cdrom->sector_size = 2048;
      cdrom->sector_header_size = 0;
    }
    else if ((size % 2336) == 0)
    {
      /* MODE 2 format without 16-byte sync data */
      cdrom->sector_size = 2336;
      cdrom->sector_header_size = 8;
    }
    else
    {
      free(cdrom);

      if (verbose_message_callback)
        verbose_message_callback("Could not determine sector size");

      return NULL;
    }
  }

  return cdrom;
}

static int cdreader_open_bin(struct cdrom_t* cdrom, const char* path, const char* mode)
{
  cdrom->file_handle = rc_file_open(path);
  if (!cdrom->file_handle)
    return 0;

  /* determine sector size */
  cdreader_determine_sector_size(cdrom);

  /* could not determine, which means we'll probably have more issues later
   * but use the CUE provided information anyway
   */
  if (cdrom->sector_size == 0)
  {
    /* All of these modes have 2048 byte payloads. In MODE1/2352 and MODE2/2352
     * modes, the mode can actually be specified per sector to change the payload
     * size, but that reduces the ability to recover from errors when the disc
     * is damaged, so it's seldomly used, and when it is, it's mostly for audio
     * or video data where a blip or two probably won't be noticed by the user.
     * So, while we techincally support all of the following modes, we only do
     * so with 2048 byte payloads.
     * http://totalsonicmastering.com/cuesheetsyntax.htm
     * MODE1/2048 ? CDROM Mode1 Data (cooked) [no header, no footer]
     * MODE1/2352 ? CDROM Mode1 Data (raw)    [16 byte header, 288 byte footer]
     * MODE2/2336 ? CDROM-XA Mode2 Data       [8 byte header, 280 byte footer]
     * MODE2/2352 ? CDROM-XA Mode2 Data       [24 byte header, 280 byte footer]
     */
    if (memcmp(mode, "MODE2/2352", 10) == 0)
    {
      cdrom->sector_size = 2352;
      cdrom->sector_header_size = 24;
    }
    else if (memcmp(mode, "MODE1/2048", 10) == 0)
    {
      cdrom->sector_size = 2048;
      cdrom->sector_header_size = 0;
    }
    else if (memcmp(mode, "MODE2/2336", 10) == 0)
    {
      cdrom->sector_size = 2336;
      cdrom->sector_header_size = 8;
    }
    else if (memcmp(mode, "MODE1/2352", 10) == 0)
    {
      cdrom->sector_size = 2352;
      cdrom->sector_header_size = 16;
    }
  }

  return (cdrom->sector_size != 0);
}

static char* cdreader_get_bin_path(const char* cue_path, const char* bin_name)
{
  const char* filename = rc_path_get_filename(cue_path);
  const size_t bin_name_len = strlen(bin_name);
  const size_t cue_path_len = filename - cue_path;
  const size_t needed = cue_path_len + bin_name_len + 1;

  char* bin_filename = (char*)malloc(needed);
  if (!bin_filename)
  {
    char buffer[64];
    snprintf(buffer, sizeof(buffer), "Failed to allocate %u bytes", (unsigned)needed);
    rc_hash_error((const char*)buffer);
  }
  else
  {
    memcpy(bin_filename, cue_path, cue_path_len);
    memcpy(bin_filename + cue_path_len, bin_name, bin_name_len + 1);
  }

  return bin_filename;
}

static int64_t cdreader_get_bin_size(const char* cue_path, const char* bin_name)
{
  int64_t size = 0;
  char* bin_filename = cdreader_get_bin_path(cue_path, bin_name);
  if (bin_filename)
  {
    void* file_handle = rc_file_open(bin_filename);
    if (file_handle)
    {
      rc_file_seek(file_handle, 0, SEEK_END);
      size = rc_file_tell(file_handle);
      rc_file_close(file_handle);
    }

    free(bin_filename);
  }

  return size;
}

static void* cdreader_open_cue_track(const char* path, uint32_t track)
{
  void* file_handle;
  int64_t file_offset = 0;
  char buffer[1024], mode[16];
  char* bin_filename;
  char file[256];
  char *ptr, *ptr2, *end;
  int current_track = 0;
  int sector_size = 0;
  int track_first_sector = 0;
  int previous_sector_size = 0;
  int previous_index_sector_offset = 0;
  int previous_track_is_data = 0;
  int64_t previous_track_sector_offset = 0;
  char previous_track_mode[16];
  int largest_track = 0;
  int largest_track_sector_count = 0;
  int64_t largest_track_offset = 0;
  char largest_track_mode[16];
  char largest_track_file[256];
  int64_t offset = 0;
  int done = 0;
  size_t num_read = 0;
  struct cdrom_t* cdrom = NULL;

  file_handle = rc_file_open(path);
  if (!file_handle)
    return NULL;

  file[0] = '\0';
  do
  {
    num_read = rc_file_read(file_handle, buffer, sizeof(buffer) - 1);
    if (num_read == 0)
      break;

    buffer[num_read] = 0;
    if (num_read == sizeof(buffer) - 1)
      end = buffer + sizeof(buffer) * 3 / 4;
    else
      end = buffer + num_read;

    for (ptr = buffer; ptr < end; ++ptr)
    {
      while (*ptr == ' ')
        ++ptr;

      if (strncasecmp(ptr, "INDEX ", 6) == 0)
      {
        int m = 0, s = 0, f = 0;
        int index;
        int sector_offset;

        ptr += 6;
        index = atoi(ptr);

        while (*ptr != ' ' && *ptr != '\n')
          ++ptr;
        while (*ptr == ' ')
          ++ptr;

        /* convert mm:ss:ff to sector count */
        sscanf(ptr, "%d:%d:%d", &m, &s, &f);
        sector_offset = ((m * 60) + s) * 75 + f;
        sector_offset -= previous_index_sector_offset;

        if (index == 1)
          track_first_sector += sector_offset;

        /* if looking for the largest data track, determine previous track size */
        if (index == 1 && track == RC_HASH_CDTRACK_LARGEST && previous_track_is_data)
        {
          if (sector_offset > largest_track_sector_count)
          {
            largest_track_sector_count = sector_offset;
            largest_track_offset = previous_track_sector_offset;
            largest_track = current_track - 1;
            memcpy(largest_track_mode, previous_track_mode, sizeof(largest_track_mode));
            strcpy(largest_track_file, file);
          }
        }

        /* calculate the true offset and update the counters for the next INDEX marker */
        offset += (int64_t)sector_offset * previous_sector_size;
        previous_sector_size = sector_size;
        previous_index_sector_offset += sector_offset;

        if (index == 1)
        {
          if (verbose_message_callback)
          {
            char message[128];
            char* scan = mode;
            while (*scan && !isspace((unsigned char)*scan))
              ++scan;
            *scan = '\0';

            /* it's undesirable to truncate offset to 32-bits, but %lld isn't defined in c89. */
            snprintf(message, sizeof(message), "Found %s track %d (sector size %d, track starts at %d)",
                     mode, current_track, sector_size, (int)offset);
            verbose_message_callback(message);
          }

          if (current_track == (int)track)
          {
            done = 1;
            break;
          }

          memcpy(previous_track_mode, mode, sizeof(previous_track_mode));
          previous_track_is_data = (memcmp(mode, "MODE", 4) == 0);
          previous_track_sector_offset = offset;

          if (previous_track_is_data && track == RC_HASH_CDTRACK_FIRST_DATA)
          {
            track = current_track;
            done = 1;
            break;
          }
        }
      }
      else if (strncasecmp(ptr, "TRACK ", 6) == 0)
      {
        ptr += 6;
        current_track = atoi(ptr);

        while (*ptr != ' ')
          ++ptr;
        while (*ptr == ' ')
          ++ptr;
        memcpy(mode, ptr, sizeof(mode));

        previous_sector_size = sector_size;

        if (memcmp(mode, "MODE", 4) == 0)
        {
          sector_size = atoi(ptr + 6);
        }
        else
        {
          /* assume AUDIO */
          sector_size = 2352;
        }
      }
      else if (strncasecmp(ptr, "FILE ", 5) == 0)
      {
        if (previous_sector_size > 0)
        {
          /* determine previous track size */
          int sector_count = (int)cdreader_get_bin_size(path, file) / previous_sector_size;
          track_first_sector += sector_count;

          /* if looking for the largest data track, check to see if this one is larger */
          if (track == RC_HASH_CDTRACK_LARGEST && previous_track_is_data)
          {
            if (sector_count > largest_track_sector_count)
            {
              largest_track_sector_count = sector_count;
              largest_track_offset = previous_track_sector_offset;
              largest_track = current_track;
              memcpy(largest_track_mode, previous_track_mode, sizeof(largest_track_mode));
              strcpy(largest_track_file, file);
            }
          }
        }

        ptr += 5;
        ptr2 = ptr;
        if (*ptr == '"')
        {
          ++ptr;
          do
          {
            ++ptr2;
          } while (*ptr2 && *ptr2 != '\n' && *ptr2 != '"');
        }
        else
        {
          do
          {
            ++ptr2;
          } while (*ptr2 && *ptr2 != '\n' && *ptr2 != ' ');
        }

        if (ptr2 - ptr < (int)sizeof(file))
        {
          memcpy(file, ptr, ptr2 - ptr);
          file[ptr2 - ptr] = '\0';
        }
        else
        {
          file[0] = '\0';
        }

        current_track = 0;
        previous_sector_size = 0;
        previous_index_sector_offset = 0;
        offset = 0;
      }

      while (*ptr && *ptr != '\n')
        ++ptr;
    }

    if (done)
      break;

    file_offset += (ptr - buffer);
    rc_file_seek(file_handle, file_offset, SEEK_SET);

  } while (1);

  rc_file_close(file_handle);

  if (track == RC_HASH_CDTRACK_LARGEST)
  {
    previous_track_is_data = (memcmp(mode, "MODE", 4) == 0);
    if (previous_track_is_data)
    {
      int sector_count = (int)cdreader_get_bin_size(path, file) / previous_sector_size;
      sector_count -= previous_index_sector_offset;

      if (sector_count > largest_track_sector_count)
      {
        largest_track_sector_count = sector_count;
        largest_track_offset = previous_track_sector_offset;
        largest_track = current_track;
        memcpy(largest_track_mode, previous_track_mode, sizeof(largest_track_mode));
        strcpy(largest_track_file, file);
      }
    }

    if (largest_track > 0)
    {
      current_track = largest_track;
      track = (uint32_t)largest_track;
      offset = largest_track_offset;
      memcpy(mode, largest_track_mode, sizeof(mode));
      strcpy(file, largest_track_file);
    }
  }

  if (current_track == (int)track)
  {
    cdrom = (struct cdrom_t*)calloc(1, sizeof(*cdrom));
    if (!cdrom)
    {
      snprintf((char*)buffer, sizeof(buffer), "Failed to allocate %u bytes", (unsigned)sizeof(*cdrom));
      rc_hash_error((const char*)buffer);
      return NULL;
    }

    cdrom->first_sector_offset = offset;
    cdrom->first_sector = track_first_sector;

    /* verify existance of bin file */
    bin_filename = cdreader_get_bin_path(path, file);
    if (bin_filename)
    {
      if (cdreader_open_bin(cdrom, bin_filename, mode))
      {
        if (verbose_message_callback)
        {
          if (cdrom->first_sector_offset)
            snprintf((char*)buffer, sizeof(buffer), "Opened track %d (sector size %d, track starts at %d)",
                     track, cdrom->sector_size, (int)cdrom->first_sector_offset);
          else
            snprintf((char*)buffer, sizeof(buffer), "Opened track %d (sector size %d)", track, cdrom->sector_size);

          verbose_message_callback((const char*)buffer);
        }
      }
      else
      {
        snprintf((char*)buffer, sizeof(buffer), "Could not open %s", bin_filename);
        rc_hash_error((const char*)buffer);

        free(cdrom);
        cdrom = NULL;
      }

      free(bin_filename);
    }
  }

  return cdrom;
}

static void* cdreader_open_gdi_track(const char* path, uint32_t track)
{
  void* file_handle;
  char buffer[1024];
  char mode[16] = "MODE1/";
  char sector_size[16];
  char file[256];
  int64_t track_size;
  int track_type;
  char* bin_path = "";
  uint32_t current_track = 0;
  char* ptr, *ptr2, *end;
  int lba = 0;

  uint32_t largest_track = 0;
  int64_t largest_track_size = 0;
  char largest_track_file[256];
  char largest_track_sector_size[16];
  int largest_track_lba = 0;

  int found = 0;
  size_t num_read = 0;
  int64_t file_offset = 0;
  struct cdrom_t* cdrom = NULL;

  file_handle = rc_file_open(path);
  if (!file_handle)
    return NULL;

  file[0] = '\0';
  do
  {
    num_read = rc_file_read(file_handle, buffer, sizeof(buffer) - 1);
    if (num_read == 0)
      break;

    buffer[num_read] = 0;
    if (num_read == sizeof(buffer) - 1)
      end = buffer + sizeof(buffer) * 3 / 4;
    else
      end = buffer + num_read;

    ptr = buffer;

    /* the first line contains the number of tracks, so we can get the last track index from it */
    if (track == RC_HASH_CDTRACK_LAST)
      track = atoi(ptr);

    /* first line contains the number of tracks and will be skipped */
    while (ptr < end)
    {
      /* skip until next newline */
      while (*ptr != '\n' && ptr < end)
        ++ptr;

      /* skip newlines */
      while ((*ptr == '\n' || *ptr == '\r') && ptr < end)
        ++ptr;

      /* line format: [trackid] [lba] [type] [sectorsize] [file] [?] */
      while (isspace((unsigned char)*ptr))
        ++ptr;

      current_track = (uint32_t)atoi(ptr);
      if (track && current_track != track)
        continue;

      while (isdigit((unsigned char)*ptr))
        ++ptr;
      ++ptr;

      while (isspace((unsigned char)*ptr))
        ++ptr;

      lba = atoi(ptr);
      while (isdigit((unsigned char)*ptr))
        ++ptr;
      ++ptr;

      while (isspace((unsigned char)*ptr))
        ++ptr;

      track_type = atoi(ptr);
      while (isdigit((unsigned char)*ptr))
        ++ptr;
      ++ptr;

      while (isspace((unsigned char)*ptr))
        ++ptr;

      ptr2 = sector_size;
      while (isdigit((unsigned char)*ptr))
        *ptr2++ = *ptr++;
      *ptr2 = '\0';
      ++ptr;

      while (isspace((unsigned char)*ptr))
        ++ptr;

      ptr2 = file;
      if (*ptr == '\"')
      {
        ++ptr;
        while (*ptr != '\"')
          *ptr2++ = *ptr++;
        ++ptr;
      }
      else
      {
        while (*ptr != ' ')
          *ptr2++ = *ptr++;
      }
      *ptr2 = '\0';

      if (track == current_track || (track == RC_HASH_CDTRACK_FIRST_DATA && track_type == 4))
      {
        found = 1;
        break;
      }
      else if (track == RC_HASH_CDTRACK_LARGEST && track_type == 4)
      {
        track_size = cdreader_get_bin_size(path, file);
        if (track_size > largest_track_size)
        {
          largest_track_size = track_size;
          largest_track = current_track;
          largest_track_lba = lba;
          strcpy(largest_track_file, file);
          strcpy(largest_track_sector_size, sector_size);
        }
      }
    }

    if (found)
      break;

    file_offset += (ptr - buffer);
    rc_file_seek(file_handle, file_offset, SEEK_SET);

  } while (1);

  rc_file_close(file_handle);

  cdrom = (struct cdrom_t*)calloc(1, sizeof(*cdrom));
  if (!cdrom)
  {
    snprintf((char*)buffer, sizeof(buffer), "Failed to allocate %u bytes", (unsigned)sizeof(*cdrom));
    rc_hash_error((const char*)buffer);
    return NULL;
  }

  /* if we were tracking the largest track, make it the current track.
   * otherwise, current_track will be the requested track, or last track. */
  if (largest_track != 0 && largest_track != current_track)
  {
    current_track = largest_track;
    strcpy(file, largest_track_file);
    strcpy(sector_size, largest_track_sector_size);
    lba = largest_track_lba;
  }

  /* open the bin file for the track - construct mode parameter from sector_size */
  ptr = &mode[6];
  ptr2 = sector_size;
  while (*ptr2 && *ptr2 != '\"')
    *ptr++ = *ptr2++;
  *ptr = '\0';

  bin_path = cdreader_get_bin_path(path, file);
  if (cdreader_open_bin(cdrom, bin_path, mode))
  {
    cdrom->first_sector_offset = 0;
    cdrom->first_sector = lba;

    if (verbose_message_callback)
    {
      snprintf((char*)buffer, sizeof(buffer), "Opened track %d (sector size %d)", track, cdrom->sector_size);
      verbose_message_callback((const char*)buffer);
    }
  }
  else
  {
    snprintf((char*)buffer, sizeof(buffer), "Could not open %s", bin_path);
    rc_hash_error((const char*)buffer);

    free(cdrom);
    cdrom = NULL;
  }

  free(bin_path);

  return cdrom;
}

static void* cdreader_open_track(const char* path, uint32_t track)
{
  /* backwards compatibility - 0 used to mean largest */
  if (track == 0)
    track = RC_HASH_CDTRACK_LARGEST;

  if (rc_path_compare_extension(path, "cue"))
    return cdreader_open_cue_track(path, track);
  if (rc_path_compare_extension(path, "gdi"))
    return cdreader_open_gdi_track(path, track);

  return cdreader_open_bin_track(path, track);
}

static size_t cdreader_read_sector(void* track_handle, uint32_t sector, void* buffer, size_t requested_bytes)
{
  int64_t sector_start;
  size_t num_read, total_read = 0;
  uint8_t* buffer_ptr = (uint8_t*)buffer;

  struct cdrom_t* cdrom = (struct cdrom_t*)track_handle;
  if (!cdrom)
    return 0;

  sector_start = (int64_t)sector * cdrom->sector_size + cdrom->sector_header_size + cdrom->first_sector_offset;

  while (requested_bytes > 2048)
  {
    rc_file_seek(cdrom->file_handle, sector_start, SEEK_SET);
    num_read = rc_file_read(cdrom->file_handle, buffer_ptr, 2048);
    total_read += num_read;

    if (num_read < 2048)
      return total_read;

    buffer_ptr += 2048;
    sector_start += cdrom->sector_size;
    requested_bytes -= 2048;
  }

  rc_file_seek(cdrom->file_handle, sector_start, SEEK_SET);
  num_read = rc_file_read(cdrom->file_handle, buffer_ptr, (int)requested_bytes);
  total_read += num_read;

  return total_read;
}

static void cdreader_close_track(void* track_handle)
{
  struct cdrom_t* cdrom = (struct cdrom_t*)track_handle;
  if (cdrom)
  {
    if (cdrom->file_handle)
      rc_file_close(cdrom->file_handle);

    free(track_handle);
  }
}

static uint32_t cdreader_absolute_sector_to_track_sector(void* track_handle, uint32_t sector)
{
  struct cdrom_t* cdrom = (struct cdrom_t*)track_handle;
  if (cdrom)
    return sector - cdrom->first_sector;

  return 0;
}

void rc_hash_init_default_cdreader()
{
  struct rc_hash_cdreader cdreader;

  cdreader.open_track = cdreader_open_track;
  cdreader.read_sector = cdreader_read_sector;
  cdreader.close_track = cdreader_close_track;
  cdreader.absolute_sector_to_track_sector = cdreader_absolute_sector_to_track_sector;

  rc_hash_init_custom_cdreader(&cdreader);
}
