#include "templatezone.h"
#include "capital.h"
#include "containers.h"
#include "crystal.h"
#include "mapgenerator.h"
#include "player.h"
#include "subrace.h"
#include "unit.h"
#include "village.h"
#include <cassert>
#include <iostream>
#include <iterator>
#include <sstream>

void TemplateZone::setCenter(const VPosition& value)
{
    // Wrap zone around (0, 1) square.
    // If it doesn't fit on one side, will come out on the opposite side.
    center = value;

    center.x = static_cast<float>(std::fmod(center.x, 1));
    center.y = static_cast<float>(std::fmod(center.y, 1));

    if (center.x < 0.f) {
        center.x = 1.f - std::abs(center.x);
    }

    if (center.y < 0.f) {
        center.y = 1.f - std::abs(center.y);
    }
}

void TemplateZone::initTowns()
{
    // Clear ring around forts to ensure crunchPath always hits it
    auto cutPathAroundTown = [this](const Fortification& fort) {
        auto clearPosition = [this](const Position& position) {
            if (mapGenerator->isPossible(position)) {
                mapGenerator->setOccupied(position, TileType::Free);
            }
        };

        for (auto& blocked : fort.getBlockedPositions()) {
            mapGenerator->foreachNeighbor(blocked, clearPosition);
        }

        // Clear town entrance
        mapGenerator->foreachNeighbor(fort.getEntrance() + Position(1, 1), clearPosition);
    };

    std::size_t citiesTotal{};

    auto addCities = [this, &cutPathAroundTown, &citiesTotal](const CityInfo& cityInfo,
                                                              const CMidgardID& ownerId,
                                                              const CMidgardID& subraceId) {
        for (std::size_t tier = 0; tier < cityInfo.cities.size(); ++tier) {
            for (std::uint8_t i = 0; i < cityInfo.cities[tier]; ++i) {
                auto villageId{mapGenerator->createId(CMidgardID::Type::Fortification)};
                auto village{std::make_unique<Village>(villageId)};

                village->setOwner(ownerId);
                village->setSubrace(subraceId);
                village->setTier(tier + 1);

                // Place first city immediately
                if (citiesTotal == 0) {
                    auto villagePtr{village.get()};
                    placeObject(std::move(village), pos - villagePtr->getSize() / 2);
                    cutPathAroundTown(*villagePtr);
                    // All roads lead to tile near central village entrance
                    setPosition(villagePtr->getEntrance() + Position(1, 1));

                    mapGenerator->registerZone(RaceType::Neutral);
                } else {
                    addRequiredObject(std::move(village));
                }

                ++citiesTotal;
            }
        }
    };

    if (type == TemplateZoneType::PlayerStart || type == TemplateZoneType::AiStart) {
        std::cout << "Preparing player zone\n";

        // Create capital id
        auto capitalId{mapGenerator->createId(CMidgardID::Type::Fortification)};
        // Create capital object
        auto capital{std::make_unique<Capital>(capitalId)};
        auto fort{capital.get()};

        assert(ownerId != emptyId);
        fort->setOwner(ownerId);

        auto ownerPlayer{mapGenerator->map->find<Player>(ownerId)};
        assert(ownerPlayer != nullptr);

        auto playerRace{mapGenerator->getRaceType(ownerPlayer->getRace())};

        // Create starting leader unit
        auto leaderId{mapGenerator->createId(CMidgardID::Type::Unit)};
        auto leader{std::make_unique<Unit>(leaderId)};
        leader->setImplId(mapGenerator->map->getStartingLeaderImplId(playerRace));
        leader->setHp(150);
        leader->setName("Leader");
        mapGenerator->insertObject(std::move(leader));

        // Create starting stack
        auto stackId{mapGenerator->createId(CMidgardID::Type::Stack)};
        auto stack{std::make_unique<Stack>(stackId)};
        auto leaderAdded{stack->addLeader(leaderId, 2)};
        assert(leaderAdded);
        stack->setInside(capitalId);
        stack->setMove(20);
        stack->setOwner(ownerId);

        fort->setStack(stackId);

        auto subraceType{mapGenerator->map->getSubRaceType(playerRace)};

        CMidgardID subraceId;
        mapGenerator->map->visit(CMidgardID::Type::SubRace,
                                 [this, subraceType, &subraceId](const ScenarioObject* object) {
                                     auto subrace{dynamic_cast<const SubRace*>(object)};

                                     if (subrace->getType() == subraceType) {
                                         assert(subrace->getPlayerId() == ownerId);
                                         subraceId = subrace->getId();
                                     }
                                 });

        fort->setSubrace(subraceId);
        stack->setSubrace(subraceId);

        // Place capital at the center of the zone
        placeObject(std::move(capital), pos - fort->getSize() / 2,
                    mapGenerator->map->getRaceTerrain(playerRace));
        cutPathAroundTown(*fort);
        // All roads lead to tile near capital entrance
        setPosition(fort->getEntrance() + Position(1, 1));

        mapGenerator->registerZone(playerRace);

        placeObject(std::move(stack), fort->getPosition());
        ++citiesTotal;

        addCities(playerCities, ownerId, subraceId);
        addCities(neutralCities, mapGenerator->getNeutralPlayerId(),
                  mapGenerator->getNeutralSubraceId());
    } else if (type != TemplateZoneType::Water) {
        addCities(neutralCities, mapGenerator->getNeutralPlayerId(),
                  mapGenerator->getNeutralSubraceId());
    }
}

void TemplateZone::initFreeTiles()
{
    std::copy_if(tileInfo.begin(), tileInfo.end(),
                 std::inserter(possibleTiles, possibleTiles.end()),
                 [this](const Position& position) {
                     return this->mapGenerator->isPossible(position);
                 });

    // Zone must have at least one free tile where other paths go - for instance in the center
    if (freePaths.empty()) {
        addFreePath(getPosition());
    }
}

void TemplateZone::createBorder()
{
    for (auto& tile : tileInfo) {
        bool edge{};

        mapGenerator->foreachNeighbor(tile, [this, &edge](const Position& position) {
            if (edge) {
                // Optimization, do it only once
                return;
            }

            // Optimization
            if (mapGenerator->getZoneId(position) == id) {
                return;
            }

            // Fix missing position
            if (mapGenerator->isPossible(position)) {
                mapGenerator->setOccupied(position, TileType::Blocked);
            }

            // Uncomment to generate thick borders, looks too thick for 48 map
            constexpr bool thickBorders{false};

            if constexpr (thickBorders) {
                // We are edge if at least one tile does not belong to zone.
                // Mark all nearby tiles as blocked
                mapGenerator->foreachNeighbor(position, [this](const Position& nearby) {
                    if (mapGenerator->isPossible(nearby)) {
                        mapGenerator->setOccupied(nearby, TileType::Blocked);
                    }
                });

                edge = true;
            }
        });
    }
}

void TemplateZone::fill()
{
    initTerrain();

    addAllPossibleObjects();
    // Zone center should be always clear to allow other tiles to connect
    initFreeTiles();
    connectLater();
    fractalize();
    placeRuins();
    placeMines();
    createRequiredObjects();
    createTreasures();

    std::cout << "Zone " << id << " filled successfully\n";
}

void TemplateZone::createObstacles()
{
    struct Mountain
    {
        int size{1};
        int image{1};
    };

    // clang-format off
    static const Mountain knownMountains[]={
        Mountain{5, 7},
        Mountain{3, 8},
        Mountain{3, 1},
        Mountain{3, 5},
        Mountain{5, 2},
        Mountain{5, 6},
        Mountain{3, 4},
        Mountain{3, 6},
        Mountain{1, 1},
        Mountain{1, 4},
        Mountain{1, 10},
        Mountain{1, 8},
        Mountain{1, 2},
        Mountain{1, 5},
        Mountain{1, 7},
        Mountain{1, 9},
        Mountain{1, 6},
        Mountain{2, 1},
        Mountain{2, 3}
    };
    // clang-format on

    using MountainsVector = std::vector<Mountain>;
    using MountainPair = std::pair<int /* mountain size */, MountainsVector>;

    std::map<int /* mountain size */, MountainsVector> obstaclesBySize;
    std::vector<MountainPair> possibleObstacles;

    for (const auto& mountain : knownMountains) {
        obstaclesBySize[mountain.size].push_back(mountain);
    }

    for (const auto& [size, vector] : obstaclesBySize) {
        possibleObstacles.push_back({size, vector});
    }

    std::sort(possibleObstacles.begin(), possibleObstacles.end(),
              [](const MountainPair& a, const MountainPair& b) {
                  // Bigger mountains first
                  return a.first > b.first;
              });

    auto tryPlaceMountainHere = [this, &possibleObstacles](const Position& tile, int index) {
        auto it{nextItem(possibleObstacles[index].second, mapGenerator->randomGenerator)};

        const MapElement mountainElement({it->size, it->size});
        if (canObstacleBePlacedHere(mountainElement, tile)) {
            placeMountain(tile, mountainElement.getSize(), it->image);
            return true;
        }

        return false;
    };

    for (const auto& tile : tileInfo) {
        // Fill tiles that should be blocked with obstacles
        if (mapGenerator->shouldBeBlocked(tile)) {

            // Start from biggets obstacles
            for (int i = 0; i < (int)possibleObstacles.size(); ++i) {
                if (tryPlaceMountainHere(tile, i)) {
                    break;
                }
            }
        }
    }

    // Cleanup, remove unused possible tiles to make space for roads
    for (auto& tile : tileInfo) {
        if (mapGenerator->isPossible(tile)) {
            mapGenerator->setOccupied(tile, TileType::Free);
        }
    }
}

void TemplateZone::connectRoads()
{
    std::cout << "Started building roads\n";

    std::set<Position> roadNodesCopy{roadNodes};
    std::set<Position> processed;

    while (!roadNodesCopy.empty()) {
        auto node{*roadNodesCopy.begin()};
        roadNodesCopy.erase(node);

        Position cross{-1, -1};

        auto comparator = [&node](const Position& a, const Position& b) {
            return node.distanceSquared(a) < node.distanceSquared(b);
        };

        if (!processed.empty()) {
            // Connect with existing network
            cross = *std::min_element(processed.begin(), processed.end(), comparator);
        } else if (!roadNodesCopy.empty()) {
            // Connect with any other unconnected node
            cross = *std::min_element(roadNodesCopy.begin(), roadNodesCopy.end(), comparator);
        } else {
            // No other nodes left, for example single road node in this zone
            break;
        }

        std::cout << "Building road from " << node << " to " << cross << '\n';
        if (createRoad(node, cross)) {
            // Don't draw road starting at end point which is already connected
            processed.insert(cross);

            eraseIfPresent(roadNodesCopy, cross);
        }

        processed.insert(node);
    }

    std::cout << "Finished building roads\n";
}

ObjectPlacingResult TemplateZone::tryToPlaceObjectAndConnectToPath(MapElement& mapElement,
                                                                   const Position& position)
{
    mapElement.setPosition(position);
    mapGenerator->setOccupied(mapElement.getEntrance(), TileType::Blocked);

    for (const auto& tile : mapElement.getBlockedPositions()) {
        if (mapGenerator->map->isInTheMap(tile)) {
            mapGenerator->setOccupied(tile, TileType::Blocked);
        }
    }

    const auto accessibleOffset{getAccessibleOffset(mapElement, position)};
    if (!accessibleOffset.isValid()) {
        std::cerr << "Can not access required object at position " << position << ", retrying\n";
        return ObjectPlacingResult::CannotFit;
    }

    if (!connectPath(accessibleOffset, true)) {
        std::cerr << "Failed to create path to required object at position " << position
                  << ", retrying\n";
        return ObjectPlacingResult::SealedOff;
    }

    return ObjectPlacingResult::Success;
}

void TemplateZone::addRequiredObject(ScenarioObjectPtr&& object, int guardStrength)
{
    requiredObjects.push_back({std::move(object), guardStrength});
}

void TemplateZone::addCloseObject(ScenarioObjectPtr&& object, int guardStrength)
{
    closeObjects.push_back({std::move(object), guardStrength});
}

// See:
// https://stackoverflow.com/questions/26377430/how-to-perform-a-dynamic-cast-with-a-unique-ptr/26377517
template <typename To, typename From>
std::unique_ptr<To> dynamic_unique_cast(std::unique_ptr<From>&& p)
{
    if (To* cast = dynamic_cast<To*>(p.get())) {
        std::unique_ptr<To> result(dynamic_cast<To*>(p.release()));
        return result;
    }

    throw std::bad_cast();
}

void TemplateZone::placeScenarioObject(ScenarioObjectPtr&& object, const Position& position)
{
    switch (object->getId().getType()) {
    case CMidgardID::Type::Fortification: {
        auto fort{dynamic_unique_cast<Fortification>(std::move(object))};
        placeObject(std::move(fort), position);
        break;
    }

    case CMidgardID::Type::Stack: {
        auto stack{dynamic_unique_cast<Stack>(std::move(object))};
        placeObject(std::move(stack), position);
        break;
    }

    case CMidgardID::Type::Crystal: {
        auto crystal{dynamic_unique_cast<Crystal>(std::move(object))};
        placeObject(std::move(crystal), position);
        break;
    }

    case CMidgardID::Type::Ruin: {
        auto ruin{dynamic_unique_cast<Ruin>(std::move(object))};
        placeObject(std::move(ruin), position);
        break;
    }
    }
}

void TemplateZone::placeObject(std::unique_ptr<Fortification>&& fortification,
                               const Position& position,
                               TerrainType terrain,
                               bool updateDistance)
{
    // Check position
    if (!mapGenerator->map->isInTheMap(position)) {
        CMidgardID::String fortId{};
        fortification->getId().toString(fortId);

        std::stringstream stream;
        stream << "Position of fort " << fortId.data() << " at " << position
               << " is outside of the map\n";
        throw std::runtime_error(stream.str());
    }

    fortification->setPosition(position);

    // Check entrance
    // Since position and entrance form rectangle we don't need to check other tiles
    if (!mapGenerator->map->isInTheMap(fortification->getEntrance())) {
        CMidgardID::String fortId{};
        fortification->getId().toString(fortId);

        std::stringstream stream;
        stream << "Entrance " << fortification->getEntrance() << " of fort " << fortId.data()
               << " at " << position << " is outside of the map\n";
        throw std::runtime_error(stream.str());
    }

    // Mark fort tiles and entrance as used
    auto blocked{fortification->getBlockedPositions()};
    blocked.insert(fortification->getEntrance());

    for (auto& tile : blocked) {
        mapGenerator->setOccupied(tile, TileType::Used);
        // Change terrain under city to race specific
        mapGenerator->paintTerrain(tile, terrain, GroundType::Plain);
    }

    // Update distances
    if (updateDistance) {
        updateDistances(position);
    }

    // Add road node using entrance point
    addRoadNode(fortification->getEntrance());

    mapGenerator->map->insertMapElement(*fortification.get(), fortification->getId());
    // Store object in scenario map
    mapGenerator->insertObject(std::move(fortification));
}

void TemplateZone::placeObject(std::unique_ptr<Stack>&& stack,
                               const Position& position,
                               bool updateDistance)
{
    // Check position
    if (!mapGenerator->map->isInTheMap(position)) {
        CMidgardID::String stackId{};
        stack->getId().toString(stackId);

        std::stringstream stream;
        stream << "Position of stack " << stackId.data() << " at " << position
               << " is outside of the map\n";
        throw std::runtime_error(stream.str());
    }

    stack->setPosition(position);

    // Mark stack tiles as used
    auto blocked{stack->getBlockedPositions()};
    blocked.insert(stack->getEntrance());

    for (auto& tile : blocked) {
        mapGenerator->setOccupied(tile, TileType::Used);
    }

    // Update distances
    if (updateDistance) {
        updateDistances(position);
    }

    mapGenerator->map->insertMapElement(*stack.get(), stack->getId());
    // Store object in scenario map
    mapGenerator->insertObject(std::move(stack));
}

void TemplateZone::placeObject(std::unique_ptr<Crystal>&& crystal,
                               const Position& position,
                               bool updateDistance)
{
    // Check position
    if (!mapGenerator->map->isInTheMap(position)) {
        CMidgardID::String crystalId{};
        crystal->getId().toString(crystalId);

        std::stringstream stream;
        stream << "Position of crystal " << crystalId.data() << " at " << position
               << " is outside of the map\n";
        throw std::runtime_error(stream.str());
    }

    crystal->setPosition(position);

    // Mark crystal tiles as used
    auto blocked{crystal->getBlockedPositions()};
    blocked.insert(crystal->getEntrance());

    for (auto& tile : blocked) {
        mapGenerator->setOccupied(tile, TileType::Used);
    }

    // Update distances
    if (updateDistance) {
        updateDistances(position);
    }

    mapGenerator->map->insertMapElement(*crystal.get(), crystal->getId());
    // Store object in scenario map
    mapGenerator->insertObject(std::move(crystal));
}

void TemplateZone::placeObject(std::unique_ptr<Ruin>&& ruin,
                               const Position& position,
                               bool updateDistance)
{
    // Check position
    if (!mapGenerator->map->isInTheMap(position)) {
        CMidgardID::String ruinId{};
        ruin->getId().toString(ruinId);

        std::stringstream stream;
        stream << "Position of ruin " << ruinId.data() << " at " << position
               << " is outside of the map\n";
        throw std::runtime_error(stream.str());
    }

    ruin->setPosition(position);

    // Check entrance
    // Since position and entrance form rectangle we don't need to check other tiles
    if (!mapGenerator->map->isInTheMap(ruin->getEntrance())) {
        CMidgardID::String ruinId{};
        ruin->getId().toString(ruinId);

        std::stringstream stream;
        stream << "Entrance " << ruin->getEntrance() << " of ruin " << ruinId.data() << " at "
               << position << " is outside of the map\n";
        throw std::runtime_error(stream.str());
    }

    // Mark ruin tiles and entrance as used
    auto blocked{ruin->getBlockedPositions()};
    blocked.insert(ruin->getEntrance());

    for (auto& tile : blocked) {
        mapGenerator->setOccupied(tile, TileType::Used);
    }

    // Update distances
    if (updateDistance) {
        updateDistances(position);
    }

    mapGenerator->map->insertMapElement(*ruin.get(), ruin->getId());
    // Store object in scenario map
    mapGenerator->insertObject(std::move(ruin));
}

void TemplateZone::placeMountain(const Position& position, const Position& size, int image)
{
    for (int x = 0; x < size.x; ++x) {
        for (int y = 0; y < size.y; ++y) {
            const auto pos{position + Position{x, y}};

            if (!mapGenerator->map->isInTheMap(position)) {
                std::stringstream stream;
                stream << "Position of mountain at " << pos << " is outside of the map\n";
                throw std::runtime_error(stream.str());
            }

            mapGenerator->setOccupied(pos, TileType::Used);
        }
    }

    mapGenerator->map->addMountain(position, size, image);
}

void TemplateZone::updateDistances(const Position& position)
{
    for (auto& tile : possibleTiles) {
        const auto distance{static_cast<float>(position.distanceSquared(tile))};
        const auto currentDistance{mapGenerator->getNearestObjectDistance(tile)};

        mapGenerator->setNearestObjectDistance(tile, std::min(distance, currentDistance));
    }
}

void TemplateZone::addRoadNode(const Position& position)
{
    roadNodes.insert(position);
}

void TemplateZone::addFreePath(const Position& position)
{
    mapGenerator->setOccupied(position, TileType::Free);
    freePaths.insert(position);
}

bool TemplateZone::connectWithCenter(const Position& position,
                                     bool onlyStraight,
                                     bool passThroughBlocked)
{
    // A* algorithm

    // Nodes that are already evaluated
    std::set<Position> closed;
    // The set of tentative nodes to be evaluated, initially containing the start node
    PriorityQueue queue;
    // Map of navigated nodes
    std::map<Position, Position> cameFrom;
    std::map<Position, float> distances;

    // First node points to finish condition.
    // Invalid position of (-1 -1) used as stop element
    cameFrom[position] = Position(-1, -1);
    queue.push(std::make_pair(position, 0.f));
    distances[position] = 0.f;

    while (!queue.empty()) {
        auto node = queue.top();
        // Remove top element
        queue.pop();

        const auto& currentNode{node.first};
        closed.insert(currentNode);

        // Reached center of the zone, stop
        if (currentNode == pos) {
            // Trace the path using the saved parent information and return path
            Position backTracking{currentNode};
            while (cameFrom[backTracking].isValid()) {
                mapGenerator->setOccupied(backTracking, TileType::Free);
                backTracking = cameFrom[backTracking];
            }

            return true;
        } else {
            auto functor = [this, &queue, &closed, &cameFrom, &currentNode, &distances,
                            passThroughBlocked](Position& p) {
                if (contains(closed, p)) {
                    return;
                }

                if (mapGenerator->getZoneId(p) != id) {
                    return;
                }

                float movementCost{};
                if (mapGenerator->isFree(p)) {
                    movementCost = 1.f;
                } else if (mapGenerator->isPossible(p)) {
                    movementCost = 2.f;
                } else if (passThroughBlocked && mapGenerator->shouldBeBlocked(p)) {
                    movementCost = 3.f;
                } else {
                    return;
                }

                // We prefer to use already free paths
                const float distance{distances[currentNode] + movementCost};
                auto bestDistanceSoFar{std::numeric_limits<int>::max()};

                auto it{distances.find(p)};
                if (it != distances.end()) {
                    bestDistanceSoFar = static_cast<int>(it->second);
                }

                if (distance < bestDistanceSoFar) {
                    cameFrom[p] = currentNode;
                    queue.push(std::make_pair(p, distance));
                    distances[p] = distance;
                }
            };

            if (onlyStraight) {
                mapGenerator->foreachDirectNeighbor(currentNode, functor);
            } else {
                mapGenerator->foreachNeighbor(currentNode, functor);
            }
        }
    }

    return false;
}

bool TemplateZone::crunchPath(const Position& source,
                              const Position& destination,
                              bool onlyStraight,
                              std::set<Position>* clearedTiles)
{
    bool result{};
    bool end{};

    Position currentPosition{source};
    auto distance{static_cast<float>(currentPosition.distanceSquared(destination))};

    while (!end) {
        if (currentPosition == destination) {
            result = true;
            break;
        }

        auto lastDistance{distance};

        auto processNeighbors = [this, &currentPosition, &destination, &distance, &result, &end,
                                 clearedTiles](Position& position) {
            if (result) {
                return;
            }

            if (position == destination) {
                result = true;
                end = true;
            }

            if (position.distanceSquared(destination) >= distance) {
                return;
            }

            if (mapGenerator->isBlocked(position)) {
                return;
            }

            if (mapGenerator->getZoneId(position) != id) {
                return;
            }

            if (mapGenerator->isPossible(position)) {
                mapGenerator->setOccupied(position, TileType::Free);
                if (clearedTiles) {
                    clearedTiles->insert(position);
                }

                currentPosition = position;
                distance = static_cast<float>(currentPosition.distanceSquared(destination));
            } else if (mapGenerator->isFree(position)) {
                end = true;
                result = true;
            }
        };

        if (onlyStraight) {
            mapGenerator->foreachDirectNeighbor(currentPosition, processNeighbors);
        } else {
            mapGenerator->foreachNeighbor(currentPosition, processNeighbors);
        }

        Position anotherPosition{-1, -1};

        // We do not advance, use more advanced pathfinding algorithm?
        if (!(result || distance < lastDistance)) {
            // Try any nearby tiles, even if its not closer than current
            // Start with significantly larger value
            float lastDistance{2 * distance};

            auto functor = [this, &currentPosition, &destination, &lastDistance, &anotherPosition,
                            clearedTiles](Position& position) {
                // Try closest tiles from all surrounding unused tiles
                if (currentPosition.distanceSquared(destination) >= lastDistance) {
                    return;
                }

                if (mapGenerator->getZoneId(position) != id) {
                    return;
                }

                if (!mapGenerator->isPossible(position)) {
                    return;
                }

                if (clearedTiles) {
                    clearedTiles->insert(position);
                }

                anotherPosition = position;
                lastDistance = static_cast<float>(currentPosition.distanceSquared(destination));
            };

            if (onlyStraight) {
                mapGenerator->foreachDirectNeighbor(currentPosition, functor);
            } else {
                mapGenerator->foreachNeighbor(currentPosition, functor);
            }

            if (anotherPosition.isValid()) {
                if (clearedTiles) {
                    clearedTiles->insert(anotherPosition);
                }

                mapGenerator->setOccupied(anotherPosition, TileType::Free);
                currentPosition = anotherPosition;
            }
        }

        if (!(result || distance < lastDistance || anotherPosition.isValid())) {
            std::cout << "No tile closer than " << currentPosition << " found on path from "
                      << source << " to " << destination << '\n';
            break;
        }
    }

    return result;
}

bool TemplateZone::connectPath(const Position& source, bool onlyStraight)
{
    // A* algorithm

    // The set of nodes already evaluated
    std::set<Position> closed;
    // The set of tentative nodes to be evaluated, initially containing the start node
    PriorityQueue open;
    // The map of navigated nodes
    std::map<Position, Position> cameFrom;
    std::map<Position, float> distances;

    // First node points to finish condition
    cameFrom[source] = Position{-1, -1};
    distances[source] = 0.f;
    open.push({source, 0.f});

    // Cost from start along best known path.
    // Estimated total cost from start to goal through y.

    while (!open.empty()) {
        auto node{open.top()};
        open.pop();
        const auto currentNode{node.first};

        closed.insert(currentNode);

        // We reached free paths, stop
        if (mapGenerator->isFree(currentNode)) {
            // Trace the path using the saved parent information and return path
            auto backTracking{currentNode};
            while (cameFrom[backTracking].isValid()) {
                mapGenerator->setOccupied(backTracking, TileType::Free);
                backTracking = cameFrom[backTracking];
            }

            return true;
        }

        auto functor = [this, &open, &closed, &cameFrom, &currentNode, &distances](Position& pos) {
            if (contains(closed, pos)) {
                return;
            }

            // No paths through blocked or occupied tiles, stay within zone
            if (mapGenerator->isBlocked(pos) || mapGenerator->getZoneId(pos) != id) {
                return;
            }

            const auto distance{static_cast<int>(distances[currentNode]) + 1};
            int bestDistanceSoFar{std::numeric_limits<int>::max()};

            auto it{distances.find(pos)};
            if (it != distances.end()) {
                bestDistanceSoFar = static_cast<int>(it->second);
            }

            if (distance < bestDistanceSoFar) {
                cameFrom[pos] = currentNode;
                open.push({pos, static_cast<float>(distance)});
                distances[pos] = static_cast<float>(distance);
            }
        };

        if (onlyStraight) {
            mapGenerator->foreachDirectNeighbor(currentNode, functor);
        } else {
            mapGenerator->foreachNeighbor(currentNode, functor);
        }
    }

    // These tiles are sealed off and can't be connected anymore
    for (const auto& tile : closed) {
        if (mapGenerator->isPossible(tile)) {
            mapGenerator->setOccupied(tile, TileType::Blocked);
        }

        eraseIfPresent(possibleTiles, tile);
    }

    return false;
}

bool TemplateZone::addStack(const Position& position,
                            int strength,
                            bool clearSurroundingTiles,
                            bool zoneGuard)
{
    auto leaderId{mapGenerator->createId(CMidgardID::Type::Unit)};
    auto leader{std::make_unique<Unit>(leaderId)};
    // Use Ork leader for testing
    leader->setImplId(CMidgardID("g000uu5113"));
    leader->setHp(200);
    leader->setName("Ork");
    mapGenerator->insertObject(std::move(leader));

    auto stackId{mapGenerator->createId(CMidgardID::Type::Stack)};
    auto stack{std::make_unique<Stack>(stackId)};
    auto leaderAdded{stack->addLeader(leaderId, 2)};
    assert(leaderAdded);

    stack->setMove(20);
    stack->setOwner(mapGenerator->getNeutralPlayerId());
    stack->setSubrace(mapGenerator->getNeutralSubraceId());

    placeObject(std::move(stack), position);

    if (clearSurroundingTiles) {
        // Do not spawn anything near stack
        mapGenerator->foreachNeighbor(position, [this](Position& tile) {
            if (mapGenerator->isPossible(tile)) {
                mapGenerator->setOccupied(tile, TileType::Free);
            }
        });
    }

    return true;
}

void TemplateZone::initTerrain()
{
    if (type == TemplateZoneType::Water) {
        paintZoneTerrain(TerrainType::Neutral, GroundType::Water);
        return;
    }

    // TODO: create random patches of race-specific terrains,
    // excluding playable races in scenario
    // paintZoneTerrain(TerrainType::Neutral, GroundType::Plain);
}

void TemplateZone::addAllPossibleObjects()
{
    // VCMI populates vector of possible objects with small structures
    // describing rmg values, probability, maximum per zone and creation functor
    // It adds:
    // limited quantity:
    // non-static objects (what ?)
    // prisons

    // unlimited quantity:
    // dwellings
    // spell scrolls
    // pandoras boxes
    // seer huts and their rewards

    // What should I add here ?
    // Disciples have not too many objects,
    // we can list them all and their quantity & probability in map template
}

void TemplateZone::connectLater()
{ }

void TemplateZone::fractalize()
{
    for (const auto& tile : tileInfo) {
        if (mapGenerator->isFree(tile)) {
            freePaths.insert(tile);
        }
    }

    std::vector<Position> clearedTiles(freePaths.begin(), freePaths.end());
    std::set<Position> possibleTiles;
    std::set<Position> tilesToIgnore;

    int totalDensity{};
    for (const auto& treasure : treasureInfo) {
        totalDensity += treasure.density;
    }

    // TODO: move this setting into template for better zone free space control
    // TODO: adjust this setting based on template value
    // and number of objects (and their average size?)
    const float minDistance{7.5 * 10};

    for (const auto& tile : tileInfo) {
        if (mapGenerator->isPossible(tile)) {
            possibleTiles.insert(tile);
        }
    }

    // This should come from zone connections
    assert(!clearedTiles.empty());
    // Connect them with a grid
    std::vector<Position> nodes;

    if (type != TemplateZoneType::Junction) {
        // Junction is not fractalized,
        // has only one straight path everything else remains blocked
        while (!possibleTiles.empty()) {
            // Link tiles in random order
            std::vector<Position> tilesToMakePath(possibleTiles.begin(), possibleTiles.end());
            randomShuffle(tilesToMakePath, mapGenerator->randomGenerator);

            Position nodeFound{-1, -1};

            for (const auto& tileToMakePath : tilesToMakePath) {
                // Find closest free tile
                float currentDistance{1e10};
                Position closestTile{-1, -1};

                for (const auto& clearTile : clearedTiles) {
                    auto distance{static_cast<float>(tileToMakePath.distanceSquared(clearTile))};
                    if (distance < currentDistance) {
                        currentDistance = distance;
                        closestTile = clearTile;
                    }

                    if (currentDistance <= minDistance) {
                        // This tile is close enough. Forget about it and check next one
                        tilesToIgnore.insert(tileToMakePath);
                        break;
                    }
                }

                // If tiles is not close enough, make path to it
                if (currentDistance > minDistance) {
                    nodeFound = tileToMakePath;
                    nodes.push_back(nodeFound);
                    // From now on nearby tiles will be considered handled
                    clearedTiles.push_back(nodeFound);
                    // Next iteration - use already cleared tiles
                    break;
                }
            }

            // These tiles are already connected, ignore them
            for (const auto& tileToClear : tilesToIgnore) {
                eraseIfPresent(possibleTiles, tileToClear);
            }

            // Nothing else can be done (?)
            if (!nodeFound.isValid()) {
                break;
            }

            tilesToIgnore.clear();
        }
    }

    // Cut straight paths towards the center
    for (const auto& node : nodes) {
        auto subnodes{nodes};

        std::sort(subnodes.begin(), subnodes.end(), [&node](const Position& a, const Position& b) {
            return node.distanceSquared(a) < node.distanceSquared(b);
        });

        std::vector<Position> nearbyNodes;
        if (subnodes.size() >= 2) {
            // node[0] is our node we want to connect
            nearbyNodes.push_back(subnodes[1]);
        }

        if (subnodes.size() >= 3) {
            nearbyNodes.push_back(subnodes[2]);
        }

        // Connect with all the paths
        crunchPath(node, findClosestTile(freePaths, node), true, &freePaths);
        // Connect with nearby nodes
        for (const auto& nearbyNode : nearbyNodes) {
            // Do not allow to make another path network
            crunchPath(node, nearbyNode, true, &freePaths);
        }
    }

    // Make sure they are clear
    for (const auto& node : nodes) {
        mapGenerator->setOccupied(node, TileType::Free);
    }

    // Now block most distant tiles away from passages
    const float blockDistance{minDistance * 0.25f};

    for (const auto& tile : tileInfo) {
        if (!mapGenerator->isPossible(tile)) {
            continue;
        }

        if (freePaths.count(tile)) {
            continue;
        }

        bool closeTileFound{};
        for (const auto& clearTile : freePaths) {
            const auto distance{static_cast<float>(tile.distanceSquared(clearTile))};

            if (distance < blockDistance) {
                closeTileFound = true;
                break;
            }
        }

        if (!closeTileFound) {
            // This tile is far enough from passages
            mapGenerator->setOccupied(tile, TileType::Blocked);
        }
    }

    constexpr bool debugFractalize{false};

    if constexpr (debugFractalize) {
        char name[100] = {0};
        std::snprintf(name, sizeof(name) - 1, "zone %d fractalize.png", id);

        mapGenerator->debugTiles(name);
    }
}

void TemplateZone::placeRuins()
{
    /*
    Vanilla ruin images:
    0 - ambar
    1 - small castle ruins
    2 - farm ruins
    3 - squared with colonnade
    4 - tower
    5 - squared with red roof
    6 - tower in mountains
    7 - circular panteon
    8 - mountain clans style
    9 - water temple
    10 - elven cottage
    */
    static const int ruinImages[] = {0, 1, 2, 3, 4, 5, 6, 7};

    auto& rand{mapGenerator->randomGenerator};

    for (const auto& ruinInfo : ruins) {
        auto ruinId{mapGenerator->createId(CMidgardID::Type::Ruin)};
        auto ruin{std::make_unique<Ruin>(ruinId)};

        const auto cashGold{rand.getInt64Range(ruinInfo.cash.min, ruinInfo.cash.max)()};
        Currency cash;
        cash.set(Currency::Type::Gold, static_cast<std::uint16_t>(cashGold));

        // TODO: create specific item if itemId is not empty
        // create reward item if itemId is empty and item value is set
        ruin->setCash(cash);
        ruin->setTitle("Ruin");
        int ruinImage = (int)rand.getInt64Range(0, std::size(ruinImages) - 1)();
        ruin->setImage(ruinImage);

        const auto unitId{mapGenerator->createId(CMidgardID::Type::Unit)};
        auto unitAdded{ruin->addUnit(unitId, 2)};
        assert(unitAdded);

        auto unit{std::make_unique<Unit>(unitId)};
        // Use non-leader Ork for testing
        unit->setImplId(CMidgardID("g000uu5013"));
        unit->setHp(200);
        mapGenerator->insertObject(std::move(unit));

        addRequiredObject(std::move(ruin));
    }
}

bool TemplateZone::placeMines()
{
    auto nativeResource{mapGenerator->map->getNativeResource(RaceType::Neutral)};

    if (ownerId != emptyId) {
        auto player{mapGenerator->map->find<Player>(ownerId)};
        assert(player != nullptr);

        const auto ownerRace{mapGenerator->map->getRaceType(player->getRace())};
        nativeResource = mapGenerator->map->getNativeResource(ownerRace);
    }

    for (const auto& mineInfo : mines) {
        const auto resourceType{mineInfo.first};

        for (std::uint8_t i = 0; i < mineInfo.second; ++i) {
            auto crystalId{mapGenerator->createId(CMidgardID::Type::Crystal)};
            auto crystal{std::make_unique<Crystal>(crystalId)};

            crystal->setResourceType(resourceType);

            // Only first gold mine and mana crystal are placed close
            if (i == 0 && (resourceType == nativeResource || resourceType == ResourceType::Gold)) {
                addCloseObject(std::move(crystal));
            } else {
                addRequiredObject(std::move(crystal));
            }
        }
    }

    return true;
}

bool TemplateZone::createRequiredObjects()
{
    std::cout << "Creating required objects\n";

    for (auto& pair : requiredObjects) {
        auto& object{pair.first};
        Position position;

        auto mapElement{dynamic_cast<MapElement*>(object.get())};
        if (!mapElement) {
            std::cerr << "Required object is not MapElement!\n";
            return false;
        }

        while (true) {
            const auto elementSize{mapElement->getSize().x};
            const auto sizeSquared{elementSize * elementSize};
            // TODO: move this setting into template for better object placement ?
            const auto minDistance{elementSize * 2};

            if (!findPlaceForObject(*mapElement, minDistance, position)) {
                std::cerr << "Failed to fill zone " << id << " due to lack of space\n";
                return false;
            }

            if (tryToPlaceObjectAndConnectToPath(*mapElement, position)
                == ObjectPlacingResult::Success) {

                placeScenarioObject(std::move(object), position);
                // TODO:
                // guardObject();
                break;
            }
        }
    }

    for (auto& pair : closeObjects) {
        auto& object{pair.first};

        auto mapElement{dynamic_cast<MapElement*>(object.get())};
        if (!mapElement) {
            std::cerr << "Close object is not MapElement!\n";
            return false;
        }

        const auto tilesBlockedByObject{mapElement->getBlockedOffsets()};

        bool finished{};
        bool attempt{true};
        while (!finished && attempt) {
            attempt = false;

            std::vector<Position> tiles(possibleTiles.begin(), possibleTiles.end());
            // New tiles vector after each object has been placed,
            // OR misplaced area has been sealed off

            auto eraseFunc = [this, mapElement](Position& tile) {
                // Object must be accessible from at least one surounding
                // tile and must not be at the border of the map
                return mapGenerator->map->isAtTheBorder(tile)
                       || mapGenerator->map->isAtTheBorder(*mapElement, tile)
                       || !isAccessibleFromSomewhere(*mapElement, tile);
            };

            tiles.erase(std::remove_if(tiles.begin(), tiles.end(), eraseFunc), tiles.end());

            auto targetPosition{requestedPositions.find(object.get()) != requestedPositions.end()
                                    ? requestedPositions[object.get()]
                                    : pos};
            // Smallest distance to zone center, greatest distance to nearest object
            auto isCloser = [this, &targetPosition, &tilesBlockedByObject](const Position& a,
                                                                           const Position& b) {
                float lDist{std::numeric_limits<float>::max()};
                float rDist{std::numeric_limits<float>::max()};

                for (auto t : tilesBlockedByObject) {
                    t += targetPosition;
                    lDist = fmin(lDist, static_cast<float>(t.distance(a)));
                    rDist = fmin(rDist, static_cast<float>(t.distance(b)));
                }

                // Objects within 12 tile radius are preferred
                // (smaller distance rating)
                lDist *= (lDist > 12) ? 10 : 1;
                rDist *= (rDist > 12) ? 10 : 1;

                return (lDist * 0.5f - std::sqrt(mapGenerator->getNearestObjectDistance(a)))
                       < (rDist * 0.5f - std::sqrt(mapGenerator->getNearestObjectDistance(b)));
            };

            std::sort(tiles.begin(), tiles.end(), isCloser);

            if (tiles.empty()) {
                std::cerr << "Failed to fill zone " << id << " due to lack of space\n";
                return false;
            }

            for (const auto& tile : tiles) {
                // Code partially adapted from findPlaceForObject()
                if (!areAllTilesAvailable(*mapElement, tile, tilesBlockedByObject)) {
                    continue;
                }

                attempt = true;

                auto result{tryToPlaceObjectAndConnectToPath(*mapElement, tile)};
                if (result == ObjectPlacingResult::Success) {
                    placeScenarioObject(std::move(object), tile);
                    // TODO:
                    // guardObject();
                    finished = true;
                    break;
                }

                if (result == ObjectPlacingResult::CannotFit) {
                    // Next tile
                    continue;
                }

                // tiles expired, pick new ones
                if (result == ObjectPlacingResult::SealedOff) {
                    break;
                }

                throw std::runtime_error("Wrong result of tryToPlaceObjectAndConnectToPath()");
            }
        }
    }

    requiredObjects.clear();
    closeObjects.clear();

    return true;
}

void TemplateZone::createTreasures()
{ }

bool TemplateZone::findPlaceForObject(const MapElement& mapElement,
                                      int minDistance,
                                      Position& position)
{
    float bestDistance{0.f};
    bool result{};

    auto blockedOffsets{mapElement.getBlockedOffsets()};

    for (const auto& tile : tileInfo) {
        // Avoid borders
        if (mapGenerator->map->isAtTheBorder(mapElement, tile)) {
            continue;
        }

        if (!isAccessibleFromSomewhere(mapElement, tile)) {
            continue;
        }

        if (!isEntranceAccessible(mapElement, tile)) {
            continue;
        }

        const auto& t = mapGenerator->getTile(tile);
        const auto distance{t.getNearestObjectDistance()};

        const auto isPossible{mapGenerator->isPossible(tile)};
        const auto distanceMoreThanMin{distance >= minDistance};
        const auto distanceMoreThanBest{distance > bestDistance};

        // Avoid borders
        if (isPossible && (distanceMoreThanMin) && (distanceMoreThanBest)) {
            if (areAllTilesAvailable(mapElement, tile, blockedOffsets)) {
                bestDistance = distance;
                position = tile;
                result = true;
            }
        }
    }

    // Block found tile
    if (result) {
        mapGenerator->setOccupied(position, TileType::Blocked);
    }

    return result;
}

bool TemplateZone::isAccessibleFromSomewhere(const MapElement& mapElement,
                                             const Position& position) const
{
    return getAccessibleOffset(mapElement, position).isValid();
}

bool TemplateZone::isEntranceAccessible(const MapElement& mapElement,
                                        const Position& position) const
{
    const auto entrance{position + mapElement.getEntranceOffset()};

    // If at least one tile nearby entrance is inaccessible
    // assume whole map element is also inaccessible
    for (const auto& offset : mapElement.getEntranceOffsets()) {
        const auto entranceTile{entrance + offset};

        if (!mapGenerator->map->isInTheMap(entranceTile)) {
            return false;
        }

        if (mapGenerator->isBlocked(entranceTile)) {
            return false;
        }
    }

    return true;
}

Position TemplateZone::getAccessibleOffset(const MapElement& mapElement,
                                           const Position& position) const
{
    const auto blocked{mapElement.getBlockedOffsets()};
    Position result{-1, -1};

    for (int x = -1; x < 2; ++x) {
        for (int y = -1; y < 2; ++y) {
            // Check only if object is visitable from another tile
            if (x == 0 && y == 0) {
                continue;
            }

            const Position offset{Position{x, y} + mapElement.getEntranceOffset()};

            if (contains(blocked, offset)) {
                continue;
            }

            const Position nearbyPos{position + offset};
            if (!mapGenerator->map->isInTheMap(nearbyPos)) {
                continue;
            }

            if (mapElement.isVisitableFrom({x, y}) && !mapGenerator->isBlocked(nearbyPos)
                && tileInfo.find(nearbyPos) != tileInfo.end()) {
                result = nearbyPos;
            }
        }
    }

    return result;
}

bool TemplateZone::areAllTilesAvailable(const MapElement& mapElement,
                                        const Position& position,
                                        const std::set<Position>& blockedOffsets) const
{
    for (const auto& offset : blockedOffsets) {
        const auto t{position + offset};

        if (!mapGenerator->map->isInTheMap(t) || !mapGenerator->isPossible(t)
            || mapGenerator->getZoneId(t) != id) {
            // If at least one tile is not possible, object can't be placed here
            return false;
        }
    }

    return true;
}

bool TemplateZone::canObstacleBePlacedHere(const MapElement& mapElement,
                                           const Position& position) const
{
    // Blockmap may fit in the map, but botom-right corner does not
    if (!mapGenerator->map->isInTheMap(position)) {
        return false;
    }

    auto blockedOffsets{mapElement.getBlockedOffsets()};
    for (const auto& offset : blockedOffsets) {
        const Position t{position + offset};

        if (!mapGenerator->map->isInTheMap(t)) {
            return false;
        }

        if (!mapGenerator->shouldBeBlocked(t)) {
            return false;
        }

        /*const auto isPossible{mapGenerator->isPossible(t)};
        const auto shouldBeBlocked{mapGenerator->shouldBeBlocked(t)};

        if (!(isPossible || shouldBeBlocked)) {
            // If at least one tile is not possible, object can't be placed here
            return false;
        }*/
    }

    return true;
}

void TemplateZone::paintZoneTerrain(TerrainType terrain, GroundType ground)
{
    std::vector<Position> tiles(tileInfo.begin(), tileInfo.end());
    mapGenerator->paintTerrain(tiles, terrain, ground);
}

std::set<Position> TemplateZone::getRoads() const
{
    std::set<Position> tiles;
    for (const auto& tile : roads) {
        if (mapGenerator->map->isInTheMap(tile)) {
            tiles.insert(tile);
        }
    }

    for (const auto& tile : roadNodes) {
        // Mark roads for our nodes, but not for zone guards in other zones
        if (mapGenerator->getZoneId(tile) == id) {
            tiles.insert(tile);
        }
    }

    return tiles;
}

bool TemplateZone::createRoad(const Position& source, const Position& destination)
{
    // A* algorithm

    // The set of nodes already evaluated
    std::set<Position> closed;
    // The set of tentative nodes to be evaluated, initially containing the start node
    PriorityQueue queue;
    // The map of navigated nodes
    std::map<Position, Position> cameFrom;
    std::map<Position, float> distances;

    // Just in case zone guard already has road under it
    // Road under nodes will be added at very end
    mapGenerator->setRoad(source, false);

    // First node points to finish condition
    cameFrom[source] = Position{-1, -1};
    queue.push({source, 0.f});
    distances[source] = 0.f;
    // Cost from start along best known path

    while (!queue.empty()) {
        auto node{queue.top()};
        queue.pop();

        auto& currentNode{node.first};
        closed.insert(currentNode);

        if (currentNode == destination || mapGenerator->isRoad(currentNode)) {
            // The goal node was reached.
            // Trace the path using the saved parent information and return path
            Position backtracking{currentNode};
            while (cameFrom[backtracking].isValid()) {
                // Add node to path
                roads.insert(backtracking);
                mapGenerator->setRoad(backtracking, true);
                backtracking = cameFrom[backtracking];
            }

            return true;
        }

        const auto& currentTile{mapGenerator->map->getTile(currentNode)};
        bool directNeighbourFound{false};
        float movementCost{1.f};

        auto functor = [this, &queue, &distances, &closed, &cameFrom, &currentNode, &currentTile,
                        &node, &destination, &directNeighbourFound, &movementCost](Position& p) {
            if (contains(closed, p)) {
                // We already visited that node
                return;
            }

            float distance{node.second + movementCost};
            float bestDistanceSoFar{std::numeric_limits<float>::max()};

            auto it{distances.find(p)};
            if (it != distances.end()) {
                bestDistanceSoFar = it->second;
            }

            if (distance >= bestDistanceSoFar) {
                return;
            }

            auto& tile{mapGenerator->map->getTile(p)};
            const auto canMoveBetween{mapGenerator->map->canMoveBetween(currentNode, p)};

            const auto emptyPath{mapGenerator->isFree(p) && mapGenerator->isFree(currentNode)};
            // Moving from or to visitable object
            const auto visitable{(tile.visitable || currentTile.visitable) && canMoveBetween};
            // Already completed the path
            const auto completed{p == destination};

            if (emptyPath || visitable || completed) {
                // Otherwise guard position may appear already connected to other zone.
                if (mapGenerator->getZoneId(p) == id || completed) {
                    cameFrom[p] = currentNode;
                    distances[p] = distance;
                    queue.push({p, distance});
                    directNeighbourFound = true;
                }
            }
        };

        // Roads cannot be placed diagonally
        mapGenerator->foreachDirectNeighbor(currentNode, functor);
        if (!directNeighbourFound) {
            // Moving diagonally is penalized over moving two tiles straight
            movementCost = 2.1f;
            mapGenerator->foreachDiagonalNeighbor(currentNode, functor);
        }
    }

    std::cout << "Failed create road from " << source << " to " << destination << '\n';
    return false;
}
