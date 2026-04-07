#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <glm/glm.hpp>

struct Vertex
{
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;

    bool operator==(const Vertex &o) const
    {
        return pos == o.pos && color == o.color &&
               texCoord == o.texCoord && normal == o.normal;
    }
};

// Hash for Vertex — needed by loadOBJ to deduplicate vertices.
template <>
struct std::hash<Vertex>
{
    size_t operator()(const Vertex &v) const noexcept
    {
        // Combine hashes of every field. The magic constant is from Boost.
        auto h = [](size_t seed, size_t val) {
            return seed ^ (val + 0x9e3779b9 + (seed << 6) + (seed >> 2));
        };
        auto fh = [](float f) { return std::hash<float>{}(f); };
        size_t s = 0;
        s = h(s, fh(v.pos.x));      s = h(s, fh(v.pos.y));      s = h(s, fh(v.pos.z));
        s = h(s, fh(v.color.x));    s = h(s, fh(v.color.y));    s = h(s, fh(v.color.z));
        s = h(s, fh(v.texCoord.x)); s = h(s, fh(v.texCoord.y));
        s = h(s, fh(v.normal.x));   s = h(s, fh(v.normal.y));   s = h(s, fh(v.normal.z));
        return s;
    }
};

std::pair<std::vector<Vertex>, std::vector<uint32_t>> makeCube(glm::vec3 color, float size);
std::pair<std::vector<Vertex>, std::vector<uint32_t>> makePlane(glm::vec3 color, float size);
std::pair<std::vector<Vertex>, std::vector<uint32_t>> loadOBJ(const std::string &path);
