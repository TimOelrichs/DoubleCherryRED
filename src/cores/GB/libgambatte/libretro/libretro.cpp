#include <stdlib.h>

#include <libretro.h>
#include <libretro_core_options.h>
#include "gambatte_log.h"
#include "blipper.h"
#include "cc_resampler.h"
#include "gambatte.h"
#include "gbcpalettes.h"
#include "bootloader.h"
#include "local_serial.h"

#ifdef HAVE_NETWORK
#include "net_serial.h"
#endif


#if defined(__DJGPP__) && defined(__STRICT_ANSI__)
/* keep this above libretro-common includes */
#undef __STRICT_ANSI__
#endif

#ifndef PATH_MAX_LENGTH
#if defined(_XBOX1) || defined(_3DS) || defined(PSP) || defined(PS2) || defined(GEKKO)|| defined(WIIU) || defined(ORBIS) || defined(__PSL1GHT__) || defined(__PS3__)
#define PATH_MAX_LENGTH 512
#else
#define PATH_MAX_LENGTH 4096
#endif
#endif

#include <string/stdstring.h>
#include <file/file_path.h>
#include <streams/file_stream.h>
#include <array/rhmap.h>

#include <cassert>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <string>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <vector>

#ifdef _3DS
extern "C" void* linearMemAlign(size_t size, size_t alignment);
extern "C" void linearFree(void* mem);
#endif

static retro_video_refresh_t video_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static gambatte::video_pixel_t* video_buf;
static std::vector<gambatte::GB*> v_gb;

//static gambatte::GB gb;

static bool libretro_supports_option_categories = false;
static bool libretro_supports_bitmasks          = false;
static bool libretro_supports_set_variable      = false;
static unsigned libretro_msg_interface_version  = 0;
static bool libretro_supports_ff_override       = false;
static bool libretro_ff_enabled                 = false;
static bool libretro_ff_enabled_prev            = false;

static bool show_gb_link_settings = true;

/* Minimum (and default) turbo pulse train
 * is 2 frames ON, 2 frames OFF */
#define TURBO_PERIOD_MIN      4
#define TURBO_PERIOD_MAX      120
#define TURBO_PULSE_WIDTH_MIN 2
#define TURBO_PULSE_WIDTH_MAX 15

static unsigned libretro_input_state[4] = { 0,0,0,0 };
static bool up_down_allowed          = false;
static unsigned turbo_period         = TURBO_PERIOD_MIN;
static unsigned turbo_pulse_width    = TURBO_PULSE_WIDTH_MIN;
static unsigned turbo_a_counter      = 0;
static unsigned turbo_b_counter      = 0;

static bool rom_loaded = false;

//Dual mode runs two GBCs side by side.
//Currently, they load the same ROM, take the same input, and only the left one supports SRAM, cheats, savestates, or sound.
//Can be made useful later, but for now, it's just a tech demo.
//#define DUAL_MODE

#define DUAL_MODE
#ifdef DUAL_MODE
#define NUM_GAMEBOYS 2
#else
#define NUM_GAMEBOYS 1
#endif


bool use_official_bootloader = false;

#define GB_SCREEN_WIDTH 160
#define VIDEO_WIDTH (GB_SCREEN_WIDTH * NUM_GAMEBOYS)
#define VIDEO_HEIGHT 144
/* Video buffer 'width' is 256, not 160 -> assume
 * there is a benefit to making this a power of 2 
 */
#define VIDEO_BUFF_SIZE (256 * NUM_GAMEBOYS * VIDEO_HEIGHT * sizeof(gambatte::video_pixel_t))
#define VIDEO_PITCH (256 * NUM_GAMEBOYS)
#define VIDEO_REFRESH_RATE (4194304.0 / 70224.0)

#include "inline/audio_resample_inline.h"
#include "inline/palette_switching_inline.h"
#include "inline/interframe_blending_inline.h"
#include "inline/rubmble_support_inline.h"



#ifdef HAVE_NETWORK
/* Core options 'update display' callback */
static bool update_option_visibility(void)
{
   struct retro_variable var = {0};
   bool updated              = false;
   unsigned i;

   /* If frontend supports core option categories,
    * then gambatte_show_gb_link_settings is ignored
    * and no options should be hidden */
   if (libretro_supports_option_categories)
      return false;

   var.key = "gambatte_show_gb_link_settings";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      bool show_gb_link_settings_prev = show_gb_link_settings;

      show_gb_link_settings = true;
      if (strcmp(var.value, "disabled") == 0)
         show_gb_link_settings = false;

      if (show_gb_link_settings != show_gb_link_settings_prev)
      {
         struct retro_core_option_display option_display;

         option_display.visible = show_gb_link_settings;

         option_display.key = "gambatte_gb_link_mode";
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

         option_display.key = "gambatte_gb_link_network_port";
         environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);

         for (i = 0; i < 12; i++)
         {
            char key[64] = {0};

            /* Should be using std::to_string() here, but some
             * compilers don't support it... */
            sprintf(key, "%s%u",
                 "gambatte_gb_link_network_server_ip_", i + 1);

            option_display.key = key;

            environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY, &option_display);
         }

         updated = true;
      }
   }

   return updated;
}
#endif

/* Fast forward override */
static void set_fastforward_override(bool fastforward)
{
   struct retro_fastforwarding_override ff_override;

   if (!libretro_supports_ff_override)
      return;

   ff_override.ratio        = -1.0f;
   ff_override.notification = true;

   if (fastforward)
   {
      ff_override.fastforward    = true;
      ff_override.inhibit_toggle = true;
   }
   else
   {
      ff_override.fastforward    = false;
      ff_override.inhibit_toggle = false;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff_override);
}

static bool file_present_in_system(const char *fname)
{
   const char *system_dir = NULL;
   char full_path[PATH_MAX_LENGTH];

   full_path[0] = '\0';

   if (string_is_empty(fname))
      return false;

   /* Get system directory */
   if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) ||
       !system_dir)
   {
      gambatte_log(RETRO_LOG_WARN,
            "No system directory defined, unable to look for '%s'.\n", fname);
      return false;
   }

   fill_pathname_join(full_path, system_dir,
         fname, sizeof(full_path));

   return path_is_valid(full_path);
}

static bool get_bootloader_from_file(void* userdata, bool isgbc, uint8_t* data, uint32_t buf_size)
{
   const char *system_dir = NULL;
   const char *bios_name  = NULL;
   RFILE *bios_file       = NULL;
   int64_t bios_size      = 0;
   int64_t bytes_read     = 0;
   char bios_path[PATH_MAX_LENGTH];

   bios_path[0] = '\0';

   if (!use_official_bootloader)
      return false;

   /* Get system directory */
   if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) ||
       !system_dir)
   {
      gambatte_log(RETRO_LOG_WARN,
            "No system directory defined, unable to look for bootloader.\n");
      return false;
   }

   /* Get BIOS type */
   if (isgbc)
   {
      bios_name = "gbc_bios.bin";
      bios_size = 0x900;
   }
   else
   {
      bios_name = "gb_bios.bin";
      bios_size = 0x100;
   }

   if (bios_size > buf_size)
      return false;

   /* Get BIOS path */
   fill_pathname_join(bios_path, system_dir,
         bios_name, sizeof(bios_path));

   /* Read BIOS file */
   bios_file = filestream_open(bios_path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!bios_file)
      return false;

   bytes_read = filestream_read(bios_file,
         data, bios_size);
   filestream_close(bios_file);

   if (bytes_read != bios_size)
      return false;

   gambatte_log(RETRO_LOG_INFO, "Read bootloader: %s\n", bios_path);

   return true;
}

namespace input
{
   struct map { unsigned snes; unsigned gb; };
   static const map btn_map[] = {
      { RETRO_DEVICE_ID_JOYPAD_A, gambatte::InputGetter::A },
      { RETRO_DEVICE_ID_JOYPAD_B, gambatte::InputGetter::B },
      { RETRO_DEVICE_ID_JOYPAD_SELECT, gambatte::InputGetter::SELECT },
      { RETRO_DEVICE_ID_JOYPAD_START, gambatte::InputGetter::START },
      { RETRO_DEVICE_ID_JOYPAD_RIGHT, gambatte::InputGetter::RIGHT },
      { RETRO_DEVICE_ID_JOYPAD_LEFT, gambatte::InputGetter::LEFT },
      { RETRO_DEVICE_ID_JOYPAD_UP, gambatte::InputGetter::UP },
      { RETRO_DEVICE_ID_JOYPAD_DOWN, gambatte::InputGetter::DOWN },
   };
}

static void update_input_state(void)
{
   unsigned i;
   unsigned res                = 0;
   bool turbo_a                = false;
   bool turbo_b                = false;
   bool palette_prev           = false;
   bool palette_next           = false;
   bool palette_switch_enabled = (libretro_supports_set_variable &&
         internal_palette_active);


   for (int gbi = 0; gbi < NUM_GAMEBOYS; gbi++)
   {
       res = 0;
       if (libretro_supports_bitmasks)
       {
           int16_t ret = input_state_cb(gbi, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);

           for (i = 0; i < sizeof(input::btn_map) / sizeof(input::map); i++)
               res |= (ret & (1 << input::btn_map[i].snes)) ? input::btn_map[i].gb : 0;

           libretro_ff_enabled = libretro_supports_ff_override &&
               (ret & (1 << RETRO_DEVICE_ID_JOYPAD_R2));

           turbo_a = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_X));
           turbo_b = (ret & (1 << RETRO_DEVICE_ID_JOYPAD_Y));

           if (palette_switch_enabled)
           {
               palette_prev = (bool)(ret & (1 << RETRO_DEVICE_ID_JOYPAD_L));
               palette_next = (bool)(ret & (1 << RETRO_DEVICE_ID_JOYPAD_R));
           }
       }
       else
       {
           for (i = 0; i < sizeof(input::btn_map) / sizeof(input::map); i++)
               res |= input_state_cb(gbi, RETRO_DEVICE_JOYPAD, 0, input::btn_map[i].snes) ? input::btn_map[i].gb : 0;

           libretro_ff_enabled = libretro_supports_ff_override &&
               input_state_cb(gbi, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);

           turbo_a = input_state_cb(gbi, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
           turbo_b = input_state_cb(gbi, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);

           if (palette_switch_enabled)
           {
               palette_prev = input_state_cb(gbi, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
               palette_next = input_state_cb(gbi, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
           }
       }

       if (!up_down_allowed)
       {
           if (res & gambatte::InputGetter::UP)
               if (res & gambatte::InputGetter::DOWN)
                   res &= ~(gambatte::InputGetter::UP | gambatte::InputGetter::DOWN);

           if (res & gambatte::InputGetter::LEFT)
               if (res & gambatte::InputGetter::RIGHT)
                   res &= ~(gambatte::InputGetter::LEFT | gambatte::InputGetter::RIGHT);
       }

       /* Handle fast forward button */
       if (libretro_ff_enabled != libretro_ff_enabled_prev)
       {
           set_fastforward_override(libretro_ff_enabled);
           libretro_ff_enabled_prev = libretro_ff_enabled;
       }

       /* Handle turbo buttons */
       if (turbo_a)
       {
           res |= (turbo_a_counter < turbo_pulse_width) ?
               gambatte::InputGetter::A : 0;

           turbo_a_counter++;
           if (turbo_a_counter >= turbo_period)
               turbo_a_counter = 0;
       }
       else
           turbo_a_counter = 0;

       if (turbo_b)
       {
           res |= (turbo_b_counter < turbo_pulse_width) ?
               gambatte::InputGetter::B : 0;

           turbo_b_counter++;
           if (turbo_b_counter >= turbo_period)
               turbo_b_counter = 0;
       }
       else
           turbo_b_counter = 0;

       /* Handle internal palette switching */
       if (palette_prev || palette_next)
       {
           if (palette_switch_counter == 0)
           {
               size_t palette_index = internal_palette_index;

               if (palette_prev)
               {
                   if (palette_index > 0)
                       palette_index--;
                   else
                       palette_index = NUM_PALETTES_TOTAL - 1;
               }
               else /* palette_next */
               {
                   if (palette_index < NUM_PALETTES_TOTAL - 1)
                       palette_index++;
                   else
                       palette_index = 0;
               }

               palette_switch_set_index(palette_index);
           }

           palette_switch_counter++;
           if (palette_switch_counter >= PALETTE_SWITCH_PERIOD)
               palette_switch_counter = 0;
       }
       else
           palette_switch_counter = 0;

       libretro_input_state[gbi] = res;
   }
}

/* gb_input is called multiple times per frame.
 * Determine input state once per frame using
 * update_input_state(), and simply return
 * cached value here */
class SNESInput : public gambatte::InputGetter
{
   public:
       //SNESInput() {}
       SNESInput(int index) {
           this->index = index; 
      }
      unsigned operator()()
      {
         return libretro_input_state[index];
      }
    private:
     int index;
};

static SNESInput* gb_input[NUM_GAMEBOYS];




#ifdef HAVE_NETWORK
enum SerialMode {
   SERIAL_NONE,
   SERIAL_SERVER,
   SERIAL_CLIENT,
   SERIAL_LOCAL,
};
static NetSerial gb_net_serial;
static SerialMode gb_serialMode = SERIAL_NONE;
static int gb_NetworkPort = 12345;
static std::string gb_NetworkClientAddr;
#endif

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "Gambatte";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
#ifdef HAVE_NETWORK
   info->library_version = "v0.5.0-netlink" GIT_VERSION;
#else
   info->library_version = "v0.5.0" GIT_VERSION;
#endif
   info->need_fullpath = false;
   info->block_extract = false;
   info->valid_extensions = "gb|gbc|dmg";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{

   info->geometry.base_width   = VIDEO_WIDTH;
   info->geometry.base_height  = VIDEO_HEIGHT;
   info->geometry.max_width    = VIDEO_WIDTH;
   info->geometry.max_height   = VIDEO_HEIGHT;
   //info->geometry.aspect_ratio = (float)GB_SCREEN_WIDTH / (float)VIDEO_HEIGHT;
   info->geometry.aspect_ratio = (float)VIDEO_WIDTH / (float)VIDEO_HEIGHT;

   info->timing.fps            = VIDEO_REFRESH_RATE;
   info->timing.sample_rate    = use_cc_resampler ?
         SOUND_SAMPLE_RATE_CC : SOUND_SAMPLE_RATE_BLIPPER;
}

static void check_system_specs(void)
{
   unsigned level = 4;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init(void)
{
   struct retro_log_callback log;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      gambatte_log_set_cb(log.log);
   else
      gambatte_log_set_cb(NULL);

   // Using uint_least32_t in an audio interface expecting you to cast to short*? :( Weird stuff.
   assert(sizeof(gambatte::uint_least32_t) == sizeof(uint32_t));

   v_gb.clear();


   for (int i = 0; i < NUM_GAMEBOYS; i++)
   {
       //gb_input[i] = new SNESInput(i);
       v_gb.push_back(new gambatte::GB);
       //v_gb[i]->setInputGetter(&gb_input[i]);
       v_gb[i]->setInputGetter(new SNESInput(i));

   }


/*
   v_gb[0]->setInputGetter(&gb_input);
#ifdef DUAL_MODE
   gb2.setInputGetter(&gb_input);
#endif
   */

#ifdef _3DS
   video_buf = (gambatte::video_pixel_t*)linearMemAlign(VIDEO_BUFF_SIZE, 128);
#else
   video_buf = (gambatte::video_pixel_t*)malloc(VIDEO_BUFF_SIZE);
#endif

   check_system_specs();
   
   //gb/gbc bootloader support

   for (int i = 0; i < NUM_GAMEBOYS; i++)
   {
       v_gb[i]->setBootloaderGetter(get_bootloader_from_file);
   }
   


   // Initialise internal palette maps
   initPaletteMaps();

   // Initialise palette switching functionality
   init_palette_switch();

   struct retro_variable var = {0};
   var.key = "gambatte_gb_bootloader";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         use_official_bootloader = true;
      else
         use_official_bootloader = false;
   }
   else
      use_official_bootloader = false;

   libretro_supports_bitmasks = false;
   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   libretro_supports_ff_override = false;
   if (environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, NULL))
      libretro_supports_ff_override = true;
}

void retro_deinit(void)
{
#ifdef _3DS
   linearFree(video_buf);
#else
   free(video_buf);
#endif
   video_buf = NULL;
   deinit_frame_blending();
   audio_resampler_deinit();

   freePaletteMaps();
   deinit_palette_switch();

   if (libretro_ff_enabled)
      set_fastforward_override(false);

   libretro_supports_option_categories = false;
   libretro_supports_bitmasks          = false;
   libretro_supports_ff_override       = false;
   libretro_ff_enabled                 = false;
   libretro_ff_enabled_prev            = false;

   //libretro_input_state = 0;
   up_down_allowed      = false;
   turbo_period         = TURBO_PERIOD_MIN;
   turbo_pulse_width    = TURBO_PULSE_WIDTH_MIN;
   turbo_a_counter      = 0;
   turbo_b_counter      = 0;

   deactivate_rumble();
   memset(&rumble, 0, sizeof(struct retro_rumble_interface));
   rumble_level = 0;
}

void retro_set_environment(retro_environment_t cb)
{
   struct retro_vfs_interface_info vfs_iface_info;
   bool option_categories = false;
   environ_cb = cb;

   /* Set core options
    * An annoyance: retro_set_environment() can be called
    * multiple times, and depending upon the current frontend
    * state various environment callbacks may be disabled.
    * This means the reported 'categories_supported' status
    * may change on subsequent iterations. We therefore have
    * to record whether 'categories_supported' is true on any
    * iteration, and latch the result */
   libretro_set_core_options(environ_cb, &option_categories);
   libretro_supports_option_categories |= option_categories;

//#ifdef HAVE_NETWORK
   /* If frontend supports core option categories,
    * gambatte_show_gb_link_settings is unused and
    * should be hidden */
   if (libretro_supports_option_categories)
   {
      struct retro_core_option_display option_display;

      option_display.visible = false;
      option_display.key     = "gambatte_show_gb_link_settings";

      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY,
            &option_display);
   }
   /* If frontend does not support core option
    * categories, core options may be shown/hidden
    * at runtime. In this case, register 'update
    * display' callback, so frontend can update
    * core options menu without calling retro_run() */
   else
   {
      struct retro_core_options_update_display_callback update_display_cb;
      update_display_cb.callback = update_option_visibility;

      environ_cb(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK,
            &update_display_cb);
   }


   vfs_iface_info.required_interface_version = 2;
   vfs_iface_info.iface                      = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
	   filestream_vfs_init(&vfs_iface_info);
}

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_reset()
{
   

   for (int i = 0; i < NUM_GAMEBOYS; i++)
   {

       // gambatte seems to clear out SRAM on reset.
       uint8_t* sram = 0;
       uint8_t* rtc = 0;

       if (v_gb[i]->savedata_size())
       {
           sram = new uint8_t[v_gb[i]->savedata_size()];
           memcpy(sram, v_gb[i]->savedata_ptr(), v_gb[i]->savedata_size());
       }
       if (v_gb[i]->rtcdata_size())
       {
           rtc = new uint8_t[v_gb[i]->rtcdata_size()];
           memcpy(rtc, v_gb[i]->rtcdata_ptr(), v_gb[i]->rtcdata_size());
       }

       v_gb[i]->reset();

       if (sram)
       {
           memcpy(v_gb[i]->savedata_ptr(), sram, v_gb[i]->savedata_size());
           delete[] sram;
       }
       if (rtc)
       {
           memcpy(v_gb[i]->rtcdata_ptr(), rtc, v_gb[i]->rtcdata_size());
           delete[] rtc;
       }

   }


  
}

static size_t serialize_size = 0;
size_t retro_serialize_size(void)
{
   return v_gb[0]->stateSize();
}

bool retro_serialize(void *data, size_t size)
{
   serialize_size = retro_serialize_size();

   if (size != serialize_size)
      return false;

   v_gb[0]->saveState(data);
   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   serialize_size = retro_serialize_size();

   if (size != serialize_size)
   {
       printf("savestate size doesn't match %d : %d\n ", size, serialize_size);
       return false;
   }

   v_gb[0]->loadState(data);
   return true;
}

void retro_cheat_reset()
{
   v_gb[0]->clearCheats();
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
   std::string code_str(code);

   replace(code_str.begin(), code_str.end(), '+', ';');

   if (code_str.find("-") != std::string::npos) {
      v_gb[0]->setGameGenie(code_str);
   } else {
      v_gb[0]->setGameShark(code_str);
   }
}

enum gb_colorization_enable_type
{
   GB_COLORIZATION_DISABLED = 0,
   GB_COLORIZATION_AUTO     = 1,
   GB_COLORIZATION_CUSTOM   = 2,
   GB_COLORIZATION_INTERNAL = 3,
   GB_COLORIZATION_GBC      = 4,
   GB_COLORIZATION_SGB      = 5
};

static enum gb_colorization_enable_type gb_colorization_enable = GB_COLORIZATION_DISABLED;

static std::string rom_path;
static char internal_game_name[17];

static void load_custom_palette(void)
{
   const char *system_dir = NULL;
   const char *rom_file   = NULL;
   char *rom_name         = NULL;
   RFILE *palette_file    = NULL;
   unsigned line_index    = 0;
   bool path_valid        = false;
   unsigned rgb32         = 0;
   char palette_path[PATH_MAX_LENGTH];

   palette_path[0] = '\0';

   /* Get system directory */
   if (!environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) ||
       !system_dir)
   {
      gambatte_log(RETRO_LOG_WARN,
            "No system directory defined, unable to look for custom palettes.\n");
      return;
   }

   /* Look for palette named after ROM file */
   rom_file = path_basename(rom_path.c_str());
   if (!string_is_empty(rom_file))
   {
      size_t len = (strlen(rom_file) + 1) * sizeof(char);
      rom_name = (char*)malloc(len);
      strlcpy(rom_name, rom_file, len);
      path_remove_extension(rom_name);
      if (!string_is_empty(rom_name))
      {
         fill_pathname_join_special_ext(palette_path,
               system_dir, "palettes", rom_name, ".pal",
               sizeof(palette_path));
         path_valid = path_is_valid(palette_path);
      }
      free(rom_name);
      rom_name = NULL;
   }

   if (!path_valid)
   {
      /* Look for palette named after the internal game
       * name in the ROM header */
      fill_pathname_join_special_ext(palette_path,
            system_dir, "palettes", internal_game_name, ".pal",
            sizeof(palette_path));
      path_valid = path_is_valid(palette_path);
   }

   if (!path_valid)
   {
      /* Look for default custom palette file (default.pal) */
      fill_pathname_join_special_ext(palette_path,
            system_dir, "palettes", "default", ".pal",
            sizeof(palette_path));
      path_valid = path_is_valid(palette_path);
   }

   if (!path_valid)
      return; /* Unable to find any custom palette file */

   palette_file = filestream_open(palette_path,
         RETRO_VFS_FILE_ACCESS_READ,
         RETRO_VFS_FILE_ACCESS_HINT_NONE);

   if (!palette_file)
   {
      gambatte_log(RETRO_LOG_WARN,
            "Failed to open custom palette: %s\n", palette_path);
      return;
   }

   gambatte_log(RETRO_LOG_INFO, "Using custom palette: %s\n", palette_path);

   /* Iterate over palette file lines */
   while (!filestream_eof(palette_file))
   {
       char* line = filestream_getline(palette_file);
       const char* value_str = NULL;

       if (!line)
           break;

       /* Remove any leading/trailing whitespace
        * > Additionally handles 'leftovers' from
        *   CRLF line terminators if palette file
        *   happens to be in DOS format */
       string_trim_whitespace(line);

       if (string_is_empty(line) || /* Skip empty lines */
           (*line == '[') ||        /* Skip ini sections */
           (*line == ';'))          /* Skip ini comments */
           goto palette_line_end;

       /* Supposed to be a typo here... */
       if (string_starts_with(line, "slectedScheme="))
           goto palette_line_end;

       /* Get substring after first '=' character */
       value_str = strchr(line, '=');
       if (!value_str ||
           string_is_empty(++value_str))
       {
           gambatte_log(RETRO_LOG_WARN,
               "Error in %s, line %u (color left as default)\n",
               palette_path, line_index);
           goto palette_line_end;
       }

       /* Extract colour value */
       rgb32 = string_to_unsigned(value_str);
       if (rgb32 == 0)
       {
           /* string_to_unsigned() will return 0 if
            * string is invalid, so perform a manual
            * validity check... */
           for (; *value_str != '\0'; value_str++)
           {
               if (*value_str != '0')
               {
                   gambatte_log(RETRO_LOG_WARN,
                       "Unable to read palette color in %s, line %u (color left as default)\n",
                       palette_path, line_index);
                   goto palette_line_end;
               }
           }
       }

#ifdef VIDEO_RGB565
       rgb32 = (rgb32 & 0x0000F8) >> 3 | /* blue */
           (rgb32 & 0x00FC00) >> 5 | /* green */
           (rgb32 & 0xF80000) >> 8;  /* red */
#elif defined(VIDEO_ABGR1555)
       rgb32 = (rgb32 & 0x0000F8) << 7 | /* blue */
           (rgb32 & 0xF800) >> 6 | /* green */
           (rgb32 & 0xF80000) >> 19;  /* red */
#endif


       for (int i = 0; i < v_gb.size(); i++)
       {
           if (string_starts_with(line, "Background0="))
               v_gb[i]->setDmgPaletteColor(0, 0, rgb32);
           else if (string_starts_with(line, "Background1="))
               v_gb[i]->setDmgPaletteColor(0, 1, rgb32);
           else if (string_starts_with(line, "Background2="))
               v_gb[i]->setDmgPaletteColor(0, 2, rgb32);
           else if (string_starts_with(line, "Background3="))
               v_gb[i]->setDmgPaletteColor(0, 3, rgb32);
           else if (string_starts_with(line, "Sprite%2010="))
               v_gb[i]->setDmgPaletteColor(1, 0, rgb32);
           else if (string_starts_with(line, "Sprite%2011="))
               v_gb[i]->setDmgPaletteColor(1, 1, rgb32);
           else if (string_starts_with(line, "Sprite%2012="))
               v_gb[i]->setDmgPaletteColor(1, 2, rgb32);
           else if (string_starts_with(line, "Sprite%2013="))
               v_gb[i]->setDmgPaletteColor(1, 3, rgb32);
           else if (string_starts_with(line, "Sprite%2020="))
               v_gb[i]->setDmgPaletteColor(2, 0, rgb32);
           else if (string_starts_with(line, "Sprite%2021="))
               v_gb[i]->setDmgPaletteColor(2, 1, rgb32);
           else if (string_starts_with(line, "Sprite%2022="))
               v_gb[i]->setDmgPaletteColor(2, 2, rgb32);
           else if (string_starts_with(line, "Sprite%2023="))
               v_gb[i]->setDmgPaletteColor(2, 3, rgb32);
           else
               gambatte_log(RETRO_LOG_WARN,
                   "Error in %s, line %u (color left as default)\n",
                   palette_path, line_index);

        }

palette_line_end:
      line_index++;
      free(line);
      line = NULL;
   }

   filestream_close(palette_file);
}

static void find_internal_palette(const unsigned short **palette, bool *is_gbc)
{
   const char *palette_title = NULL;
   size_t index              = 0;
   struct retro_variable var = {0};

   // Read main internal palette setting
   var.key   = "gambatte_gb_internal_palette";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      // Handle TWB64 packs
      if (string_is_equal(var.value, "TWB64 - Pack 1"))
      {
         var.key   = "gambatte_gb_palette_twb64_1";
         var.value = NULL;

         if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
            palette_title = var.value;

         // Determine 'consolidated' palette index
         if (palette_title)
            index = RHMAP_GET_STR(palettes_twb64_1_index_map, palette_title);
         if (index > 0)
            index--;
         internal_palette_index = NUM_PALETTES_DEFAULT + index;
      }
      else if (string_is_equal(var.value, "TWB64 - Pack 2"))
      {
         var.key   = "gambatte_gb_palette_twb64_2";
         var.value = NULL;

         if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
            palette_title = var.value;

         // Determine 'consolidated' palette index
         if (palette_title)
            index = RHMAP_GET_STR(palettes_twb64_2_index_map, palette_title);
         if (index > 0)
            index--;
         internal_palette_index = NUM_PALETTES_DEFAULT +
               NUM_PALETTES_TWB64_1 + index;
      }
      // Handle PixelShift packs
      else if (string_is_equal(var.value, "PixelShift - Pack 1"))
      {
         var.key   = "gambatte_gb_palette_pixelshift_1";
         var.value = NULL;

         if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
            palette_title = var.value;

         // Determine 'consolidated' palette index
         if (palette_title)
            index = RHMAP_GET_STR(palettes_pixelshift_1_index_map, palette_title);
         if (index > 0)
            index--;
         internal_palette_index = NUM_PALETTES_DEFAULT +
               NUM_PALETTES_TWB64_1 + NUM_PALETTES_TWB64_2 + index;
      }
      else
      {
         palette_title = var.value;

         // Determine 'consolidated' palette index
         index = RHMAP_GET_STR(palettes_default_index_map, palette_title);
         if (index > 0)
            index--;
         internal_palette_index = index;
      }
   }

   // Ensure we have a valid palette title
   if (!palette_title)
   {
      palette_title          = "GBC - Grayscale";
      internal_palette_index = 8;
   }

   // Search for requested palette
   *palette = findGbcDirPal(palette_title);

   // If palette is not found (i.e. if a palette
   // is removed from the core, and a user loads
   // old core options settings), fall back to
   // black and white
   if (!(*palette))
   {
      palette_title          = "GBC - Grayscale";
      *palette               = findGbcDirPal(palette_title);
      internal_palette_index = 8;
      // No error check here - if this fails,
      // the core is entirely broken...
   }

   // Check whether this is a GBC palette
   if (!strncmp("GBC", palette_title, 3))
      *is_gbc = true;
   else
      *is_gbc = false;

   // Record that an internal palette is
   // currently in use
   internal_palette_active = true;
}

static void check_variables(bool startup)
{
   unsigned i, j;

   unsigned colorCorrection = 0;
   struct retro_variable var = {0};
   var.key = "gambatte_gbc_color_correction";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "GBC only"))
         colorCorrection = 1;
      else if (!strcmp(var.value, "always"))
         colorCorrection = 2;
   }
   
   unsigned colorCorrectionMode = 0;
   var.key   = "gambatte_gbc_color_correction_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value && !strcmp(var.value, "fast")) {
      colorCorrectionMode = 1;
   }

   for (int i = 0; i < v_gb.size(); i++)
       v_gb[i]->setColorCorrectionMode(colorCorrectionMode);
   
   
   
   float colorCorrectionBrightness = 0.5f; /* central */
   var.key   = "gambatte_gbc_frontlight_position";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "above screen"))
         colorCorrectionBrightness = 1.0f;
      else if (!strcmp(var.value, "below screen"))
         colorCorrectionBrightness = 0.0f;
   }

   for (int i = 0; i < v_gb.size(); i++)
    v_gb[i]->setColorCorrectionBrightness(colorCorrectionBrightness);
   
   unsigned darkFilterLevel = 0;
   var.key   = "gambatte_dark_filter_level";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      darkFilterLevel = static_cast<unsigned>(atoi(var.value));
   }

   for (int i = 0; i < v_gb.size(); i++)
    v_gb[i]->setDarkFilterLevel(darkFilterLevel);

   bool old_use_cc_resampler = use_cc_resampler;
   use_cc_resampler          = false;
   var.key                   = "gambatte_audio_resampler";
   var.value                 = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) &&
       var.value && !strcmp(var.value, "cc"))
      use_cc_resampler = true;

   if (!startup && (use_cc_resampler != old_use_cc_resampler))
   {
      struct retro_system_av_info av_info;
      audio_resampler_deinit();
      audio_resampler_init(false);
      retro_get_system_av_info(&av_info);
      environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
   }

   up_down_allowed = false;
   var.key         = "gambatte_up_down_allowed";
   var.value       = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         up_down_allowed = true;
      else
         up_down_allowed = false;
   }

   turbo_period      = TURBO_PERIOD_MIN;
   turbo_pulse_width = TURBO_PULSE_WIDTH_MIN;
   var.key           = "gambatte_turbo_period";
   var.value         = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      turbo_period = atoi(var.value);
      turbo_period = (turbo_period < TURBO_PERIOD_MIN) ?
            TURBO_PERIOD_MIN : turbo_period;
      turbo_period = (turbo_period > TURBO_PERIOD_MAX) ?
            TURBO_PERIOD_MAX : turbo_period;

      turbo_pulse_width = turbo_period >> 1;
      turbo_pulse_width = (turbo_pulse_width < TURBO_PULSE_WIDTH_MIN) ?
            TURBO_PULSE_WIDTH_MIN : turbo_pulse_width;
      turbo_pulse_width = (turbo_pulse_width > TURBO_PULSE_WIDTH_MAX) ?
            TURBO_PULSE_WIDTH_MAX : turbo_pulse_width;

      turbo_a_counter = 0;
      turbo_b_counter = 0;
   }

   rumble_level = 0;
   var.key      = "gambatte_rumble_level";
   var.value    = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      rumble_level = atoi(var.value);
      rumble_level = (rumble_level > 10) ? 10 : rumble_level;
      rumble_level = (rumble_level > 0)  ? ((0x1999 * rumble_level) + 0x5) : 0;
   }
   if (rumble_level == 0)
      deactivate_rumble();

   /* Interframe blending option has its own handler */
   check_frame_blend_variable();

#ifdef HAVE_NETWORK

   //gb_serialMode = SERIAL_NONE;
   gb_serialMode = SERIAL_LOCAL;
   var.key = "gambatte_gb_link_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      if (!strcmp(var.value, "Network Server")) {
         gb_serialMode = SERIAL_SERVER;
      } else if (!strcmp(var.value, "Network Client")) {
         gb_serialMode = SERIAL_CLIENT;
      }
   }

   var.key = "gambatte_gb_link_network_port";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value) {
      gb_NetworkPort=atoi(var.value);
   }

   unsigned ip_index = 1;
   gb_NetworkClientAddr = "";

   for (i = 0; i < 4; i++)
   {
      std::string octet = "0";
      char tmp[8] = {0};

      for (j = 0; j < 3; j++)
      {
         char key[64] = {0};

         /* Should be using std::to_string() here, but some
          * compilers don't support it... */
         sprintf(key, "%s%u",
              "gambatte_gb_link_network_server_ip_", ip_index);

         var.key = key;
         var.value = NULL;

         if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
            octet += std::string(var.value);

         ip_index++;
      }

      /* Remove leading zeros
       * Should be using std::stoul() here, but some compilers
       * don't support it... */
      sprintf(tmp, "%u", atoi(octet.c_str()));
      octet = std::string(tmp);

      if (i < 3)
         octet += ".";

      gb_NetworkClientAddr += octet;
   }

   switch(gb_serialMode)
   {
      case SERIAL_SERVER:
         gb_net_serial.start(true, gb_NetworkPort, gb_NetworkClientAddr);
         v_gb[0]->setSerialIO(&gb_net_serial);
         break;
      case SERIAL_CLIENT:
         gb_net_serial.start(false, gb_NetworkPort, gb_NetworkClientAddr);
         v_gb[0]->setSerialIO(&gb_net_serial);
         break;
      case SERIAL_LOCAL:
      {
          LocalSerial* sio_a = new LocalSerial();
          LocalSerial* sio_b = new LocalSerial();

          sio_a->setConnectedSerialIO(sio_b);
          sio_b->setConnectedSerialIO(sio_a);

          sio_a->setLinkTarget(v_gb[0]);
          sio_b->setLinkTarget(v_gb[1]);

          v_gb[0]->setSerialIO(sio_a);
          v_gb[1]->setSerialIO(sio_b);
          break;
      }
      default:
         gb_net_serial.stop();
         v_gb[0]->setSerialIO(NULL);
         break;
   }

   /* Show/hide core options */
   update_option_visibility();

#endif

   internal_palette_active = false;
   var.key = "gambatte_gb_colorization";

   if (!environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) {
      // Should really wait until the end to call setColorCorrection(),
      // but don't want to have to change the indentation of all the
      // following code... (makes it too difficult to see the changes in
      // a git diff...)
       for (int i = 0; i < NUM_GAMEBOYS; i++)
           v_gb[i]->setColorCorrection(v_gb[i]->isCgb() && (colorCorrection != 0));
    
      return;
   }
   
   if (v_gb[0]->isCgb()) {

      for (int i = 0; i < NUM_GAMEBOYS; i++)
        v_gb[i]->setColorCorrection(colorCorrection != 0);

      return;
   }

   // else it is a GB-mono game -> set a color palette

   if (strcmp(var.value, "disabled") == 0)
      gb_colorization_enable = GB_COLORIZATION_DISABLED;
   else if (strcmp(var.value, "auto") == 0)
      gb_colorization_enable = GB_COLORIZATION_AUTO;
   else if (strcmp(var.value, "custom") == 0)
      gb_colorization_enable = GB_COLORIZATION_CUSTOM;
   else if (strcmp(var.value, "internal") == 0)
      gb_colorization_enable = GB_COLORIZATION_INTERNAL;
   else if (strcmp(var.value, "GBC") == 0)
      gb_colorization_enable = GB_COLORIZATION_GBC;
   else if (strcmp(var.value, "SGB") == 0)
      gb_colorization_enable = GB_COLORIZATION_SGB;

   // Containers for GBC/SGB BIOS built-in palettes
   const unsigned short *gbc_bios_palette = NULL;
   const unsigned short *sgb_bios_palette = NULL;
   bool isGbcPalette = false;

   switch (gb_colorization_enable)
   {
      case GB_COLORIZATION_AUTO:
         // Automatic colourisation
         // Order of preference:
         // 1 - SGB, if more colourful than GBC
         // 2 - GBC, if more colourful than SGB
         // 3 - SGB, if no GBC palette defined
         // 4 - User-defined internal palette, if neither GBC nor SGB palettes defined
         //
         // Load GBC BIOS built-in palette
         gbc_bios_palette = findGbcTitlePal(internal_game_name);
         // Load SGB BIOS built-in palette
         sgb_bios_palette = findSgbTitlePal(internal_game_name);
         // If both GBC and SGB palettes are defined,
         // use whichever is more colourful
         if (gbc_bios_palette)
         {
            isGbcPalette = true;
            if (sgb_bios_palette)
            {
               if (gbc_bios_palette != p005 &&
                   gbc_bios_palette != p006 &&
                   gbc_bios_palette != p007 &&
                   gbc_bios_palette != p008 &&
                   gbc_bios_palette != p012 &&
                   gbc_bios_palette != p013 &&
                   gbc_bios_palette != p016 &&
                   gbc_bios_palette != p017 &&
                   gbc_bios_palette != p01B)
               {
                  // Limited color GBC palette -> use SGB equivalent
                  gbc_bios_palette = sgb_bios_palette;
                  isGbcPalette = false;
               }
            }
         }
         // If no GBC palette is defined, use SGB palette
         if (!gbc_bios_palette)
         {
            gbc_bios_palette = sgb_bios_palette;
         }
         // If neither GBC nor SGB palettes are defined, set
         // user-defined internal palette
         if (!gbc_bios_palette)
         {
            find_internal_palette(&gbc_bios_palette, &isGbcPalette);
         }
         break;
      case GB_COLORIZATION_CUSTOM:
         load_custom_palette();
         break;
      case GB_COLORIZATION_INTERNAL:
         find_internal_palette(&gbc_bios_palette, &isGbcPalette);
         break;
      case GB_COLORIZATION_GBC:
         // Force GBC colourisation
         gbc_bios_palette = findGbcTitlePal(internal_game_name);
         if (!gbc_bios_palette)
         {
            gbc_bios_palette = findGbcDirPal("GBC - Dark Green"); // GBC Default
         }
         isGbcPalette = true;
         break;
      case GB_COLORIZATION_SGB:
         // Force SGB colourisation
         gbc_bios_palette = findSgbTitlePal(internal_game_name);
         if (!gbc_bios_palette)
         {
            gbc_bios_palette = findGbcDirPal("SGB - 1A"); // SGB Default
         }
         break;
      default: // GB_COLORIZATION_DISABLED
         gbc_bios_palette = findGbcDirPal("GBC - Grayscale");
         isGbcPalette = true;
         break;
   }
   
   // Enable colour correction, if required
   for (int i = 0; i < NUM_GAMEBOYS; i++)
    v_gb[i]->setColorCorrection((colorCorrection == 2) || ((colorCorrection == 1) && isGbcPalette));
   
   // If gambatte is using custom colourisation
   // then we have already loaded the palette.
   // In this case we can therefore skip this loop.
   if (gb_colorization_enable != GB_COLORIZATION_CUSTOM)
   {
      unsigned rgb32 = 0;
      for (unsigned palnum = 0; palnum < 3; ++palnum)
      {
         for (unsigned colornum = 0; colornum < 4; ++colornum)
         {
             for (int i = 0; i < NUM_GAMEBOYS; i++) {
                 rgb32 = v_gb[i]->gbcToRgb32(gbc_bios_palette[palnum * 4 + colornum]);
                 v_gb[i]->setDmgPaletteColor(palnum, colornum, rgb32);
             }
            
         }
      }
   }
}

static unsigned pow2ceil(unsigned n) {
   --n;
   n |= n >> 1;
   n |= n >> 2;
   n |= n >> 4;
   n |= n >> 8;
   ++n;

   return n;
}


bool retro_load_game(const struct retro_game_info *info)
{
   bool can_dupe = false;
   environ_cb(RETRO_ENVIRONMENT_GET_CAN_DUPE, &can_dupe);
   if (!can_dupe)
   {
      gambatte_log(RETRO_LOG_ERROR, "Cannot dupe frames!\n");
      return false;
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble))
      gambatte_log(RETRO_LOG_INFO, "Rumble environment supported.\n");
   else
      gambatte_log(RETRO_LOG_INFO, "Rumble environment not supported.\n");

   struct retro_input_descriptor desc[] = {
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },


      { 0 },
   };

   struct retro_input_descriptor desc_ff[] = { /* ff: fast forward */
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },


      { 0 },
   };

   struct retro_input_descriptor desc_ps[] = { /* ps: palette switching */
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },


      { 0 },
   };

   struct retro_input_descriptor desc_ff_ps[] = { /* ff: fast forward, ps: palette switching */
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },
      { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },

      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },
      { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },

      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },
      { 2, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },

      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "D-Pad Left" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "D-Pad Up" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "D-Pad Down" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "D-Pad Right" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Turbo B" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "Turbo A" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "Prev. Internal Palette" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "Next Internal Palette" },
      { 3, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Fast Forward" },

      { 0 },
   };

   if (libretro_supports_ff_override)
   {
      if (libretro_supports_set_variable)
         environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc_ff_ps);
      else
         environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc_ff);
   }
   else
   {
      if (libretro_supports_set_variable)
         environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc_ps);
      else
         environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);
   }

#if defined(VIDEO_RGB565) || defined(VIDEO_ABGR1555)
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      gambatte_log(RETRO_LOG_ERROR, "RGB565 is not supported.\n");
      return false;
   }
#else
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      gambatte_log(RETRO_LOG_ERROR, "XRGB8888 is not supported.\n");
      return false;
   }
#endif
   
   bool has_gbc_bootloader = file_present_in_system("gbc_bios.bin");

   unsigned flags = 0;
   struct retro_variable var = {0};
   var.key = "gambatte_gb_hwmode";
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "GB"))
      {
          flags |= gambatte::GB::FORCE_DMG;
      }
      
      if (!strcmp(var.value, "GBC"))
      {
         if (has_gbc_bootloader && use_official_bootloader)
            flags |= gambatte::GB::FORCE_CGB;
      }

      if (!strcmp(var.value, "GBA"))
      {
         flags |= gambatte::GB::GBA_CGB;
         if (has_gbc_bootloader && use_official_bootloader)
            flags |= gambatte::GB::FORCE_CGB;
      }
   }

   for (int i = 0; i < NUM_GAMEBOYS; i++)
   {
       if (v_gb[i]->load(info->data, info->size, flags) != 0)
           return false;
   }
   

   rom_path = info->path ? info->path : "";
   strncpy(internal_game_name, (const char*)info->data + 0x134, sizeof(internal_game_name) - 1);
   internal_game_name[sizeof(internal_game_name)-1]='\0';

   gambatte_log(RETRO_LOG_INFO, "Got internal game name: %s.\n", internal_game_name);

   check_variables(true);
   audio_resampler_init(true);

   unsigned sramlen       = v_gb[0]->savedata_size();
   const uint64_t rom     = RETRO_MEMDESC_CONST;
   const uint64_t mainram = RETRO_MEMDESC_SYSTEM_RAM;
   struct retro_memory_map mmaps;

   struct retro_memory_descriptor descs[10] =
   {
      { mainram, v_gb[0]->rambank0_ptr(),     0, 0xC000,          0, 0, 0x1000, NULL },
      { mainram, v_gb[0]->rambank1_ptr(),     0, 0xD000,          0, 0, 0x1000, NULL },
      { mainram, v_gb[0]->zeropage_ptr(),     0, 0xFF80,          0, 0, 0x0080, NULL },
      {       0, v_gb[0]->vram_ptr(),         0, 0x8000,          0, 0, 0x2000, NULL },
      {       0, v_gb[0]->oamram_ptr(),       0, 0xFE00, 0xFFFFFFE0, 0, 0x00A0, NULL },
      {     rom, v_gb[0]->rombank0_ptr(),     0, 0x0000,          0, 0, 0x4000, NULL },
      {     rom, v_gb[0]->rombank1_ptr(),     0, 0x4000,          0, 0, 0x4000, NULL },
      {       0, v_gb[0]->oamram_ptr(),   0x100, 0xFF00,          0, 0, 0x0080, NULL },
      {       0, 0,                     0,      0,          0, 0,      0,    0 },
      {       0, 0,                     0,      0,          0, 0,      0,    0 }
   };

   unsigned i = 8;
   if (sramlen)
   {
      descs[i].ptr    = v_gb[0]->savedata_ptr();
      descs[i].start  = 0xA000;
      descs[i].select = (size_t)~0x1FFF;
      descs[i].len    = sramlen;
      i++;
   }

   if (v_gb[0]->isCgb())
   {
      descs[i].flags  = mainram;
      descs[i].ptr    = v_gb[0]->rambank2_ptr();
      descs[i].start  = 0x10000;
      descs[i].select = 0xFFFFA000;
      descs[i].len    = 0x6000;
      i++;
   }

   mmaps.descriptors     = descs;
   mmaps.num_descriptors = i;   
   environ_cb(RETRO_ENVIRONMENT_SET_MEMORY_MAPS, &mmaps);
   
   bool yes = true;
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &yes);

   rom_loaded = true;
   return true;
}


bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }

void retro_unload_game()
{
   rom_loaded = false;
}

unsigned retro_get_region() { return RETRO_REGION_NTSC; }

void *retro_get_memory_data(unsigned id)
{
   if (rom_loaded) switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:
         return v_gb[0]->savedata_ptr();
      case RETRO_MEMORY_RTC:
         return v_gb[0]->rtcdata_ptr();
      case RETRO_MEMORY_SYSTEM_RAM:
         /* Really ugly hack here, relies upon 
          * libgambatte/src/memory/memptrs.cpp MemPtrs::reset not
          * realizing that that memchunk hack is ugly, or 
          * otherwise getting rearranged. */
         return v_gb[0]->rambank0_ptr();
   }

   return 0;
}

size_t retro_get_memory_size(unsigned id)
{
   if (rom_loaded) switch (id)
   {
      case RETRO_MEMORY_SAVE_RAM:
         return v_gb[0]->savedata_size();
      case RETRO_MEMORY_RTC:
         return v_gb[0]->rtcdata_size();
      case RETRO_MEMORY_SYSTEM_RAM:
         /* This is rather hacky too... it relies upon 
          * libgambatte/src/memory/cartridge.cpp not changing
          * the call to memptrs.reset, but this is 
          * probably mostly safe.
          *
          * GBC will probably not get a
          * hardware upgrade anytime soon. */
         return (v_gb[0]->isCgb() ? 8 : 2) * 0x1000ul;
   }

   return 0;
}


#include "inline/helper_inline.h"

void retro_run()
{
    static uint64_t samples_count = 0;
    static uint64_t frames_count = 0;

    input_poll_cb();
    update_input_state();

    /*
    uint64_t expected_frames = samples_count / SOUND_SAMPLES_PER_FRAME;
    if (frames_count < expected_frames) // Detect frame dupes.
    {
       video_cb(NULL, VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_PITCH * sizeof(gambatte::video_pixel_t));
       frames_count++;
       return;
    }
    */

    union
    {
        gambatte::uint_least32_t u32[SOUND_BUFF_SIZE];
        int16_t i16[2 * SOUND_BUFF_SIZE];
    } static sound_buf[NUM_GAMEBOYS];
    unsigned samples = SOUND_SAMPLES_PER_RUN;


    while (v_gb[0]->runFor(video_buf, VIDEO_PITCH, sound_buf[0].u32, SOUND_BUFF_SIZE, samples) == -1)
    {
        for (int i = 1; i < NUM_GAMEBOYS; i++)
        {
            v_gb[i]->runFor(video_buf + (GB_SCREEN_WIDTH * i), VIDEO_PITCH, sound_buf[i].u32, SOUND_BUFF_SIZE, samples);
        }

        if (use_cc_resampler)
            CC_renderaudio((audio_frame_t*)sound_buf[0].u32, samples);
        else
        {
            blipper_renderaudio(sound_buf[0].i16, samples);

            unsigned read_avail = blipper_read_avail(resampler_l);
            if (read_avail >= (BLIP_BUFFER_SIZE >> 1))
                audio_out_buffer_read_blipper(read_avail);
        }

        samples_count += samples;
        samples = SOUND_SAMPLES_PER_RUN;

 
    }

    for (int i = 1; i < NUM_GAMEBOYS; i++)
    {
        while (v_gb[i]->runFor(video_buf + (GB_SCREEN_WIDTH * i), VIDEO_PITCH, sound_buf[i].u32, SOUND_BUFF_SIZE, samples) == -1) {}
    }



    /* Perform interframe blending, if required */
    if (blend_frames)
        blend_frames();

    video_cb(video_buf, VIDEO_WIDTH, VIDEO_HEIGHT, VIDEO_PITCH * sizeof(gambatte::video_pixel_t));

    if (use_cc_resampler)
        CC_renderaudio((audio_frame_t*)sound_buf[0].u32, samples);
    else
    {
        blipper_renderaudio(sound_buf[0].i16, samples);

        unsigned read_avail = blipper_read_avail(resampler_l);
        audio_out_buffer_read_blipper(read_avail);
    }
    samples_count += samples;
    audio_upload_samples();

    /* Apply any 'pending' rumble effects */
    if (rumble_active)
        apply_rumble();

    frames_count++;

    bool updated = false;
    if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
        check_variables(false);


    /* GBRemix testing*/
    unsigned char* ram = (unsigned char*)retro_get_memory_data(2);

    char ruppees = ram[0x1B5E];
    if (ruppees == 1)
    {
   
        /*  
        *Testing trying to load savestate
        * 
        */

        const char* savestate_filename = "C:\\Users\\TimZen\\Downloads\\RetroArch\\RetroArch-Win64\\states\\Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2) (SGB Enhanced) (GB Compatible).state2";
        size_t state_size;
        
        // Read the savestate into a buffer
        char* state_buffer = read_savestate(savestate_filename, &state_size);
        if (state_buffer == NULL) {
            printf("Failed to read the savestate\n");
            return;
        }

        // Unserialize the savestate using Libretro API
        if (!retro_unserialize(state_buffer, state_size)) {
            printf("Failed to unserialize the savestate\n");
            free(state_buffer);
            return;
        }

        // Successfully unserialized the savestate, now you can proceed with emulation

        // Free the allocated buffer
        free(state_buffer);

    }

}



unsigned retro_api_version() { return RETRO_API_VERSION; }

