#pragma once

#include "enums.h"
#include <functional>
#include <vector>

namespace rsg {

struct SpellInfo;
class RandomGenerator;

// Filter that decides whether spell should be discarded from pick or not.
// Returns true for spell that should be removed from pick pool
using SpellFilterFunc = std::function<bool(const SpellInfo*)>;
using SpellFilterList = std::initializer_list<SpellFilterFunc>;

// Picks any random spell after applying filters
SpellInfo* pickSpell(RandomGenerator& random, const SpellFilterList& filters);
// Picks random spell of specific type
SpellInfo* pickSpell(SpellType spellType, RandomGenerator& random, const SpellFilterList& filters);

} // namespace rsg
