/*--------------------------------------------------
   TGB Dual - Gameboy Emulator -
   Copyright (C) 2001  Hii

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; either version 2
   of the License, or (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
*/

// libretro implementation of the renderer, should probably be renamed from dmy.

#include <string.h>
//#include <math.h>
#include <cmath>
#include <time.h>
#include <vector>

#include <cores/GB/TGBDual/TGBDualRenderer.hpp>



void TGBDualRenderer::refresh() {
    static int16_t stream[SAMPLES_PER_FRAME * 2];
    // static int16_t stream[SAMPLES_PER_FRAME];


    //if (v_gb[1] && gblink_enable)
    //if (emulated_gbs > 1)
    if (true)
    {
        /*
      // if dual gb mode
      if (audio_2p_mode == 2)
      {
         // mix down to one per channel (dual mono)
         int16_t tmp_stream[SAMPLES_PER_FRAME*2];
         this->snd_render->render(tmp_stream, SAMPLES_PER_FRAME);
         for(int i = 0; i < SAMPLES_PER_FRAME; ++i)
         {
            int l = tmp_stream[(i*2)+0], r = tmp_stream[(i*2)+1];
            stream[(i*2)+which_gb] = int16_t( (l+r) / 2 );
         }
      }
      else if (audio_2p_mode == which_gb)
      */

        if (audio_2p_mode == which_gb)
        {
            // only play gb 0 or 1

            this->snd_render->render(stream, SAMPLES_PER_FRAME);
            audio_batch_cb(stream, SAMPLES_PER_FRAME);

            memset(stream, 0, sizeof(stream));

        }
        if (which_gb >= (emulated_gbs - 1))
        {
            // only do audio callback after both gb's are rendered.
            //audio_batch_cb(stream, SAMPLES_PER_FRAME);

            //audio_2p_mode &= 3;
            //memset(stream, 0, sizeof(stream));
        }
    }
    else
    {
        this->snd_render->render(stream, SAMPLES_PER_FRAME);
        audio_batch_cb(stream, SAMPLES_PER_FRAME);
    }
    fixed_time = time(NULL);


}

int TGBDualRenderer::check_pad()
{
    int16_t joypad_bits;
    if (libretro_supports_bitmasks)
        joypad_bits = input_state_cb(which_gb, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
    else
    {
        unsigned i;
        joypad_bits = 0;
        for (i = 0; i < (RETRO_DEVICE_ID_JOYPAD_R3 + 1); i++)
            joypad_bits |= input_state_cb(which_gb, RETRO_DEVICE_JOYPAD, 0, i) ? (1 << i) : 0;
    }

    // update pad state: a,b,select,start,down,up,left,right
    return
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_A)) ? 1 : 0) << 0 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_B)) ? 1 : 0) << 1 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_X)) ? 1 : 0) << 1 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_SELECT)) ? 1 : 0) << 2 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_START)) ? 1 : 0) << 3 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_DOWN)) ? 1 : 0) << 4 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_UP)) ? 1 : 0) << 5 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_LEFT)) ? 1 : 0) << 6 |
        ((joypad_bits & (1 << RETRO_DEVICE_ID_JOYPAD_RIGHT)) ? 1 : 0) << 7;
}



byte TGBDualRenderer::get_time(int type)
{
    dword now = fixed_time - cur_time;

    switch (type)
    {
    case 8: // second
        return (byte)(now % 60);
    case 9: // minute
        return (byte)((now / 60) % 60);
    case 10: // hour
        return (byte)((now / (60 * 60)) % 24);
    case 11: // day (L)
        return (byte)((now / (24 * 60 * 60)) & 0xff);
    case 12: // day (H)
        return (byte)((now / (256 * 24 * 60 * 60)) & 1);
    }
    return 0;
}

void TGBDualRenderer::set_time(int type, byte dat)
{
    dword now = fixed_time;
    dword adj = now - cur_time;

    switch (type)
    {
    case 8: // second
        adj = (adj / 60) * 60 + (dat % 60);
        break;
    case 9: // minute
        adj = (adj / (60 * 60)) * 60 * 60 + (dat % 60) * 60 + (adj % 60);
        break;
    case 10: // hour
        adj = (adj / (24 * 60 * 60)) * 24 * 60 * 60 + (dat % 24) * 60 * 60 + (adj % (60 * 60));
        break;
    case 11: // day (L)
        adj = (adj / (256 * 24 * 60 * 60)) * 256 * 24 * 60 * 60 + (dat * 24 * 60 * 60) + (adj % (24 * 60 * 60));
        break;
    case 12: // day (H)
        adj = (dat & 1) * 256 * 24 * 60 * 60 + (adj % (256 * 24 * 60 * 60));
        break;
    }
    cur_time = now - adj;
}

