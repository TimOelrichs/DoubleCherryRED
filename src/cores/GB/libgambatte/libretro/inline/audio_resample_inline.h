/*************************/
/* Audio Resampler START */
/*************************/

/* There are 35112 stereo sound samples in a video frame */
#define SOUND_SAMPLES_PER_FRAME   35112
/* We request 2064 samples from each call of GB::runFor() */
#define SOUND_SAMPLES_PER_RUN     2064
/* Native GB/GBC hardware audio sample rate (~2 MHz) */
#define SOUND_SAMPLE_RATE_NATIVE  (VIDEO_REFRESH_RATE * (double)SOUND_SAMPLES_PER_FRAME)

#define SOUND_SAMPLE_RATE_CC      (SOUND_SAMPLE_RATE_NATIVE / CC_DECIMATION_RATE) /* ~64k */
#define SOUND_SAMPLE_RATE_BLIPPER (SOUND_SAMPLE_RATE_NATIVE / 64) /* ~32k */

/* GB::runFor() nominally generates up to
 * (SOUND_SAMPLES_PER_RUN + 2064) samples, which
 * defines our sound buffer size
 * NOTE: This is in fact a lie - in the upstream code,
 * GB::runFor() can generate more than
 * (SOUND_SAMPLES_PER_RUN + 2064) samples, causing a
 * buffer overflow. It has been necessary to add an
 * internal hard cap/bail out in the event that
 * excess samples are detected... */
#define SOUND_BUFF_SIZE         (SOUND_SAMPLES_PER_RUN + 2064)

 /* Blipper produces between 548 and 549 output samples
  * per frame. For safety, we want to keep the blip
  * buffer no more than ~50% full. (2 * 549) = 1098,
  * so add some padding and round up to (1024 + 512) */
#define BLIP_BUFFER_SIZE (1024 + 512)

static blipper_t* resampler_l = NULL;
static blipper_t* resampler_r = NULL;

static bool use_cc_resampler = false;

static int16_t* audio_out_buffer = NULL;
static size_t audio_out_buffer_size = 0;
static size_t audio_out_buffer_pos = 0;
static size_t audio_batch_frames_max = (1 << 16);

static void audio_out_buffer_init(void)
{
    float sample_rate = use_cc_resampler ?
        SOUND_SAMPLE_RATE_CC : SOUND_SAMPLE_RATE_BLIPPER;
    float samples_per_frame = sample_rate / VIDEO_REFRESH_RATE;
    size_t buffer_size = ((size_t)samples_per_frame + 1) << 1;

    /* Create a buffer that is double the required size
     * to minimise the likelihood of resize operations
     * (core tends to produce very brief 'bursts' of high
     * sample counts depending upon the emulated content...) */
    buffer_size = (buffer_size << 1);

    audio_out_buffer = (int16_t*)malloc(buffer_size * sizeof(int16_t));
    audio_out_buffer_size = buffer_size;
    audio_out_buffer_pos = 0;
    audio_batch_frames_max = (1 << 16);
}

static void audio_out_buffer_deinit(void)
{
    if (audio_out_buffer)
        free(audio_out_buffer);

    audio_out_buffer = NULL;
    audio_out_buffer_size = 0;
    audio_out_buffer_pos = 0;
    audio_batch_frames_max = (1 << 16);
}

static INLINE void audio_out_buffer_resize(size_t num_samples)
{
    size_t buffer_capacity = (audio_out_buffer_size -
        audio_out_buffer_pos) >> 1;

    if (buffer_capacity < num_samples)
    {
        int16_t* tmp_buffer = NULL;
        size_t tmp_buffer_size;

        tmp_buffer_size = audio_out_buffer_size +
            ((num_samples - buffer_capacity) << 1);
        tmp_buffer_size = (tmp_buffer_size << 1) - (tmp_buffer_size >> 1);
        tmp_buffer = (int16_t*)malloc(tmp_buffer_size * sizeof(int16_t));

        memcpy(tmp_buffer, audio_out_buffer,
            audio_out_buffer_pos * sizeof(int16_t));

        free(audio_out_buffer);
        audio_out_buffer = tmp_buffer;
        audio_out_buffer_size = tmp_buffer_size;
    }
}

void audio_out_buffer_write(int16_t* samples, size_t num_samples)
{
    audio_out_buffer_resize(num_samples);

    memcpy(audio_out_buffer + audio_out_buffer_pos,
        samples, (num_samples << 1) * sizeof(int16_t));

    audio_out_buffer_pos += num_samples << 1;
}

static void audio_out_buffer_read_blipper(size_t num_samples)
{
    int16_t* audio_out_buffer_ptr = NULL;

    audio_out_buffer_resize(num_samples);
    audio_out_buffer_ptr = audio_out_buffer + audio_out_buffer_pos;

    blipper_read(resampler_l, audio_out_buffer_ptr, num_samples, 2);
    blipper_read(resampler_r, audio_out_buffer_ptr + 1, num_samples, 2);

    audio_out_buffer_pos += num_samples << 1;
}

static void audio_upload_samples(void)
{
    int16_t* audio_out_buffer_ptr = audio_out_buffer;
    size_t num_samples = audio_out_buffer_pos >> 1;

    while (num_samples > 0)
    {
        size_t samples_to_write = (num_samples >
            audio_batch_frames_max) ?
            audio_batch_frames_max :
            num_samples;
        size_t samples_written = audio_batch_cb(
            audio_out_buffer_ptr, samples_to_write);

        if ((samples_written < samples_to_write) &&
            (samples_written > 0))
            audio_batch_frames_max = samples_written;

        num_samples -= samples_to_write;
        audio_out_buffer_ptr += samples_to_write << 1;
    }

    audio_out_buffer_pos = 0;
}

static void blipper_renderaudio(const int16_t* samples, unsigned frames)
{
    if (!frames)
        return;

    blipper_push_samples(resampler_l, samples + 0, frames, 2);
    blipper_push_samples(resampler_r, samples + 1, frames, 2);
}

static void audio_resampler_deinit(void)
{
    if (resampler_l)
        blipper_free(resampler_l);

    if (resampler_r)
        blipper_free(resampler_r);

    resampler_l = NULL;
    resampler_r = NULL;

    audio_out_buffer_deinit();
}

static void audio_resampler_init(bool startup)
{
    if (use_cc_resampler)
        CC_init();
    else
    {
        resampler_l = blipper_new(32, 0.85, 6.5, 64, BLIP_BUFFER_SIZE, NULL);
        resampler_r = blipper_new(32, 0.85, 6.5, 64, BLIP_BUFFER_SIZE, NULL);

        /* It is possible for blipper_new() to fail,
         * must handle errors */
        if (!resampler_l || !resampler_r)
        {
            /* Display warning message */
            if (libretro_msg_interface_version >= 1)
            {
                struct retro_message_ext msg = {
                   "Sinc resampler unsupported on this platform - using Cosine",
                   2000,
                   1,
                   RETRO_LOG_WARN,
                   RETRO_MESSAGE_TARGET_OSD,
                   RETRO_MESSAGE_TYPE_NOTIFICATION,
                   -1
                };
                environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
            }
            else
            {
                struct retro_message msg = {
                   "Sinc resampler unsupported on this platform - using Cosine",
                   120
                };
                environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
            }

            /* Force CC resampler */
            audio_resampler_deinit();
            use_cc_resampler = true;
            CC_init();

            /* Notify frontend of option value change */
            if (libretro_supports_set_variable)
            {
                struct retro_variable var = { 0 };
                var.key = "gambatte_audio_resampler";
                var.value = "cc";
                environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &var);
            }

            /* Notify frontend of sample rate change */
            if (!startup)
            {
                struct retro_system_av_info av_info;
                retro_get_system_av_info(&av_info);
                environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
            }
        }
    }

    audio_out_buffer_init();
}

/***********************/
/* Audio Resampler END */
/***********************/
