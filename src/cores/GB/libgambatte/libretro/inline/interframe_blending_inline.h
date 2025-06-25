
/*****************************/
/* Interframe blending START */
/*****************************/

#define LCD_RESPONSE_TIME 0.333f
/* > 'LCD Ghosting (Fast)' method does not
 *   correctly interpret the set response time,
 *   leading to an artificially subdued blur effect.
 *   We have to compensate for this by increasing
 *   the response time, hence this 'fake' value */
#define LCD_RESPONSE_TIME_FAKE 0.5f

enum frame_blend_method
{
    FRAME_BLEND_NONE = 0,
    FRAME_BLEND_MIX,
    FRAME_BLEND_LCD_GHOSTING,
    FRAME_BLEND_LCD_GHOSTING_FAST
};

static enum frame_blend_method frame_blend_type = FRAME_BLEND_NONE;
static gambatte::video_pixel_t* video_buf_prev_1 = NULL;
static gambatte::video_pixel_t* video_buf_prev_2 = NULL;
static gambatte::video_pixel_t* video_buf_prev_3 = NULL;
static gambatte::video_pixel_t* video_buf_prev_4 = NULL;
static float* video_buf_acc_r = NULL;
static float* video_buf_acc_g = NULL;
static float* video_buf_acc_b = NULL;
static float frame_blend_response[4] = { 0.0f };
static bool frame_blend_response_set = false;
static void (*blend_frames)(void) = NULL;

/* > Note: The individual frame blending functions
 *   are somewhat WET (Write Everything Twice), in that
 *   we duplicate the entire nested for loop.
 *   This code is performance-critical, so we want to
 *   minimise logic in the inner loops where possible  */
static void blend_frames_mix(void)
{
    gambatte::video_pixel_t* curr = video_buf;
    gambatte::video_pixel_t* prev = video_buf_prev_1;
    size_t x, y;

    for (y = 0; y < VIDEO_HEIGHT; y++)
    {
        for (x = 0; x < VIDEO_WIDTH; x++)
        {
            /* Get colours from current + previous frames */
            gambatte::video_pixel_t rgb_curr = *(curr + x);
            gambatte::video_pixel_t rgb_prev = *(prev + x);

            /* Store colours for next frame */
            *(prev + x) = rgb_curr;

            /* Mix colours
             * > "Mixing Packed RGB Pixels Efficiently"
             *   http://blargg.8bitalley.com/info/rgb_mixing.html */
#ifdef VIDEO_RGB565
            * (curr + x) = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x821)) >> 1;
#elif defined(VIDEO_ABGR1555)
            * (curr + x) = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x521)) >> 1;
#else
            * (curr + x) = (rgb_curr + rgb_prev + ((rgb_curr ^ rgb_prev) & 0x10101)) >> 1;
#endif
        }

        curr += VIDEO_PITCH;
        prev += VIDEO_PITCH;
    }
}

static void blend_frames_lcd_ghost(void)
{
    gambatte::video_pixel_t* curr = video_buf;
    gambatte::video_pixel_t* prev_1 = video_buf_prev_1;
    gambatte::video_pixel_t* prev_2 = video_buf_prev_2;
    gambatte::video_pixel_t* prev_3 = video_buf_prev_3;
    gambatte::video_pixel_t* prev_4 = video_buf_prev_4;
    float* response = frame_blend_response;
    size_t x, y;

    for (y = 0; y < VIDEO_HEIGHT; y++)
    {
        for (x = 0; x < VIDEO_WIDTH; x++)
        {
            /* Get colours from current + previous frames */
            gambatte::video_pixel_t rgb_curr = *(curr + x);
            gambatte::video_pixel_t rgb_prev_1 = *(prev_1 + x);
            gambatte::video_pixel_t rgb_prev_2 = *(prev_2 + x);
            gambatte::video_pixel_t rgb_prev_3 = *(prev_3 + x);
            gambatte::video_pixel_t rgb_prev_4 = *(prev_4 + x);

            /* Store colours for next frame */
            *(prev_1 + x) = rgb_curr;
            *(prev_2 + x) = rgb_prev_1;
            *(prev_3 + x) = rgb_prev_2;
            *(prev_4 + x) = rgb_prev_3;

            /* Unpack colours and convert to float */
#ifdef VIDEO_RGB565
            float r_curr = static_cast<float>(rgb_curr >> 11 & 0x1F);
            float g_curr = static_cast<float>(rgb_curr >> 6 & 0x1F);
            float b_curr = static_cast<float>(rgb_curr & 0x1F);

            float r_prev_1 = static_cast<float>(rgb_prev_1 >> 11 & 0x1F);
            float g_prev_1 = static_cast<float>(rgb_prev_1 >> 6 & 0x1F);
            float b_prev_1 = static_cast<float>(rgb_prev_1 & 0x1F);

            float r_prev_2 = static_cast<float>(rgb_prev_2 >> 11 & 0x1F);
            float g_prev_2 = static_cast<float>(rgb_prev_2 >> 6 & 0x1F);
            float b_prev_2 = static_cast<float>(rgb_prev_2 & 0x1F);

            float r_prev_3 = static_cast<float>(rgb_prev_3 >> 11 & 0x1F);
            float g_prev_3 = static_cast<float>(rgb_prev_3 >> 6 & 0x1F);
            float b_prev_3 = static_cast<float>(rgb_prev_3 & 0x1F);

            float r_prev_4 = static_cast<float>(rgb_prev_4 >> 11 & 0x1F);
            float g_prev_4 = static_cast<float>(rgb_prev_4 >> 6 & 0x1F);
            float b_prev_4 = static_cast<float>(rgb_prev_4 & 0x1F);
#elif defined(VIDEO_ABGR1555)
            float r_curr = static_cast<float>(rgb_curr & 0x1F);
            float g_curr = static_cast<float>(rgb_curr >> 5 & 0x1F);
            float b_curr = static_cast<float>(rgb_curr >> 10 & 0x1F);

            float r_prev_1 = static_cast<float>(rgb_prev_1 & 0x1F);
            float g_prev_1 = static_cast<float>(rgb_prev_1 >> 5 & 0x1F);
            float b_prev_1 = static_cast<float>(rgb_prev_1 >> 10 & 0x1F);

            float r_prev_2 = static_cast<float>(rgb_prev_2 & 0x1F);
            float g_prev_2 = static_cast<float>(rgb_prev_2 >> 5 & 0x1F);
            float b_prev_2 = static_cast<float>(rgb_prev_2 >> 10 & 0x1F);

            float r_prev_3 = static_cast<float>(rgb_prev_3 & 0x1F);
            float g_prev_3 = static_cast<float>(rgb_prev_3 >> 5 & 0x1F);
            float b_prev_3 = static_cast<float>(rgb_prev_3 >> 10 & 0x1F);

            float r_prev_4 = static_cast<float>(rgb_prev_4 & 0x1F);
            float g_prev_4 = static_cast<float>(rgb_prev_4 >> 5 & 0x1F);
            float b_prev_4 = static_cast<float>(rgb_prev_4 >> 10 & 0x1F);
#else
            float r_curr = static_cast<float>(rgb_curr >> 16 & 0x1F);
            float g_curr = static_cast<float>(rgb_curr >> 8 & 0x1F);
            float b_curr = static_cast<float>(rgb_curr & 0x1F);

            float r_prev_1 = static_cast<float>(rgb_prev_1 >> 16 & 0x1F);
            float g_prev_1 = static_cast<float>(rgb_prev_1 >> 8 & 0x1F);
            float b_prev_1 = static_cast<float>(rgb_prev_1 & 0x1F);

            float r_prev_2 = static_cast<float>(rgb_prev_2 >> 16 & 0x1F);
            float g_prev_2 = static_cast<float>(rgb_prev_2 >> 8 & 0x1F);
            float b_prev_2 = static_cast<float>(rgb_prev_2 & 0x1F);

            float r_prev_3 = static_cast<float>(rgb_prev_3 >> 16 & 0x1F);
            float g_prev_3 = static_cast<float>(rgb_prev_3 >> 8 & 0x1F);
            float b_prev_3 = static_cast<float>(rgb_prev_3 & 0x1F);

            float r_prev_4 = static_cast<float>(rgb_prev_4 >> 16 & 0x1F);
            float g_prev_4 = static_cast<float>(rgb_prev_4 >> 8 & 0x1F);
            float b_prev_4 = static_cast<float>(rgb_prev_4 & 0x1F);
#endif
            /* Mix colours for current frame and convert back to video_pixel_t
             * > Response time effect implemented via an exponential
             *   drop-off algorithm, taken from the 'Gameboy Classic Shader'
             *   by Harlequin:
             *      https://github.com/libretro/glsl-shaders/blob/master/handheld/shaders/gameboy/shader-files/gb-pass0.glsl */
            r_curr += (r_prev_1 - r_curr) * *response;
            r_curr += (r_prev_2 - r_curr) * *(response + 1);
            r_curr += (r_prev_3 - r_curr) * *(response + 2);
            r_curr += (r_prev_4 - r_curr) * *(response + 3);
            gambatte::video_pixel_t r_mix = static_cast<gambatte::video_pixel_t>(r_curr + 0.5f) & 0x1F;

            g_curr += (g_prev_1 - g_curr) * *response;
            g_curr += (g_prev_2 - g_curr) * *(response + 1);
            g_curr += (g_prev_3 - g_curr) * *(response + 2);
            g_curr += (g_prev_4 - g_curr) * *(response + 3);
            gambatte::video_pixel_t g_mix = static_cast<gambatte::video_pixel_t>(g_curr + 0.5f) & 0x1F;

            b_curr += (b_prev_1 - b_curr) * *response;
            b_curr += (b_prev_2 - b_curr) * *(response + 1);
            b_curr += (b_prev_3 - b_curr) * *(response + 2);
            b_curr += (b_prev_4 - b_curr) * *(response + 3);
            gambatte::video_pixel_t b_mix = static_cast<gambatte::video_pixel_t>(b_curr + 0.5f) & 0x1F;

            /* Repack colours for current frame */
#ifdef VIDEO_RGB565
            * (curr + x) = r_mix << 11 | g_mix << 6 | b_mix;
#elif defined(VIDEO_ABGR1555)
            * (curr + x) = b_mix << 10 | g_mix << 5 | b_mix;
#else
            * (curr + x) = r_mix << 16 | g_mix << 8 | b_mix;
#endif
        }

        curr += VIDEO_PITCH;
        prev_1 += VIDEO_PITCH;
        prev_2 += VIDEO_PITCH;
        prev_3 += VIDEO_PITCH;
        prev_4 += VIDEO_PITCH;
    }
}

static void blend_frames_lcd_ghost_fast(void)
{
    gambatte::video_pixel_t* curr = video_buf;
    float* prev_r = video_buf_acc_r;
    float* prev_g = video_buf_acc_g;
    float* prev_b = video_buf_acc_b;
    size_t x, y;

    for (y = 0; y < VIDEO_HEIGHT; y++)
    {
        for (x = 0; x < VIDEO_WIDTH; x++)
        {
            /* Get colours from current + previous frames */
            gambatte::video_pixel_t rgb_curr = *(curr + x);
            float r_prev = *(prev_r + x);
            float g_prev = *(prev_g + x);
            float b_prev = *(prev_b + x);

            /* Unpack current colours and convert to float */
#ifdef VIDEO_RGB565
            float r_curr = static_cast<float>(rgb_curr >> 11 & 0x1F);
            float g_curr = static_cast<float>(rgb_curr >> 6 & 0x1F);
            float b_curr = static_cast<float>(rgb_curr & 0x1F);
#elif defined(VIDEO_ABGR1555)
            float r_curr = static_cast<float>(rgb_curr & 0x1F);
            float g_curr = static_cast<float>(rgb_curr >> 5 & 0x1F);
            float b_curr = static_cast<float>(rgb_curr >> 10 & 0x1F);
#else
            float r_curr = static_cast<float>(rgb_curr >> 16 & 0x1F);
            float g_curr = static_cast<float>(rgb_curr >> 8 & 0x1F);
            float b_curr = static_cast<float>(rgb_curr & 0x1F);
#endif
            /* Mix colours for current frame */
            float r_mix = (r_curr * (1.0f - LCD_RESPONSE_TIME_FAKE)) + (LCD_RESPONSE_TIME_FAKE * r_prev);
            float g_mix = (g_curr * (1.0f - LCD_RESPONSE_TIME_FAKE)) + (LCD_RESPONSE_TIME_FAKE * g_prev);
            float b_mix = (b_curr * (1.0f - LCD_RESPONSE_TIME_FAKE)) + (LCD_RESPONSE_TIME_FAKE * b_prev);

            /* Store colours for next frame */
            *(prev_r + x) = r_mix;
            *(prev_g + x) = g_mix;
            *(prev_b + x) = b_mix;

            /* Convert, repack and assign colours for current frame */
#ifdef VIDEO_RGB565
            * (curr + x) = (static_cast<gambatte::video_pixel_t>(r_mix + 0.5f) & 0x1F) << 11
                | (static_cast<gambatte::video_pixel_t>(g_mix + 0.5f) & 0x1F) << 6
                | (static_cast<gambatte::video_pixel_t>(b_mix + 0.5f) & 0x1F);
#elif defined(ABGR1555)
            * (curr + x) = (static_cast<gambatte::video_pixel_t>(r_mix + 0.5f) & 0x1F)
                | (static_cast<gambatte::video_pixel_t>(g_mix + 0.5f) & 0x1F) << 5
                | (static_cast<gambatte::video_pixel_t>(b_mix + 0.5f) & 0x1F) << 10;
#else
            * (curr + x) = (static_cast<gambatte::video_pixel_t>(r_mix + 0.5f) & 0x1F) << 16
                | (static_cast<gambatte::video_pixel_t>(g_mix + 0.5f) & 0x1F) << 8
                | (static_cast<gambatte::video_pixel_t>(b_mix + 0.5f) & 0x1F);
#endif
        }

        curr += VIDEO_PITCH;
        prev_r += VIDEO_PITCH;
        prev_g += VIDEO_PITCH;
        prev_b += VIDEO_PITCH;
    }
}

static bool allocate_video_buf_prev(gambatte::video_pixel_t** buf)
{
    if (!*buf)
    {
        *buf = (gambatte::video_pixel_t*)malloc(VIDEO_BUFF_SIZE);
        if (!*buf)
            return false;
    }
    memset(*buf, 0, VIDEO_BUFF_SIZE);
    return true;
}

static bool allocate_video_buf_acc(void)
{
    size_t i;
    size_t buf_size = 256 * NUM_GAMEBOYS * VIDEO_HEIGHT * sizeof(float);

    if (!video_buf_acc_r)
    {
        video_buf_acc_r = (float*)malloc(buf_size);
        if (!video_buf_acc_r)
            return false;
    }

    if (!video_buf_acc_g)
    {
        video_buf_acc_g = (float*)malloc(buf_size);
        if (!video_buf_acc_g)
            return false;
    }

    if (!video_buf_acc_b)
    {
        video_buf_acc_b = (float*)malloc(buf_size);
        if (!video_buf_acc_b)
            return false;
    }

    /* Cannot use memset() on arrays of floats... */
    for (i = 0; i < (256 * NUM_GAMEBOYS * VIDEO_HEIGHT); i++)
    {
        video_buf_acc_r[i] = 0.0f;
        video_buf_acc_g[i] = 0.0f;
        video_buf_acc_b[i] = 0.0f;
    }
    return true;
}

static void init_frame_blending(void)
{
    blend_frames = NULL;

    /* Allocate interframe blending buffers, as required
     * NOTE: In all cases, any used buffers are 'reset'
     * to avoid drawing garbage on the next frame */
    switch (frame_blend_type)
    {
    case FRAME_BLEND_MIX:
        /* Simple 50:50 blending requires a single buffer */
        if (!allocate_video_buf_prev(&video_buf_prev_1))
            return;
        break;
    case FRAME_BLEND_LCD_GHOSTING:
        /* 'Accurate' LCD ghosting requires four buffers */
        if (!allocate_video_buf_prev(&video_buf_prev_1))
            return;
        if (!allocate_video_buf_prev(&video_buf_prev_2))
            return;
        if (!allocate_video_buf_prev(&video_buf_prev_3))
            return;
        if (!allocate_video_buf_prev(&video_buf_prev_4))
            return;
        break;
    case FRAME_BLEND_LCD_GHOSTING_FAST:
        /* 'Fast' LCD ghosting requires three (RGB)
         * 'accumulator' buffers */
        if (!allocate_video_buf_acc())
            return;
        break;
    case FRAME_BLEND_NONE:
    default:
        /* Error condition - cannot happen
         * > Just leave blend_frames() function set to NULL */
        return;
    }

    /* Set LCD ghosting response time factors,
     * if required */
    if ((frame_blend_type == FRAME_BLEND_LCD_GHOSTING) &&
        !frame_blend_response_set)
    {
        /* For the default response time of 0.333,
         * only four previous samples are required
         * since the response factor for the fifth
         * is:
         *    pow(LCD_RESPONSE_TIME, 5.0f) -> 0.00409
         * ...which is less than half a percent, and
         * therefore irrelevant.
         * If the response time were significantly
         * increased, we may need to rethink this
         * (but more samples == greater performance
         * overheads) */
        frame_blend_response[0] = LCD_RESPONSE_TIME;
        frame_blend_response[1] = std::pow(LCD_RESPONSE_TIME, 2.0f);
        frame_blend_response[2] = std::pow(LCD_RESPONSE_TIME, 3.0f);
        frame_blend_response[3] = std::pow(LCD_RESPONSE_TIME, 4.0f);

        frame_blend_response_set = true;
    }

    /* Assign frame blending function */
    switch (frame_blend_type)
    {
    case FRAME_BLEND_MIX:
        blend_frames = blend_frames_mix;
        return;
    case FRAME_BLEND_LCD_GHOSTING:
        blend_frames = blend_frames_lcd_ghost;
        return;
    case FRAME_BLEND_LCD_GHOSTING_FAST:
        blend_frames = blend_frames_lcd_ghost_fast;
        return;
    case FRAME_BLEND_NONE:
    default:
        /* Error condition - cannot happen
         * > Just leave blend_frames() function set to NULL */
        return;
    }
}

static void deinit_frame_blending(void)
{
    if (video_buf_prev_1)
    {
        free(video_buf_prev_1);
        video_buf_prev_1 = NULL;
    }

    if (video_buf_prev_2)
    {
        free(video_buf_prev_2);
        video_buf_prev_2 = NULL;
    }

    if (video_buf_prev_3)
    {
        free(video_buf_prev_3);
        video_buf_prev_3 = NULL;
    }

    if (video_buf_prev_4)
    {
        free(video_buf_prev_4);
        video_buf_prev_4 = NULL;
    }

    if (video_buf_acc_r)
    {
        free(video_buf_acc_r);
        video_buf_acc_r = NULL;
    }

    if (video_buf_acc_g)
    {
        free(video_buf_acc_g);
        video_buf_acc_g = NULL;
    }

    if (video_buf_acc_b)
    {
        free(video_buf_acc_b);
        video_buf_acc_b = NULL;
    }

    frame_blend_type = FRAME_BLEND_NONE;
    frame_blend_response_set = false;
}

static void check_frame_blend_variable(void)
{
    struct retro_variable var;
    enum frame_blend_method old_frame_blend_type = frame_blend_type;

    frame_blend_type = FRAME_BLEND_NONE;

    var.key = "gambatte_mix_frames";
    var.value = 0;

    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
    {
        if (!strcmp(var.value, "mix"))
            frame_blend_type = FRAME_BLEND_MIX;
        else if (!strcmp(var.value, "lcd_ghosting"))
            frame_blend_type = FRAME_BLEND_LCD_GHOSTING;
        else if (!strcmp(var.value, "lcd_ghosting_fast"))
            frame_blend_type = FRAME_BLEND_LCD_GHOSTING_FAST;
    }

    if (frame_blend_type == FRAME_BLEND_NONE)
        blend_frames = NULL;
    else if (frame_blend_type != old_frame_blend_type)
        init_frame_blending();
}

/***************************/
/* Interframe blending END */
/***************************/
