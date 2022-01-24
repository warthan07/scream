#include "alsa.h"

extern int verbosity;

#define SNDCHK(call, ret) { \
  if (ret < 0) {            \
    alsa_error(call, ret);  \
    return -1;              \
  }                         \
}

static struct alsa_output_data {
  snd_pcm_chmap_t *channel_map;
  snd_pcm_t *snd;

  receiver_format_t receiver_format;
  unsigned int rate;
  unsigned int bytes_per_sample;

  int latency;
  char *alsa_device;
} ao_data;

void alsa_error(const char *msg, int r)
{
  fprintf(stderr, "%s (%d): %s\n", msg, r, snd_strerror(r));
}

static void snd_error_quiet(const char *file, int line, const char *function, int err, const char *fmt) {}

int dump_alsa_info(snd_pcm_t *pcm)
{
  int ret;
  snd_output_t *log;

  ret = snd_output_stdio_attach(&log, stderr, 0);
  SNDCHK("snd_output_stdio_attach", ret);

  ret = snd_pcm_dump(pcm, log);
  SNDCHK("snd_pcm_dump", ret);

  ret = snd_output_close(log);
  SNDCHK("snd_output_close", ret);

  return 0;
}

void print_supported_pcm_params(const char *output_device) {
  const snd_pcm_format_t formats[] = {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S24_LE, SND_PCM_FORMAT_S32_LE};
  const unsigned int rates[] = {44100, 48000, 88200, 96000, 176400, 192000};

  snd_pcm_t *pcm;
  unsigned int i, min, max;
  int any_rate, err;

  err = snd_pcm_open(&pcm, output_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
  if (err < 0) {
    fprintf(stderr, "Cannot open device '%s': %s\n", output_device, snd_strerror(err));
    return;
  }
  
  if (dump_alsa_info(pcm)!=0) {
    return;
  }

  snd_pcm_hw_params_t *hw_params;
  snd_pcm_hw_params_alloca(&hw_params);
  err = snd_pcm_hw_params_any(pcm, hw_params);
  if (err < 0) {
    fprintf(stderr, "Cannot get hardware parameters: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return;
  }

  fprintf(stderr, "Supported PCM formats:");
  for (i = 0; i < 3; ++i) {
    if (!snd_pcm_hw_params_test_format(pcm, hw_params, formats[i])) {
      fprintf(stderr, " %s", snd_pcm_format_name(formats[i]));
    }    
  }
  fprintf(stderr, "\n");

  err = snd_pcm_hw_params_get_rate_min(hw_params, &min, NULL);
  if (err < 0) {
    fprintf(stderr, "Cannot get minimum rate: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return ;
  }
  err = snd_pcm_hw_params_get_rate_max(hw_params, &max, NULL);
  if (err < 0) {
    fprintf(stderr, "Cannot get maximum rate: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return ;
  }
  fprintf(stderr, "Supported sample rates:");
  if (min == max)
    fprintf(stderr, " %u", min);
  else if (!snd_pcm_hw_params_test_rate(pcm, hw_params, min + 1, 0))
    fprintf(stderr, " %u-%u", min, max);
  else {
    any_rate = 0;
    for (i = 0; i < 6; ++i) {
      if (!snd_pcm_hw_params_test_rate(pcm, hw_params, rates[i], 0)) {
        any_rate = 1;
        fprintf(stderr, " %u", rates[i]);
      }
    }
    if (!any_rate)
      fprintf(stderr, " %u-%u", min, max);
  }
  fprintf(stderr, "\n");

  err = snd_pcm_hw_params_get_channels_min(hw_params, &min);
  if (err < 0) {
    fprintf(stderr, "Cannot get minimum channels count: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return;
  }
  err = snd_pcm_hw_params_get_channels_max(hw_params, &max);
  if (err < 0) {
    fprintf(stderr, "Cannot get maximum channels count: %s\n", snd_strerror(err));
    snd_pcm_close(pcm);
    return;
  }
  fprintf(stderr, "Channels:");
  for (i = min; i <= max; ++i) {
    if (!snd_pcm_hw_params_test_channels(pcm, hw_params, i))
      fprintf(stderr, " %u", i);
  }
  fprintf(stderr, "\n\n");

  snd_pcm_close(pcm);
}

int setup_alsa(snd_pcm_t **psnd, snd_pcm_format_t format, unsigned int rate, unsigned int target_latency_ms, const char *output_device, int channels, snd_pcm_chmap_t **channel_map)
{
  int ret;
  int soft_resample = 1;
  unsigned int latency = target_latency_ms * 1000;

  // We temporarily disable the error handler of ALSA to avoid filling the log in case the device is unavailable.
  snd_lib_error_set_handler((snd_lib_error_handler_t) snd_error_quiet);
  ret = snd_pcm_open(psnd, output_device, SND_PCM_STREAM_PLAYBACK, 0);
  snd_lib_error_set_handler(NULL);
  if (ret == -EBUSY || ret == -ENOENT ) {
    // The device is currently unavailable (already opened by another software, powered-out, etc)
    // but may become available later on, so we return -2 for now and will try again later on.
    return -2;
  }
  else {
    SNDCHK("snd_pcm_open", ret);
  }

  ret = snd_pcm_set_params(*psnd, format, SND_PCM_ACCESS_RW_INTERLEAVED,
                           channels, rate, soft_resample, latency);
  SNDCHK("snd_pcm_set_params", ret);

  ret = snd_pcm_set_chmap(*psnd, *channel_map);
  if (ret == -ENXIO) { // snd_pcm_set_chmap returns -ENXIO if device does not support channel maps at all
    if (channels > 2) { // but it's relevant only above 2 channels
      fprintf(stderr, "Your device doesn't support channel maps. Channels may be in the wrong order.\n");
      // TODO ALSA has a fixed channel order and we have the source channel_map.
      // It's possible to reorder the channels in software. Maybe a place to start is the remap_data function in aplay.c
    }
  }
  else if (ret == -EBADFD) {
    if (channels > 2) {
      fprintf(stderr, "It was not possible to set the channel map. You are limited to use stereo. See https://github.com/duncanthrax/scream/issues/79\n");
    }
  }
  else {
    SNDCHK("snd_pcm_set_chmap", ret);
  }

  return 0;
}

static int close_alsa(snd_pcm_t *snd) {
  int ret;
  if (!snd) return 0;
  ret = snd_pcm_close(snd);
  SNDCHK("snd_pcm_close", ret);
  return 0;
}

int alsa_output_init(int latency, char *alsa_device)
{
  // init receiver format to track changes
  ao_data.receiver_format.sample_rate = 0;
  ao_data.receiver_format.sample_size = 0;
  ao_data.receiver_format.channels = 2;
  ao_data.receiver_format.channel_map = 0x0003;

  ao_data.latency = latency;
  ao_data.alsa_device = alsa_device;
  ao_data.snd = NULL;

  ao_data.channel_map = malloc(sizeof(snd_pcm_chmap_t) + MAX_CHANNELS*sizeof(unsigned int));
  ao_data.channel_map->channels = 2;
  ao_data.channel_map->pos[0] = SND_CHMAP_FL;
  ao_data.channel_map->pos[1] = SND_CHMAP_FR;

  // We do not open the ALSA connection here since the device may be initially unavailable.
  // It will be open on the fly when PCM data arrives, retrying as many times as necessary until the
  // PCM stream is correctly configured. However, we still try to display the supported PCM parameters
  // for the specified ALSA device in case it can be opened (useful for debugging).
  if(verbosity) print_supported_pcm_params(alsa_device);
  return 0;
}

int alsa_output_send(receiver_data_t *data)
{
  if (data->timed_out) {
    if (ao_data.snd!=NULL) {
      close_alsa(ao_data.snd);
      ao_data.snd = NULL;
      if (verbosity) fprintf(stderr, "Closing ALSA connection after timeout\n");
    }
    return 0;
  }

  snd_pcm_format_t format;
  receiver_format_t *rf = &data->format;

  int format_changed = memcmp(&ao_data.receiver_format, rf, sizeof(receiver_format_t));
  if (format_changed || ao_data.snd==NULL) {
    if (format_changed) {
      // audio format changed, reconfigure
      memcpy(&ao_data.receiver_format, rf, sizeof(receiver_format_t));

      ao_data.rate = ((rf->sample_rate >= 128) ? 44100 : 48000) * (rf->sample_rate % 128);
      switch (rf->sample_size) {
        case 16: format = SND_PCM_FORMAT_S16_LE; ao_data.bytes_per_sample = 2; break;
        case 24: format = SND_PCM_FORMAT_S24_3LE; ao_data.bytes_per_sample = 3; break;
        case 32: format = SND_PCM_FORMAT_S32_LE; ao_data.bytes_per_sample = 4; break;
        default:
          if (verbosity)
            fprintf(stderr, "Unsupported sample size %hhu, not playing until next format switch.\n", rf->sample_size);
          ao_data.rate = 0;
      }

      ao_data.channel_map->channels = rf->channels;
      if (rf->channels == 1) {
        ao_data.channel_map->pos[0] = SND_CHMAP_MONO;
      }
      else {
        // k is the key to map a windows SPEAKER_* position to a PA_CHANNEL_POSITION_*
        // it goes from 0 (SPEAKER_FRONT_LEFT) up to 10 (SPEAKER_SIDE_RIGHT) following the order in ksmedia.h
        // the SPEAKER_TOP_* values are not used
        int k = -1;
        for (int i=0; i<rf->channels; i++) {
          for (int j = k+1; j<=10; j++) {// check the channel map bit by bit from lsb to msb, starting from were we left on the previous step
            if ((rf->channel_map >> j) & 0x01) {// if the bit in j position is set then we have the key for this channel
              k = j;
              break;
            }
          }
          // map the key value to a ALSA channel position
          switch (k) {
            case  0: ao_data.channel_map->pos[i] = SND_CHMAP_FL; break;
            case  1: ao_data.channel_map->pos[i] = SND_CHMAP_FR; break;
            case  2: ao_data.channel_map->pos[i] = SND_CHMAP_FC; break;
            case  3: ao_data.channel_map->pos[i] = SND_CHMAP_LFE; break;
            case  4: ao_data.channel_map->pos[i] = SND_CHMAP_RL; break;
            case  5: ao_data.channel_map->pos[i] = SND_CHMAP_RR; break;
            case  6: ao_data.channel_map->pos[i] = SND_CHMAP_FLC; break;
            case  7: ao_data.channel_map->pos[i] = SND_CHMAP_FRC; break;
            case  8: ao_data.channel_map->pos[i] = SND_CHMAP_RC; break;
            case  9: ao_data.channel_map->pos[i] = SND_CHMAP_SL; break;
            case 10: ao_data.channel_map->pos[i] = SND_CHMAP_SR; break;
            default:
              // center is a safe default, at least it's balanced. This shouldn't happen, but it's better to have a fallback
              if (verbosity) {
                fprintf(stderr, "Channel %i could not be mapped. Falling back to 'center'.\n", i);
              }
              ao_data.channel_map->pos[i] = SND_CHMAP_FC;
          }

          if (verbosity) {
            const char *channel_name;
            switch (k) {
              case  0: channel_name = "Front Left"; break;
              case  1: channel_name = "Front Right"; break;
              case  2: channel_name = "Front Center"; break;
              case  3: channel_name = "LFE / Subwoofer"; break;
              case  4: channel_name = "Rear Left"; break;
              case  5: channel_name = "Rear Right"; break;
              case  6: channel_name = "Front-Left Center"; break;
              case  7: channel_name = "Front-Right Center"; break;
              case  8: channel_name = "Rear Center"; break;
              case  9: channel_name = "Side Left"; break;
              case 10: channel_name = "Side Right"; break;
              default:
                channel_name = "Unknown. Setted to Center.";
            }
            fprintf(stderr, "Channel %i mapped to %s\n", i, channel_name);
          }
        }
      }
    }

    if (ao_data.rate) {
      if (ao_data.snd!=NULL) {
        close_alsa(ao_data.snd);
      }
      int ret = setup_alsa(&ao_data.snd, format, ao_data.rate, ao_data.latency, ao_data.alsa_device, rf->channels, &ao_data.channel_map);
      if (ret == -1) {          
        if (verbosity)
          fprintf(stderr, "Unable to set up ALSA with sample rate %u, sample size %hhu and %u channels, not playing until next format switch.\n", ao_data.rate, rf->sample_size, rf->channels);
        ao_data.snd = NULL;
        ao_data.rate = 0;
      }
      else if (ret == -2) {
        // The device is currently unavailable but may become available in the future 
        // without any format changes, so we do not reset ao_data.rate.
        ao_data.snd = NULL;
      }
      else {
        if (verbosity) {
          if (format_changed) {
            fprintf(stderr, "Switched format to sample rate %u, sample size %hhu and %u channels.\n", ao_data.rate, rf->sample_size, rf->channels);
          }
          else {
            fprintf(stderr, "Reopened ALSA with sample rate %u, sample size %hhu and %u channels.\n", ao_data.rate, rf->sample_size, rf->channels);
          }
        }
      }
    }
  }
  
  if (!ao_data.rate || ao_data.snd==NULL) return 0;

  int ret;
  snd_pcm_sframes_t written;

  int i = 0;
  int samples = (data->audio_size) / (ao_data.bytes_per_sample * rf->channels);
  while (i < samples) {
    written = snd_pcm_writei(ao_data.snd, &data->audio[i * ao_data.bytes_per_sample * rf->channels], samples - i);
    if (written < 0) {
      ret = snd_pcm_recover(ao_data.snd, written, 0);
      SNDCHK("snd_pcm_recover", ret);
      return 0;
    } else if (written < samples - i) {
      if (verbosity) fprintf(stderr, "Writing again after short write %ld < %d\n", written, samples - i);
    }
    i += written;
  }

  return 0;
}
