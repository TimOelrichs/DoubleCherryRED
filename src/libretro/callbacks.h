// start boilerplate

void retro_get_system_av_info(struct retro_system_av_info* info)
{
    //videoLayoutManager.getAvInfo(info);
}



unsigned retro_api_version(void) { return RETRO_API_VERSION; }
unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

void retro_set_controller_port_device(unsigned port, unsigned device) {}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_set_environment(retro_environment_t cb)
{
    static const struct retro_system_content_info_override content_overrides[] = {
        {
            "gb|dmg|gbc|cgb|sgb", /* extensions */
            false,                /* need_fullpath */
            true                  /* persistent_data */
        },
        {NULL, false, false} };

    environ_cb = cb;

    /* Set core options
 * An annoyance: retro_set_environment() can be called
 * multiple times, and depending upon the current frontend
 * state various environment callbacks may be disabled.
 * This means the reported 'categories_supported' status
 * may change on subsequent iterations. We therefore have
 * to record whether 'categories_supported' is true on any
 * iteration, and latch the result */
 //  libretro_set_core_options(environ_cb, &option_categories);
   //libretro_supports_option_categories |= option_categories;

    cb(RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO, (void*)subsystems);
    /* Request a persistent content data buffer */
    cb(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE,
        (void*)content_overrides);

    struct retro_core_options_v2 options = {
       option_cats_us,
     core_options_us // <-- v2 definitions

    };

    environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
}
