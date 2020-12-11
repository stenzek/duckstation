import sys
import configparser

def parse_ini(path):
    config = configparser.ConfigParser()
    config.read(path)

    entries = {}
    int_keys = {
        "DisplayActiveStartOffset": "display_active_start_offset",
        "DisplayActiveEndOffset": "display_active_end_offset",
        "DMAMaxSliceTicks": "dma_max_slice_ticks",
        "DMAHaltTicks": "dma_halt_ticks",
        "GPUFIFOSize" : "gpu_fifo_size",
        "GPUMaxRunAhead" : "gpu_max_run_ahead"
    }
    float_keys = {
        "GPUPGXPTolerance" : "gpu_pgxp_tolerance"
    }
    traits = [
        "ForceInterpreter",
        "ForceSoftwareRenderer",
        "ForceInterlacing",
        "DisableTrueColor",
        "DisableUpscaling",
        "DisableScaledDithering",
        "DisableForceNTSCTimings",
        "DisableWidescreen",
        "DisablePGXP",
        "DisablePGXPCulling",
        "DisablePGXPTextureCorrection",
        "ForcePGXPVertexCache",
        "ForcePGXPCPUMode",
        "DisableAnalogModeForcing",
        "ForceRecompilerMemoryExceptions",
        "ForceRecompilerICache"
    ]

    for gameid in config.sections():
        entry = {}
        for ini_key, cpp_key in int_keys.items():
            try:
                value = config.get(gameid, ini_key)
                if value is not None:
                    entry[cpp_key] = str(value)
            except configparser.NoOptionError:
                pass

        for ini_key, cpp_key in float_keys.items():
            try:
                value = config.getfloat(gameid, ini_key, fallback=None)
                if value is not None:
                    entry[cpp_key] = str(value)
            except configparser.NoOptionError:
                pass
        
        for trait in traits:
            try:
                value = config.getboolean(gameid, trait, fallback=None)
                if value == True:
                    if "traits" not in entry:
                        entry["traits"] = []
                    entry["traits"].append(trait)
            except configparser.NoOptionError:
                pass

        if len(entry) > 0:
            entries[gameid] = entry

    return entries


def write_cpp(entries, path):
    print("Writing %u entries to '%s'" % (len(entries), path))
    with open(path, "w") as f:
        f.write('#include "libretro_game_settings.h"\n')
        f.write('\n')
        f.write('std::unique_ptr<GameSettings::Entry> GetSettingsForGame(const std::string& game_code)\n')
        f.write('{\n')
        f.write('  std::unique_ptr<GameSettings::Entry> gs = std::make_unique<GameSettings::Entry>();\n')
        f.write('\n')

        for gameid, entry in entries.items():
            f.write('  if (game_code == "%s")\n' % gameid)
            f.write('  {\n')
            for key, value in entry.items():
                if key == "traits":
                    for trait in value:
                        f.write('    gs->AddTrait(GameSettings::Trait::%s);\n' % trait)
                else:
                    f.write('    gs->%s = %s;\n' % (key, value))
            f.write('    return gs;\n')
            f.write('  }\n')
            f.write('\n')

        f.write('  return {};\n')
        f.write('}\n')
    

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("usage: %s <path to gamesettings.ini> <output cpp file>" % sys.argv[0])
        sys.exit(1)

    entries = parse_ini(sys.argv[1])
    write_cpp(entries, sys.argv[2])
