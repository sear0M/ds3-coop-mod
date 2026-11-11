#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace items {

enum class Kind {
    Weapon = 0,     // upgrade level and infusion apply
    Stackable = 1,  // quantity applies (arrows, consumables, souls, titanite...)
    ArmorSet = 2,   // each group is one armor set
    Single = 3,     // one copy (rings, spells, keys, gestures...)
};

struct Item {
    std::string name;
    uint32_t id;
};

struct Group {
    std::string name;
    std::vector<Item> items;
};

struct Category {
    std::string name;
    Kind kind;
    std::vector<Group> groups;
};

const std::vector<Category>& All();

}  // namespace items
