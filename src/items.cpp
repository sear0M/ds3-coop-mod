#include "items.h"

namespace items {
namespace {

struct RawRow {
    const char* category;  // set on CATEGORY rows only
    int kind;
    const char* group;  // set on ITEM rows only
    const char* name;
    uint32_t id;
};

#define CATEGORY(name, kind) {name, kind, nullptr, nullptr, 0},
#define ITEM(group, name, id) {nullptr, 0, group, name, id},
const RawRow kRows[] = {
#include "items_data.inc"
};
#undef CATEGORY
#undef ITEM

std::vector<Category> Build() {
    std::vector<Category> out;
    for (const RawRow& row : kRows) {
        if (row.category) {
            out.push_back(Category{row.category, static_cast<Kind>(row.kind), {}});
            continue;
        }
        if (out.empty()) continue;
        auto& groups = out.back().groups;
        if (groups.empty() || groups.back().name != row.group) groups.push_back(Group{row.group, {}});
        groups.back().items.push_back(Item{row.name, row.id});
    }
    return out;
}

}  // namespace

const std::vector<Category>& All() {
    static const std::vector<Category> categories = Build();
    return categories;
}

}  // namespace items
