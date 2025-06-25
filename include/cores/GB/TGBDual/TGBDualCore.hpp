#include <DoubleCherryEngine/DoubleCherryEngine.hpp>
#include <vector>
#include "gb.h"
#include "TGBDualRenderer.hpp"
#include <memory>


class TGBDualCore : public IMultiCore, public IEventListener {

public:	

	TGBDualCore() = default;
	~TGBDualCore() = default;  
    
    std::vector<std::unique_ptr<gb>> gameboyInstances;
    std::vector<std::unique_ptr<TGBDualRenderer>> gameboyRenderers;

    // Get the number of emulated systems
   int getActiveSystemsCount() override {
        return gameboyInstances.size();
    };
   
    // Get the number of max emulated systems
   int getMaxSystemsCount() override {
       return kmaxGameboyInstancesCount_;
   };

   ScreenSize getSystemScreenSize()  {
       return screenSize_;
   };

    // Initialize the core
    virtual void init() override;

    // Deinitialize and clean up the core
    virtual void deinit() override;

    // Load a standard game
    virtual bool loadGame(const struct retro_game_info* info) override;

    // Load a game with special parameters or behavior
    bool loadGameSpecial() override;

    // Unload the currently loaded game
    void unloadGame() override;

    // Reset the core state (e.g., like a soft reset)
    void reset() override;

    // Run the core's main loop (e.g., for one frame)
    void run() override;

private:
    const int kmaxGameboyInstancesCount_ = 16; // Maximum number of GameBoys supported by this core
	ScreenSize screenSize_ = ScreenSize::GB; // Default screen size


}