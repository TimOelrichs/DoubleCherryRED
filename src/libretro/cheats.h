
void retro_cheat_reset(void)
{
    for (int i = 0; i < emulated_gbs; ++i)
    {
        if (v_gb[i])
            v_gb[i]->get_cheat()->clear();
    }

}

void retro_cheat_set(unsigned index, bool enabled, const char* code)
{
#if 1 == 0
    if (log_cb)
        log_cb(RETRO_LOG_INFO, "CHEAT:  id=%d, enabled=%d, code='%s'\n", index, enabled, code);
    // FIXME: work in progress.
    // As it stands, it seems TGB Dual only has support for Gameshark codes.
    // Unfortunately, the cheat.xml that ships with bsnes seems to only have
    // Game Genie codes, which are ROM patches rather than RAM.
    // http://nocash.emubase.de/pandocs.htm#gamegeniesharkcheats
    if (false && g_gb[0])
    {
        cheat_dat cdat;
        cheat_dat* tmp = &cdat;

        strncpy(cdat.name, code, sizeof(cdat.name));

        tmp->enable = true;
        tmp->next = NULL;

        while (false)
        { // basically, iterate over code.split('+')
            // TODO: remove all non-alnum chars here
            if (false)
            { // if strlen is 9, game genie
                // game genie format: for "ABCDEFGHI",
                // AB   = New data
                // FCDE = Memory address, XORed by 0F000h
                // GIH  = Check data (can be ignored for our purposes)
                word scramble;
                sscanf(code, "%2hhx%4hx", &tmp->dat, &scramble);
                tmp->code = 1; // TODO: test if this is correct for ROM patching
                tmp->adr = (((scramble & 0xF) << 12) ^ 0xF000) | (scramble >> 4);
            }
            else if (false)
            { // if strlen is 8, gameshark
                // gameshark format for "ABCDEFGH",
                // AB    External RAM bank number
                // CD    New Data
                // GHEF  Memory Address (internal or external RAM, A000-DFFF)
                byte adrlo, adrhi;
                sscanf(code, "%2hhx%2hhx%2hhx%2hhx", &tmp->code, &tmp->dat, &adrlo, &adrhi);
                tmp->adr = (adrhi << 8) | adrlo;
            }
            if (false)
            { // if there are more cheats left in the string
                tmp->next = new cheat_dat;
                tmp = tmp->next;
            }
        }
    }
    g_gb[0].get_cheat().add_cheat(&cdat);
#endif

    std::string code_str(code);

    // replace(code_str.begin(), code_str.end(), '+', ';');

     //if (code_str.find("-") != std::string::npos) 
    {
        v_gb[0]->set_Game_Genie(enabled, code_str);
    }

}