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

#include <DoubleCherryEngine/Renderer/VideoRenderer/VideoRenderer.hpp>
#include <DoubleCherryEngine/ColorCorrection/ColorCorrectionManager.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <array>
#include "renderer.h"

#define clampf(x, min, max) ((x) < (min) ? (min) : ((x) > (max) ? (max) : (x)))

constexpr std::array<uint32_t, 4> DMG_PALETTE = {
0xFFFFFFFF, // Weiß
0xAAAAAAFF, // Hellgrau
0x555555FF, // Dunkelgrau
0x000000FF  // Schwarz
};

constexpr int GRADIENT_STEPS = 64;
extern std::array<uint16_t, GRADIENT_STEPS> blended_palette;




class TGBDualRenderer : public renderer
{
public:
	TGBDualRenderer(int id) { id_ = id; };
	virtual ~TGBDualRenderer() {};

	 void reset() {}
	 word get_sensor(bool x_y) { return 0; }
	 void set_bibrate(bool bibrate) {}

	 void render_screen(byte* buf, int width, int height, int depth) override { video_renderer.addFrame(id_, buf); };
	 word map_color(word gbColor) override {
		 return gbColor;
	 }; //TODO: colorCorrectionManager.applyCorrection(gbColor); };
	 word unmap_color(word gbColor) { return gbColor; }; //TODO
	 int check_pad() ;
	 void refresh();
	 byte get_time(int type);
	 void set_time(int type, byte dat);


	
	dword fixed_time;
private:
	VideoRenderer& video_renderer = VideoRenderer::getInstance();
	//ColorCorrectionManager& colorCorrectionManager = ColorCorrectionManager::getInstance();
	int id_;
	int cur_time;
	int which_gb;
	bool rgb565;



};
