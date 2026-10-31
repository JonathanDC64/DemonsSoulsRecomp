// prework_check: des_prework.cpp's shaders pass as setup runs it (`des_host --shaders <game folder> <EBOOT.elf>`),
// in a program of its own that runs no game, so the pass's targets and their stamps can be tried without building
// or starting the game. Built with a runtime that has the portable renderer (PS3RECOMP_RSX_HAL), where the pass runs
// once per target.
//   prework_check <game folder> [EBOOT.elf]
// PS3_SHADER_CACHE names the cache and DES_PSO_INDEX the pipeline index, as for the game. It prints what
// des_prework_shaders prints and returns its exit status. What it reads and writes comes from a game: keep it out of
// the repository.
#include <cstdint>
#include <cstdio>

extern "C" {

// What the runtime's modules take from a game's loader (host_stubs.c has the rest): nothing runs here.
uint8_t* vm_base = nullptr;
extern const uint64_t function_table[2];  // (extern first: a const's linkage is the file's own otherwise)
extern const uint64_t function_table_count;
const uint64_t function_table[2] = {0, 0};
const uint64_t function_table_count = 0;

// The UI's (des_ui.cpp): a start's pass waits on the privacy note's question first. Setup's pass never asks.
void des_ui_privacy_wait(void) {}

int des_prework_shaders(const char* game, const char* eboot);
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        std::fprintf(stderr, "usage: prework_check <game folder> [EBOOT.elf]\n");
        return 2;
    }
    return des_prework_shaders(argv[1], argc > 2 ? argv[2] : nullptr);
}
