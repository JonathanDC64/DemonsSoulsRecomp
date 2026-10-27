// Written for Demon's Souls. Two of the game's own models for the UI, read from the player's copy of the game
// (docs/112): the Large Sword of Moonlight, drawn as the menus' cursor, and the crystal lizard in its little dance.
// Nothing of the game is in the program: the files are read from the game's folder ($PS3_VFS_ROOT/PS3_GAME/USRDIR)
// the first time a model is drawn, and kept in memory.
//
// The formats, read here from scratch: BND3 archives, FLVER0 models (big-endian), PS3 TPF textures (BC1/BC2/BC3 blocks,
// the same bytes as on the PC), Havok 5.5 packfiles (big-endian, 4-byte pointers) for the skeleton and the animations.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ui::models {

struct Vertex {
    float pos[3];
    float normal[3];
    float uv[2];
};

enum class TexFormat { BC1, BC2, BC3 };

struct Texture {
    std::string name;
    TexFormat format = TexFormat::BC1;
    int width = 0, height = 0, levels = 1;  // levels: those the data holds, largest first
    std::vector<uint8_t> data;
};

struct Mesh {
    std::vector<Vertex> vertices;  // as the model has them (the bind pose)
    std::vector<uint32_t> indices;  // triangles
    int texture = -1;               // the diffuse texture, in textures()
    int specular = -1;              // the specular texture (its colour is the highlight's), or -1
    bool blend = false;             // alpha blended (an *_Alp material): drawn after the opaque meshes, without culling
    // Skinning (empty for a rigid model): up to four of the model's bones per vertex, and their weights.
    std::vector<std::array<uint16_t, 4>> bones;
    std::vector<std::array<float, 4>> weights;
};

struct Model;

// The model by name: "moonlight" or "lizard". Read the first time; nullptr if the game's files are not there or do
// not read (said once on stderr). The UI's render thread.
const Model* get(const std::string& name);

const std::vector<Mesh>& meshes(const Model& m);
const std::vector<Texture>& textures(const Model& m);
// The model's bounding box: around its rest pose, or for an animated model around every frame of its animation.
void bounds(const Model& m, float lo[3], float hi[3]);
// Its animation's length in seconds (0 for a rigid model).
double duration(const Model& m);
// The vertices of each mesh at time t in seconds (looping), for an animated model. False for a rigid one.
bool pose(const Model& m, double t, std::vector<std::vector<Vertex>>& out);

// How a model is shown, as the previews had it. rotation: model space to the screen's (x right, y down, z toward the
// viewer), its rows: the sword with its tip to the top left, the lizard seen from its front left and a little above;
// each turned `turn` degrees more (the sword about its blade, the lizard about its up). light: toward the light, in
// the screen's space, and ambient the part of it that is everywhere. tint: the blended meshes' colour (the blade's
// moonlight blue). alpha_test: below it an opaque mesh's texel is not drawn.
struct Look {
    float rotation[9];
    float light[3];
    float ambient;
    float tint[3];
    float alpha_test;
};
Look look(const std::string& name, float turn);

// The model's slow turn, in degrees a second (0: none): DES_CURSOR_SPIN for the sword, about its blade,
// DES_UI_LIZARD_SPIN for the lizard, about its up. turn = spin * seconds, for look.
float spin(const std::string& name);

}  // namespace ui::models
