#include <cores/GB/TGBDual/TGBDualCore.hpp>

void TGBDualCore::init() {
	
    //create gameboy instances
    for (byte i = 0; i < max_gbs; i++)
    {

        render.push_back(new dmy_renderer(i));
        v_gb.push_back(new gb(render[i], true, true));
        _serialize_size[i] = 0;
    }
}

// Deinitialize and clean up the core
void TGBDualCore::deinit() {};

// Load a standard game
bool TGBDualCore::loadGame(const struct retro_game_info* info)
{
    //load roms
    for (auto& gb : gameboyInstances) {
        if (!gb->load_rom(rom_data, rom_size, NULL, 0, libretro_supports_persistent_buffer))
            return false;
    }

};

// Load a game with special parameters or behavior
bool TGBDualCore::loadGameSpecial() {};

// Unload the currently loaded game
void TGBDualCore::unloadGame() {

    gameboyInstances.clear();
    gameboyRenderers.clear();

};

// Reset the core state (e.g., like a soft reset)
void TGBDualCore::reset() {

    for (auto& gb : gameboyInstances) {
        if (gb) gb->reset();
    }
    /*
    if (master_link)
        master_link->reset();
        */

    for (int i = 0; i < v_serializable_devices.size(); i++)
    {
        v_serializable_devices[i]->reset();
    };
};



void TGBDualCore::run() override {

    for (int line = 0; line < 154; line++)
    {
        //if (extra_inputpolling_enabled) performExtraInputPoll();

        for (auto& gb : gameboyInstances) {
            if (gb) gb->run();
        }
        if (master_link)
            master_link->process();
    }
};
