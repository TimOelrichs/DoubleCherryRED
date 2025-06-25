
/***************************/
/* Palette Switching START */
/***************************/

static bool internal_palette_active = false;
static size_t internal_palette_index = 0;
static unsigned palette_switch_counter = 0;

/* Period in frames between palette switches
 * when holding RetroPad L/R */
#define PALETTE_SWITCH_PERIOD 30

 /* Note: These must be updated if the internal
  * palette options in libretro_core_options.h
  * are changed
  * > We could count the palettes at runtime,
  *   but this adds unnecessary performance
  *   overheads and seems futile given that
  *   a number of other parameters must be
  *   hardcoded anyway... */
#define NUM_PALETTES_DEFAULT       51
#define NUM_PALETTES_TWB64_1      100
#define NUM_PALETTES_TWB64_2      100
#define NUM_PALETTES_PIXELSHIFT_1  45
#define NUM_PALETTES_TOTAL        (NUM_PALETTES_DEFAULT + NUM_PALETTES_TWB64_1 + NUM_PALETTES_TWB64_2 + NUM_PALETTES_PIXELSHIFT_1)

struct retro_core_option_value* palettes_default_opt_values = NULL;
struct retro_core_option_value* palettes_twb64_1_opt_values = NULL;
struct retro_core_option_value* palettes_twb64_2_opt_values = NULL;
struct retro_core_option_value* palettes_pixelshift_1_opt_values = NULL;

static const char* internal_palette_labels[NUM_PALETTES_TOTAL] = { 0 };

static size_t* palettes_default_index_map = NULL;
static size_t* palettes_twb64_1_index_map = NULL;
static size_t* palettes_twb64_2_index_map = NULL;
static size_t* palettes_pixelshift_1_index_map = NULL;

static void parse_internal_palette_values(const char* key,
    struct retro_core_option_v2_definition* opt_defs_intl,
    size_t num_palettes, size_t palette_offset,
    struct retro_core_option_value** opt_values,
    size_t** index_map)
{
    size_t i;
    struct retro_core_option_v2_definition* opt_defs = option_defs_us;
    struct retro_core_option_v2_definition* opt_def = NULL;
    size_t label_index = 0;
#ifndef HAVE_NO_LANGEXTRA
    struct retro_core_option_v2_definition* opt_def_intl = NULL;
#endif
    /* Find option corresponding to key */
    for (opt_def = opt_defs; !string_is_empty(opt_def->key); opt_def++)
        if (string_is_equal(opt_def->key, key))
            break;

    /* Cache option values array for fast access
     * when setting palette index */
    *opt_values = opt_def->values;

    /* Loop over all palette values for specified
     * option and:
     * - Generate palette index maps
     * - Fetch palette labels for notification
     *   purposes
     * Note: We perform no error checking here,
     * since we are operating on hardcoded structs
     * over which the core has full control */
    for (i = 0; i < num_palettes; i++)
    {
        const char* value = opt_def->values[i].value;
        const char* value_label = NULL;

        /* Add entry to hash map
         * > Note that we have to set index+1, since
         *   a return value of 0 from RHMAP_GET_STR()
         *   indicates that the key string was not found */
        RHMAP_SET_STR((*index_map), value, i + 1);

        /* Check if we have a localised palette label */
#ifndef HAVE_NO_LANGEXTRA
        if (opt_defs_intl)
        {
            /* Find localised option corresponding to key */
            for (opt_def_intl = opt_defs_intl;
                !string_is_empty(opt_def_intl->key);
                opt_def_intl++)
            {
                if (string_is_equal(opt_def_intl->key, key))
                {
                    size_t j = 0;

                    /* Search for current option value */
                    for (;;)
                    {
                        const char* value_intl = opt_def_intl->values[j].value;

                        if (string_is_empty(value_intl))
                            break;

                        if (string_is_equal(value, value_intl))
                        {
                            /* We have a match; fetch localised label */
                            value_label = opt_def_intl->values[j].label;
                            break;
                        }

                        j++;
                    }

                    break;
                }
            }
        }
#endif
        /* If localised palette label is unset,
         * use value itself from option_defs_us */
        if (!value_label)
            value_label = value;

        /* Cache label for 'consolidated' palette index */
        internal_palette_labels[palette_offset + label_index++] = value_label;
    }
}

static void init_palette_switch(void)
{
    struct retro_core_option_v2_definition* opt_defs_intl = NULL;
#ifndef HAVE_NO_LANGEXTRA
    unsigned language = 0;
#endif

    libretro_supports_set_variable = false;
    if (environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, NULL))
        libretro_supports_set_variable = true;

    libretro_msg_interface_version = 0;
    environ_cb(RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION,
        &libretro_msg_interface_version);

    internal_palette_active = false;
    internal_palette_index = 0;
    palette_switch_counter = 0;

#ifndef HAVE_NO_LANGEXTRA
    if (environ_cb(RETRO_ENVIRONMENT_GET_LANGUAGE, &language) &&
        (language < RETRO_LANGUAGE_LAST) &&
        (language != RETRO_LANGUAGE_ENGLISH) &&
        options_intl[language])
        opt_defs_intl = options_intl[language]->definitions;
#endif

    /* Parse palette values for each palette group
     * > Default palettes */
    parse_internal_palette_values("gambatte_gb_internal_palette",
        opt_defs_intl, NUM_PALETTES_DEFAULT,
        0,
        &palettes_default_opt_values,
        &palettes_default_index_map);
    /* > TWB64 Pack 1 palettes */
    parse_internal_palette_values("gambatte_gb_palette_twb64_1",
        opt_defs_intl, NUM_PALETTES_TWB64_1,
        NUM_PALETTES_DEFAULT,
        &palettes_twb64_1_opt_values,
        &palettes_twb64_1_index_map);
    /* > TWB64 Pack 2 palettes */
    parse_internal_palette_values("gambatte_gb_palette_twb64_2",
        opt_defs_intl, NUM_PALETTES_TWB64_2,
        NUM_PALETTES_DEFAULT + NUM_PALETTES_TWB64_1,
        &palettes_twb64_2_opt_values,
        &palettes_twb64_2_index_map);
    /* > PixelShift - Pack 1 palettes */
    parse_internal_palette_values("gambatte_gb_palette_pixelshift_1",
        opt_defs_intl, NUM_PALETTES_PIXELSHIFT_1,
        NUM_PALETTES_DEFAULT + NUM_PALETTES_TWB64_1 + NUM_PALETTES_TWB64_2,
        &palettes_pixelshift_1_opt_values,
        &palettes_pixelshift_1_index_map);
}

static void deinit_palette_switch(void)
{
    libretro_supports_set_variable = false;
    libretro_msg_interface_version = 0;
    internal_palette_active = false;
    internal_palette_index = 0;
    palette_switch_counter = 0;
    palettes_default_opt_values = NULL;
    palettes_twb64_1_opt_values = NULL;
    palettes_twb64_2_opt_values = NULL;
    palettes_pixelshift_1_opt_values = NULL;

    RHMAP_FREE(palettes_default_index_map);
    RHMAP_FREE(palettes_twb64_1_index_map);
    RHMAP_FREE(palettes_twb64_2_index_map);
    RHMAP_FREE(palettes_pixelshift_1_index_map);
}

static void palette_switch_set_index(size_t palette_index)
{
    const char* palettes_default_value = NULL;
    const char* palettes_ext_key = NULL;
    const char* palettes_ext_value = NULL;
    size_t opt_index = 0;
    struct retro_variable var = { 0 };

    if (palette_index >= NUM_PALETTES_TOTAL)
        palette_index = NUM_PALETTES_TOTAL - 1;

    /* Check which palette group the specified
     * index corresponds to */
    if (palette_index < NUM_PALETTES_DEFAULT)
    {
        /* This is a palette from the default group */
        opt_index = palette_index;
        palettes_default_value = palettes_default_opt_values[opt_index].value;
    }
    else if (palette_index <
        NUM_PALETTES_DEFAULT + NUM_PALETTES_TWB64_1)
    {
        /* This is a palette from the TWB64 Pack 1 group */
        palettes_default_value = "TWB64 - Pack 1";

        opt_index = palette_index - NUM_PALETTES_DEFAULT;
        palettes_ext_key = "gambatte_gb_palette_twb64_1";
        palettes_ext_value = palettes_twb64_1_opt_values[opt_index].value;
    }
    else if (palette_index <
        NUM_PALETTES_DEFAULT + NUM_PALETTES_TWB64_1 + NUM_PALETTES_TWB64_2)
    {
        /* This is a palette from the TWB64 Pack 2 group */
        palettes_default_value = "TWB64 - Pack 2";

        opt_index = palette_index -
            (NUM_PALETTES_DEFAULT + NUM_PALETTES_TWB64_1);
        palettes_ext_key = "gambatte_gb_palette_twb64_2";
        palettes_ext_value = palettes_twb64_2_opt_values[opt_index].value;
    }
    else
    {
        /* This is a palette from the PixelShift Pack 1 group */
        palettes_default_value = "PixelShift - Pack 1";

        opt_index = palette_index -
            (NUM_PALETTES_DEFAULT + NUM_PALETTES_TWB64_1 + NUM_PALETTES_TWB64_2);
        palettes_ext_key = "gambatte_gb_palette_pixelshift_1";
        palettes_ext_value = palettes_pixelshift_1_opt_values[opt_index].value;
    }

    /* Notify frontend of option value changes */
    var.key = "gambatte_gb_internal_palette";
    var.value = palettes_default_value;
    environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &var);

    if (palettes_ext_key)
    {
        var.key = palettes_ext_key;
        var.value = palettes_ext_value;
        environ_cb(RETRO_ENVIRONMENT_SET_VARIABLE, &var);
    }

    /* Display notification message */
    if (libretro_msg_interface_version >= 1)
    {
        struct retro_message_ext msg = {
           internal_palette_labels[palette_index],
           2000,
           1,
           RETRO_LOG_INFO,
           RETRO_MESSAGE_TARGET_OSD,
           RETRO_MESSAGE_TYPE_NOTIFICATION_ALT,
           -1
        };
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE_EXT, &msg);
    }
    else
    {
        struct retro_message msg = {
           internal_palette_labels[palette_index],
           120
        };
        environ_cb(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
    }
}

/*************************/
/* Palette Switching END */
/*************************/
