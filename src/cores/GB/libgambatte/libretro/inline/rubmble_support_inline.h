
/************************/
/* Rumble support START */
/************************/

static struct retro_rumble_interface rumble = { 0 };
static uint16_t rumble_strength_last = 0;
static uint16_t rumble_strength_up = 0;
static uint16_t rumble_strength_down = 0;
static uint16_t rumble_level = 0;
static bool rumble_active = false;

void cartridge_set_rumble(unsigned active)
{
    if (!rumble.set_rumble_state ||
        !rumble_level)
        return;

    if (active)
        rumble_strength_up++;
    else
        rumble_strength_down++;

    rumble_active = true;
}

static void apply_rumble(void)
{
    uint16_t strength;

    if (!rumble.set_rumble_state ||
        !rumble_level)
        return;

    strength = (rumble_strength_up > 0) ?
        (rumble_strength_up * rumble_level) /
        (rumble_strength_up + rumble_strength_down) : 0;

    rumble_strength_up = 0;
    rumble_strength_down = 0;

    if (strength == rumble_strength_last)
        return;

    rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, strength);
    rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, strength);

    rumble_strength_last = strength;
}

static void deactivate_rumble(void)
{
    rumble_strength_up = 0;
    rumble_strength_down = 0;
    rumble_active = false;

    if (!rumble.set_rumble_state ||
        (rumble_strength_last == 0))
        return;

    rumble.set_rumble_state(0, RETRO_RUMBLE_WEAK, 0);
    rumble.set_rumble_state(0, RETRO_RUMBLE_STRONG, 0);

    rumble_strength_last = 0;
}

/**********************/
/* Rumble support END */
/**********************/