#include "UsrAI.h"
#include <cmath>
#include <cstdlib>
#include <climits>
#include <algorithm>
#include <vector>
#include <map>
#include <set>
#include <limits>
#include <sstream>
using namespace std;

tagGame tagUsrGame;
ins UsrIns;
/*##########DO NOT MODIFY THE CODE ABOVE##########*/

namespace {

const int kHomeBerryRadius = 18;
const int kDropoffResourceRadius = 16;
const int kLateHomeCount = 12;
const int kOpeningBuilderCount = 2;
const int kHerdRadius = 12;
const int kFarmTargetCount = 6;

int distance2(int x1, int y1, int x2, int y2)
{
    const int dx = x1 - x2;
    const int dy = y1 - y2;
    return dx * dx + dy * dy;
}

int buildingSide(int type)
{
    return (type == BUILDING_HOME || type == BUILDING_ARROWTOWER) ? 2 : 3;
}

bool isPlacementError(int result)
{
    return result == ACTION_INVALID_HUMANBUILD_DIFFERENTHIGH
        || result == ACTION_INVALID_HUMANBUILD_OVERBORDER
        || result == ACTION_INVALID_HUMANBUILD_UNEXPLORE
        || result == ACTION_INVALID_HUMANBUILD_OVERLAP
        || result == ACTION_INVALID_POSITION_NOT_FIT
        || result == ACTION_INVALID_LOCATION;
}

enum ResourceRole {
    ROLE_FOOD = 0,
    ROLE_WOOD,
    ROLE_STONE,
    ROLE_GOLD,
    ROLE_NONE
};

enum TechnologyKind {
    TECH_TOWER_ENABLE = 0,
    TECH_BROADSWORD,
    TECH_TOWER_UPGRADE,
    TECH_WOODCUTTING,
    TECH_GOLD_MINING,
    TECH_FARMING
};

ResourceRole resourceRole(const tagResource& resource)
{
    switch (resource.Type) {
    case RESOURCE_BUSH:
    case RESOURCE_GAZELLE:
        return ROLE_FOOD;
    case RESOURCE_TREE:
        return ROLE_WOOD;
    case RESOURCE_STONE:
        return ROLE_STONE;
    case RESOURCE_GOLD:
        return ROLE_GOLD;
    default:
        return ROLE_NONE;
    }
}

const tagBuilding* findBuildingBySN(const tagInfo& info, int sn)
{
    for (const tagBuilding& building : info.buildings) {
        if (building.SN == sn)
            return &building;
    }
    return NULL;
}

const tagResource* findResourceBySN(const tagInfo& info, int sn)
{
    for (const tagResource& resource : info.resources) {
        if (resource.SN == sn)
            return &resource;
    }
    return NULL;
}

const tagFarmer* findFarmerBySN(const tagInfo& info, int sn)
{
    for (const tagFarmer& farmer : info.farmers) {
        if (farmer.SN == sn)
            return &farmer;
    }
    return NULL;
}

const tagBuilding* firstBuilding(const tagInfo& info, int type, bool finishedOnly)
{
    for (const tagBuilding& building : info.buildings) {
        if (building.Type == type && (!finishedOnly || building.Percent >= 100))
            return &building;
    }
    return NULL;
}

int countBuildings(const tagInfo& info, int type, bool finishedOnly)
{
    int result = 0;
    for (const tagBuilding& building : info.buildings) {
        if (building.Type == type && (!finishedOnly || building.Percent >= 100))
            ++result;
    }
    return result;
}

int countFarmers(const tagInfo& info)
{
    int result = 0;
    for (const tagFarmer& farmer : info.farmers) {
        if (farmer.FarmerSort == FARMERTYPE_FARMER)
            ++result;
    }
    return result;
}

int countArmy(const tagInfo& info, int sort)
{
    int result = 0;
    for (const tagArmy& army : info.armies) {
        if (army.Sort == sort)
            ++result;
    }
    return result;
}

bool buildingIdle(const tagBuilding* building)
{
    return building != NULL && building->Percent >= 100
        && (building->Project == ACT_NULL || building->Project == -1);
}

} // namespace

struct UsrAIStrategy::Impl
{
    enum WorkerTaskKind {
        WORKER_TASK_NONE = 0,
        WORKER_TASK_BUILD,
        WORKER_TASK_REPAIR,
        WORKER_TASK_DEPOSIT,
        WORKER_TASK_HUNT,
        WORKER_TASK_GATHER,
        WORKER_TASK_FARM,
        WORKER_TASK_MOVE
    };

    enum WorkerCommandKind {
        WORKER_COMMAND_ACTION = 0,
        WORKER_COMMAND_MOVE,
        WORKER_COMMAND_BUILD
    };

    enum BlackboardTopic {
        TOPIC_CONSTRUCTION = 1,
        TOPIC_HUNT = 2,
        TOPIC_FARM = 4,
        TOPIC_ECONOMY = 8,
        TOPIC_DELIVERY = 16
    };

    struct WorkerIntent {
        int workerSN;
        int taskKind;
        int commandKind;
        int targetSN;
        int buildType;
        int x;
        int y;
        int priority;
        int topic;
    };

    struct WorkerLease {
        WorkerIntent intent;
        int issuedFrame;
        int lastProgressFrame;
        int lastX;
        int lastY;
        int retryCount;
        int state; // 0:assigned, 1:acknowledged, 2:running, 3:blocked

        WorkerLease()
            : issuedFrame(-1000), lastProgressFrame(-1000),
              lastX(-1), lastY(-1), retryCount(0), state(0)
        {
            intent.workerSN = -1;
            intent.taskKind = WORKER_TASK_NONE;
            intent.commandKind = WORKER_COMMAND_ACTION;
            intent.targetSN = -1;
            intent.buildType = -1;
            intent.x = -1;
            intent.y = -1;
            intent.priority = 0;
            intent.topic = 0;
        }
    };

    struct WorkerBlackboard {
        const tagInfo* snapshot;
        int frame;
        int publishedTopics;
        bool berriesDepleted;
        bool herdKnown;
        bool herdDead;
        bool goldKnown;
        vector<WorkerIntent> intents;

        WorkerBlackboard()
            : snapshot(NULL), frame(-1), publishedTopics(0),
              berriesDepleted(false), herdKnown(false), herdDead(false),
              goldKnown(false)
        {
        }

        void begin(const tagInfo& gameInfo, bool berriesAreDepleted,
                   bool herdIsKnown, bool herdIsDead, bool goldIsKnown)
        {
            snapshot = &gameInfo;
            frame = gameInfo.GameFrame;
            publishedTopics = TOPIC_CONSTRUCTION | TOPIC_HUNT
                | TOPIC_FARM | TOPIC_ECONOMY | TOPIC_DELIVERY;
            berriesDepleted = berriesAreDepleted;
            herdKnown = herdIsKnown;
            herdDead = herdIsDead;
            goldKnown = goldIsKnown;
            intents.clear();
        }
    };

    struct BuildAttempt {
        int id;
        int type;
        int x;
        int y;
        int frame;
    };

    struct TrackedTechnology {
        int action;
        int state; // 0:not requested, 1:waiting result, 2:running, 3:complete
        int buildingSN;
        int acceptedFrame;
        bool sawBusy;

        explicit TrackedTechnology(int actionNumber = -1)
            : action(actionNumber), state(0), buildingSN(-1),
              acceptedFrame(-1), sawBusy(false)
        {
        }
    };

    int lastFrame;
    int lastBuildFrame;
    int lastCenterOrderFrame;
    int lastMilitaryOrderFrame;
    int lastTelemetryFrame;
    map<int, BuildAttempt> buildAttempts;
    map<int, pair<int, int> > technologyAttempts;
    map<int, set<int> > rejectedSites;
    map<int, int> lastUnitOrderFrame;
    map<int, int> priestWaypointIndex;
    map<int, int> lastArmyBlood;
    map<int, WorkerLease> workerLeases;
    WorkerBlackboard workerBlackboard;
    set<int> formerBerryWorkers;
    set<int> huntWorkers;
    set<int> herdGazelles;
    set<int> priestsInDanger;
    set<int> priestsUnderDirectThreat;
    bool firstWaveEngaged;
    bool firstWaveCleared;
    bool sawHomeBerries;
    bool homeBerriesDepleted;
    bool huntAnchorValid;
    bool firstGazelleKilled;
    bool herdKilled;
    bool huntFinished;
    bool goldAnchorValid;
    int huntAnchorX;
    int huntAnchorY;
    int goldAnchorX;
    int goldAnchorY;
    int currentGazelleSN;
    int armyScoutSN;
    int armyScoutWaypoint;
    TrackedTechnology towerResearch;
    TrackedTechnology broadswordResearch;
    TrackedTechnology towerUpgrade;
    TrackedTechnology woodcuttingResearch;
    TrackedTechnology goldMiningResearch;
    TrackedTechnology farmingResearch;

    Impl()
        : lastFrame(-1), lastBuildFrame(-1000), lastCenterOrderFrame(-1000),
          lastMilitaryOrderFrame(-1000),
          lastTelemetryFrame(-1000),
          firstWaveEngaged(false), firstWaveCleared(false),
          sawHomeBerries(false), homeBerriesDepleted(false),
          huntAnchorValid(false), firstGazelleKilled(false), herdKilled(false),
          huntFinished(false), goldAnchorValid(false),
          huntAnchorX(-1), huntAnchorY(-1),
          goldAnchorX(-1), goldAnchorY(-1), currentGazelleSN(-1),
          armyScoutSN(-1), armyScoutWaypoint(0),
          towerResearch(BUILDING_GRANARY_ARROWTOWER),
          broadswordResearch(BUILDING_ARMYCAMP_UPGRADE_BROADSWORD),
          towerUpgrade(BUILDING_GRANARY_ARROWTOWE_UPGRADE),
          woodcuttingResearch(BUILDING_MARKET_WOOD_UPGRADE),
          goldMiningResearch(BUILDING_MARKET_GOLD_UPGRADE),
          farmingResearch(BUILDING_MARKET_FARM_UPGRADE)
    {
    }

    void reset()
    {
        *this = Impl();
    }

    bool sameWorkerIntent(const WorkerIntent& a, const WorkerIntent& b) const
    {
        return a.workerSN == b.workerSN
            && a.taskKind == b.taskKind
            && a.commandKind == b.commandKind
            && a.targetSN == b.targetSN
            && a.buildType == b.buildType
            && a.x == b.x && a.y == b.y;
    }

    void publishWorkerAction(int workerSN, int targetSN, int taskKind,
                             int priority, int topic)
    {
        WorkerIntent intent = { workerSN, taskKind, WORKER_COMMAND_ACTION,
                                targetSN, -1, -1, -1, priority, topic };
        workerBlackboard.intents.push_back(intent);
    }

    void publishWorkerMove(int workerSN, int x, int y, int priority, int topic)
    {
        WorkerIntent intent = { workerSN, WORKER_TASK_MOVE, WORKER_COMMAND_MOVE,
                                -1, -1, x, y, priority, topic };
        workerBlackboard.intents.push_back(intent);
    }

    void publishWorkerBuild(int workerSN, int type, int x, int y,
                            int priority)
    {
        WorkerIntent intent = { workerSN, WORKER_TASK_BUILD,
                                WORKER_COMMAND_BUILD, -1, type,
                                x, y, priority, TOPIC_CONSTRUCTION };
        workerBlackboard.intents.push_back(intent);
    }

    void beginWorkerBlackboard(const tagInfo& info)
    {
        workerBlackboard.begin(info, homeBerriesDepleted, huntAnchorValid,
                               herdKilled, goldAnchorValid);
        set<int> alive;
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort == FARMERTYPE_FARMER)
                alive.insert(farmer.SN);
        }
        for (map<int, WorkerLease>::iterator it = workerLeases.begin();
             it != workerLeases.end();) {
            if (alive.count(it->first) == 0)
                it = workerLeases.erase(it);
            else
                ++it;
        }
    }

    void dispatchWorkerBlackboard(UsrAI& ai, const tagInfo& info)
    {
        map<int, WorkerIntent> selected;
        for (const WorkerIntent& intent : workerBlackboard.intents) {
            map<int, WorkerIntent>::iterator current = selected.find(intent.workerSN);
            if (current == selected.end()
                || intent.priority > current->second.priority)
                selected[intent.workerSN] = intent;
        }

        for (map<int, WorkerIntent>::const_iterator it = selected.begin();
             it != selected.end(); ++it) {
            const WorkerIntent& intent = it->second;
            const tagFarmer* worker = findFarmerBySN(info, intent.workerSN);
            if (worker == NULL || worker->FarmerSort != FARMERTYPE_FARMER)
                continue;

            WorkerLease& lease = workerLeases[intent.workerSN];
            const bool changed = !sameWorkerIntent(lease.intent, intent);
            if (changed) {
                lease.intent = intent;
                lease.issuedFrame = -1000;
                lease.lastProgressFrame = info.GameFrame;
                lease.lastX = worker->BlockDR;
                lease.lastY = worker->BlockUR;
                lease.retryCount = 0;
                lease.state = 0;
            }
            else if (worker->BlockDR != lease.lastX
                     || worker->BlockUR != lease.lastY) {
                lease.lastProgressFrame = info.GameFrame;
                lease.lastX = worker->BlockDR;
                lease.lastY = worker->BlockUR;
                lease.retryCount = 0;
                if (lease.state == 3)
                    lease.state = 1;
            }

            const bool targetAcknowledged = intent.commandKind == WORKER_COMMAND_ACTION
                && worker->WorkObjectSN == intent.targetSN;
            const bool running = targetAcknowledged
                && worker->NowState != HUMAN_STATE_IDLE;
            const bool productive = targetAcknowledged
                && (worker->NowState == HUMAN_STATE_WORKING
                    || worker->NowState == HUMAN_STATE_ATTACKING);
            const bool moveComplete = intent.commandKind == WORKER_COMMAND_MOVE
                && distance2(worker->BlockDR, worker->BlockUR,
                             intent.x, intent.y) <= 4;
            if (productive) {
                lease.lastProgressFrame = info.GameFrame;
                lease.retryCount = 0;
            }
            if (running)
                lease.state = 2;
            else if (targetAcknowledged || moveComplete)
                lease.state = 1;

            const int sinceIssue = info.GameFrame - lease.issuedFrame;
            const bool unexpectedIdle = intent.commandKind == WORKER_COMMAND_ACTION
                && worker->NowState == HUMAN_STATE_IDLE && sinceIssue > 30;
            const bool stalled = !moveComplete && !productive
                && info.GameFrame - lease.lastProgressFrame > 120
                && sinceIssue > 30;
            const bool shouldIssue = changed
                || (intent.commandKind != WORKER_COMMAND_BUILD
                    && (unexpectedIdle || stalled));
            if (!shouldIssue)
                continue;

            if (intent.commandKind == WORKER_COMMAND_ACTION)
                ai.HumanAction(intent.workerSN, intent.targetSN);
            else if (intent.commandKind == WORKER_COMMAND_MOVE)
                ai.HumanMove(intent.workerSN,
                             (intent.x + 0.5) * double(BLOCKSIDELENGTH),
                             (intent.y + 0.5) * double(BLOCKSIDELENGTH));
            else {
                const int id = ai.HumanBuild(intent.workerSN, intent.buildType,
                                             intent.x, intent.y);
                BuildAttempt attempt = { id, intent.buildType, intent.x,
                                         intent.y, info.GameFrame };
                buildAttempts[id] = attempt;
                lastBuildFrame = info.GameFrame;
            }
            if (stalled)
                ++lease.retryCount;
            lease.issuedFrame = info.GameFrame;
            lease.lastX = worker->BlockDR;
            lease.lastY = worker->BlockUR;
            lease.state = stalled && lease.retryCount >= 2 ? 3 : 0;
        }

        for (map<int, WorkerLease>::iterator it = workerLeases.begin();
             it != workerLeases.end();) {
            if (selected.count(it->first) == 0 && it->second.state != 3)
                it = workerLeases.erase(it);
            else
                ++it;
        }
    }

    bool workerLeaseBlocked(int workerSN, int targetSN) const
    {
        map<int, WorkerLease>::const_iterator lease = workerLeases.find(workerSN);
        return lease != workerLeases.end()
            && lease->second.intent.targetSN == targetSN
            && lease->second.state == 3;
    }

    TrackedTechnology& technology(int kind)
    {
        if (kind == TECH_TOWER_ENABLE)
            return towerResearch;
        if (kind == TECH_BROADSWORD)
            return broadswordResearch;
        if (kind == TECH_TOWER_UPGRADE)
            return towerUpgrade;
        if (kind == TECH_WOODCUTTING)
            return woodcuttingResearch;
        if (kind == TECH_GOLD_MINING)
            return goldMiningResearch;
        return farmingResearch;
    }

    void consumeResults(const tagInfo& info)
    {
        for (map<int, BuildAttempt>::iterator it = buildAttempts.begin();
             it != buildAttempts.end();) {
            map<int, int>::const_iterator result = info.ins_ret.find(it->first);
            if (result == info.ins_ret.end()) {
                if (info.GameFrame - it->second.frame > 100)
                    it = buildAttempts.erase(it);
                else
                    ++it;
                continue;
            }
            if (isPlacementError(result->second))
                rejectedSites[it->second.type].insert(it->second.x * 1000 + it->second.y);
            it = buildAttempts.erase(it);
        }

        for (map<int, pair<int, int> >::iterator it = technologyAttempts.begin();
             it != technologyAttempts.end();) {
            map<int, int>::const_iterator result = info.ins_ret.find(it->first);
            if (result == info.ins_ret.end()) {
                if (info.GameFrame - it->second.second > 100) {
                    technology(it->second.first).state = 0;
                    it = technologyAttempts.erase(it);
                }
                else
                    ++it;
                continue;
            }

            TrackedTechnology& tech = technology(it->second.first);
            if (result->second == ACTION_SUCCESS) {
                tech.state = 2;
                tech.acceptedFrame = info.GameFrame;
                tech.sawBusy = false;
            }
            else if (result->second == ACTION_INVALID_BUILDACT_LOCK)
                tech.state = 3;
            else
                tech.state = 0;
            it = technologyAttempts.erase(it);
        }
    }

    void updateTechnologyState(const tagInfo& info, TrackedTechnology& tech)
    {
        if (tech.state != 2)
            return;
        const tagBuilding* building = findBuildingBySN(info, tech.buildingSN);
        if (building == NULL) {
            tech.state = 0;
            return;
        }
        if (building->Project == tech.action)
            tech.sawBusy = true;
        if (tech.sawBusy
            && (building->Project == ACT_NULL || building->Project == -1))
            tech.state = 3;
    }

    bool requestTechnology(UsrAI& ai, const tagInfo& info, int kind,
                           const tagBuilding* building)
    {
        TrackedTechnology& tech = technology(kind);
        if (tech.state != 0 || !buildingIdle(building))
            return false;
        const int id = ai.BuildingAction(building->SN, tech.action);
        tech.state = 1;
        tech.buildingSN = building->SN;
        technologyAttempts[id] = make_pair(kind, info.GameFrame);
        return true;
    }

    bool buildAffordable(const tagInfo& info, int type) const
    {
        switch (type) {
        case BUILDING_HOME:       return info.Wood >= BUILD_HOUSE_WOOD;
        case BUILDING_GRANARY:    return info.Wood >= BUILD_GRANARY_WOOD;
        case BUILDING_STOCK:      return info.Wood >= BUILD_STOCK_WOOD;
        case BUILDING_ARMYCAMP:   return info.Wood >= BUILD_ARMYCAMP_WOOD;
        case BUILDING_MARKET:     return info.Wood >= BUILD_MARKET_WOOD;
        case BUILDING_RANGE:      return info.Wood >= BUILD_RANGE_WOOD;
        case BUILDING_STABLE:     return info.Wood >= BUILD_STABLE_WOOD;
        case BUILDING_ARROWTOWER: return info.Stone >= BUILD_ARROWTOWER_STONE;
        case BUILDING_FARM:       return info.Wood >= BUILD_FARM_WOOD;
        default:                  return false;
        }
    }

    bool buildUnlocked(const tagInfo& info, int type) const
    {
        if (type == BUILDING_HOME || type == BUILDING_GRANARY
            || type == BUILDING_STOCK || type == BUILDING_ARMYCAMP)
            return info.civilizationStage >= CIVILIZATION_STONEAGE;
        if (type == BUILDING_MARKET)
            return info.civilizationStage >= CIVILIZATION_TOOLAGE
                && firstBuilding(info, BUILDING_GRANARY, true) != NULL;
        if (type == BUILDING_RANGE || type == BUILDING_STABLE)
            return info.civilizationStage >= CIVILIZATION_TOOLAGE
                && firstBuilding(info, BUILDING_ARMYCAMP, true) != NULL;
        if (type == BUILDING_ARROWTOWER)
            return info.civilizationStage >= CIVILIZATION_TOOLAGE
                && towerResearch.state == 3;
        if (type == BUILDING_FARM)
            return info.civilizationStage >= CIVILIZATION_TOOLAGE
                && firstBuilding(info, BUILDING_MARKET, true) != NULL;
        return false;
    }

    pair<int, int> centerBlock(const tagInfo& info) const
    {
        const tagBuilding* center = firstBuilding(info, BUILDING_CENTER, false);
        if (center != NULL)
            return make_pair(center->BlockDR, center->BlockUR);
        if (!info.farmers.empty())
            return make_pair(info.farmers[0].BlockDR, info.farmers[0].BlockUR);
        return make_pair(MAP_L / 2, MAP_U / 2);
    }

    pair<int, int> resourceAnchor(const tagInfo& info, int type,
                                  const pair<int, int>& center) const
    {
        int bestDistance = numeric_limits<int>::max();
        pair<int, int> result = center;
        for (const tagResource& resource : info.resources) {
            bool matches = false;
            if (type == BUILDING_GRANARY)
                matches = resource.Type == RESOURCE_BUSH;
            else if (type == BUILDING_STOCK)
                matches = resource.Type == RESOURCE_TREE
                       || resource.Type == RESOURCE_STONE
                       || resource.Type == RESOURCE_GOLD;
            if (!matches)
                continue;
            const int d = distance2(center.first, center.second,
                                    resource.BlockDR, resource.BlockUR);
            if (d < bestDistance) {
                bestDistance = d;
                result = make_pair(resource.BlockDR, resource.BlockUR);
            }
        }
        return result;
    }

    int manhattanDistance(int x1, int y1, int x2, int y2) const
    {
        return abs(x1 - x2) + abs(y1 - y2);
    }

    int specializedDropoffType(const tagResource& resource) const
    {
        if (resource.Type == RESOURCE_BUSH)
            return BUILDING_GRANARY;
        if (resource.Type == RESOURCE_TREE || resource.Type == RESOURCE_STONE
            || resource.Type == RESOURCE_GOLD || resource.Type == RESOURCE_GAZELLE)
            return BUILDING_STOCK;
        return -1;
    }

    bool nearPreferredDropoff(const tagInfo& info, const tagResource& resource,
                              int* distanceToDropoff = NULL) const
    {
        const int dropoffType = specializedDropoffType(resource);
        if (dropoffType < 0)
            return false;

        int specializedDistance = numeric_limits<int>::max();
        for (const tagBuilding& building : info.buildings) {
            if (building.Type != dropoffType || building.Percent < 100)
                continue;
            specializedDistance = min(specializedDistance,
                                      manhattanDistance(resource.BlockDR,
                                                        resource.BlockUR,
                                                        building.BlockDR,
                                                        building.BlockUR));
        }
        if (specializedDistance == numeric_limits<int>::max())
            return false;

        int centerDistance = numeric_limits<int>::max();
        const tagBuilding* center = firstBuilding(info, BUILDING_CENTER, true);
        if (center != NULL) {
            centerDistance = manhattanDistance(resource.BlockDR, resource.BlockUR,
                                               center->BlockDR, center->BlockUR);
        }
        if (distanceToDropoff != NULL)
            *distanceToDropoff = specializedDistance;

        // This mirrors the kernel's nearest-compatible-dropoff decision.  A
        // worker assigned here will use the stock/granary instead of the center.
        return specializedDistance <= kDropoffResourceRadius
            && specializedDistance < centerDistance;
    }

    bool hasPreferredResource(const tagInfo& info, ResourceRole role) const
    {
        for (const tagResource& resource : info.resources) {
            if (resource.Cnt > 0 && resourceRole(resource) == role
                && nearPreferredDropoff(info, resource))
                return true;
        }
        return false;
    }

    pair<int, int> frontLineAnchor(const tagInfo& info) const
    {
        const pair<int, int> center = centerBlock(info);
        int targetX = center.first;
        int targetY = center.second;
        int bestEnemyDistance = numeric_limits<int>::max();

        // A visible attacker is the strongest evidence of the actual front.
        // Enemy buildings are a stable fallback after the map has been scouted.
        for (const tagArmy& enemy : info.enemy_armies) {
            const int d = distance2(center.first, center.second,
                                    enemy.BlockDR, enemy.BlockUR);
            if (d < bestEnemyDistance) {
                bestEnemyDistance = d;
                targetX = enemy.BlockDR;
                targetY = enemy.BlockUR;
            }
        }
        for (const tagBuilding& enemy : info.enemy_buildings) {
            const int d = distance2(center.first, center.second,
                                    enemy.BlockDR, enemy.BlockUR);
            if (d < bestEnemyDistance) {
                bestEnemyDistance = d;
                targetX = enemy.BlockDR;
                targetY = enemy.BlockUR;
            }
        }

        // Before contact, the scenario's original tower marks the defended
        // approach.  This also makes the placement rotate with the map.
        if (bestEnemyDistance == numeric_limits<int>::max()) {
            int bestTowerDistance = 0;
            for (const tagBuilding& building : info.buildings) {
                if (building.Type != BUILDING_ARROWTOWER
                    || building.Percent < 100)
                    continue;
                const int d = distance2(center.first, center.second,
                                        building.BlockDR, building.BlockUR);
                if (d > bestTowerDistance) {
                    bestTowerDistance = d;
                    targetX = building.BlockDR;
                    targetY = building.BlockUR;
                }
            }
        }

        if (targetX == center.first && targetY == center.second) {
            targetX = MAP_L / 2;
            targetY = MAP_U / 2;
        }
        if (targetX == center.first && targetY == center.second)
            targetX = center.first + 1;

        const double dx = double(targetX - center.first);
        const double dy = double(targetY - center.second);
        const double length = sqrt(dx * dx + dy * dy);
        const int forwardDistance = 11;
        const int lateralDistance = 5;
        int anchorX = static_cast<int>(center.first
            + dx * forwardDistance / length - dy * lateralDistance / length);
        int anchorY = static_cast<int>(center.second
            + dy * forwardDistance / length + dx * lateralDistance / length);
        const int side = buildingSide(BUILDING_ARROWTOWER);
        anchorX = max(0, min(MAP_L - side, anchorX));
        anchorY = max(0, min(MAP_U - side, anchorY));
        return make_pair(anchorX, anchorY);
    }

    pair<int, int> preferredAnchor(const tagInfo& info, int type) const
    {
        const pair<int, int> center = centerBlock(info);
        if (type == BUILDING_STOCK && huntAnchorValid) {
            // Put the hunting stock on the base-facing side of the herd.  A
            // small gap around the carcasses prevents gatherers and returning
            // villagers from trying to cross through the same crowded tiles.
            const int dx = center.first - huntAnchorX;
            const int dy = center.second - huntAnchorY;
            const int stepX = dx > 0 ? 1 : (dx < 0 ? -1 : 0);
            const int stepY = dy > 0 ? 1 : (dy < 0 ? -1 : 0);
            if (abs(dx) >= abs(dy))
                return make_pair(huntAnchorX + stepX * 7,
                                 huntAnchorY + stepY * 3);
            return make_pair(huntAnchorX + stepX * 3,
                             huntAnchorY + stepY * 7);
        }
        if (type == BUILDING_GRANARY || type == BUILDING_STOCK)
            return resourceAnchor(info, type, center);
        if (type == BUILDING_FARM) {
            const int stepX = MAP_L / 2 > center.first ? 8 : -8;
            const int stepY = MAP_U / 2 > center.second ? 8 : -8;
            return make_pair(center.first + stepX, center.second + stepY);
        }
        switch (type) {
        case BUILDING_HOME:       return make_pair(center.first + 7, center.second + 5);
        case BUILDING_ARMYCAMP:   return make_pair(center.first + 2, center.second + 6);
        case BUILDING_MARKET:     return make_pair(center.first + 7, center.second);
        case BUILDING_RANGE:      return make_pair(center.first - 4, center.second + 5);
        case BUILDING_STABLE:     return make_pair(center.first + 6, center.second - 4);
        case BUILDING_ARROWTOWER: return frontLineAnchor(info);
        default:                  return center;
        }
    }

    bool overlapsKnownObject(const tagInfo& info, int x, int y, int side) const
    {
        const int right = x + side;
        const int top = y + side;
        for (const tagBuilding& building : info.buildings) {
            const int otherSide = buildingSide(building.Type);
            if (x < building.BlockDR + otherSide + 1 && right + 1 > building.BlockDR
                && y < building.BlockUR + otherSide + 1 && top + 1 > building.BlockUR)
                return true;
        }
        for (const tagBuilding& building : info.enemy_buildings) {
            const int otherSide = buildingSide(building.Type);
            if (x < building.BlockDR + otherSide && right > building.BlockDR
                && y < building.BlockUR + otherSide && top > building.BlockUR)
                return true;
        }
        for (const tagResource& resource : info.resources) {
            if (resource.BlockDR >= x - 1 && resource.BlockDR < right + 1
                && resource.BlockUR >= y - 1 && resource.BlockUR < top + 1)
                return true;
        }
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.BlockDR >= x && farmer.BlockDR < right
                && farmer.BlockUR >= y && farmer.BlockUR < top)
                return true;
        }
        for (const tagArmy& army : info.armies) {
            if (army.BlockDR >= x && army.BlockDR < right
                && army.BlockUR >= y && army.BlockUR < top)
                return true;
        }
        return false;
    }

    bool validSite(const tagInfo& info, int type, int x, int y) const
    {
        const int side = buildingSide(type);
        if (x < 0 || y < 0 || x + side > MAP_L || y + side > MAP_U)
            return false;
        map<int, set<int> >::const_iterator rejected = rejectedSites.find(type);
        if (rejected != rejectedSites.end()
            && rejected->second.count(x * 1000 + y) != 0)
            return false;
        if (info.theMap == NULL)
            return false;
        const int height = (*info.theMap)[x][y].height;
        if (height < 0)
            return false;
        for (int dx = 0; dx < side; ++dx) {
            for (int dy = 0; dy < side; ++dy) {
                if ((*info.theMap)[x + dx][y + dy].height != height)
                    return false;
            }
        }
        return !overlapsKnownObject(info, x, y, side);
    }

    bool findBuildSite(const tagInfo& info, int type, int& x, int& y) const
    {
        if (type == BUILDING_ARROWTOWER) {
            const pair<int, int> center = centerBlock(info);
            const pair<int, int> anchor = frontLineAnchor(info);
            const int frontX = anchor.first - center.first;
            const int frontY = anchor.second - center.second;
            const int anchorProjection = frontX * frontX + frontY * frontY;

            // Search around the front-line anchor, but never let obstacle
            // avoidance wrap the result back behind the base.
            for (int radius = 0; radius <= 10; ++radius) {
                for (int dx = -radius; dx <= radius; ++dx) {
                    for (int dy = -radius; dy <= radius; ++dy) {
                        if (radius > 0 && abs(dx) != radius && abs(dy) != radius)
                            continue;
                        const int candidateX = anchor.first + dx;
                        const int candidateY = anchor.second + dy;
                        const int projection = (candidateX - center.first) * frontX
                            + (candidateY - center.second) * frontY;
                        if (projection * 3 < anchorProjection * 2)
                            continue;
                        if (validSite(info, type, candidateX, candidateY)) {
                            x = candidateX;
                            y = candidateY;
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        if (type == BUILDING_STOCK && huntAnchorValid) {
            const pair<int, int> center = centerBlock(info);
            const int deltaX = center.first - huntAnchorX;
            const int deltaY = center.second - huntAnchorY;
            const int step = (deltaX != 0 && deltaY != 0) ? 5 : 7;
            const int desiredX = huntAnchorX
                + (deltaX > 0 ? step : (deltaX < 0 ? -step : 0));
            const int desiredY = huntAnchorY
                + (deltaY > 0 ? step : (deltaY < 0 ? -step : 0));
            long bestScore = numeric_limits<long>::max();
            bool found = false;
            for (int candidateX = huntAnchorX - 10;
                 candidateX <= huntAnchorX + 10; ++candidateX) {
                for (int candidateY = huntAnchorY - 10;
                     candidateY <= huntAnchorY + 10; ++candidateY) {
                    const int herdDistance = distance2(candidateX, candidateY,
                                                       huntAnchorX, huntAnchorY);
                    if (herdDistance < 5 * 5 || herdDistance > 10 * 10
                        || !validSite(info, type, candidateX, candidateY))
                        continue;
                    int nearbyObstacles = 0;
                    for (const tagResource& resource : info.resources) {
                        if (distance2(candidateX + 1, candidateY + 1,
                                      resource.BlockDR, resource.BlockUR) <= 5 * 5)
                            ++nearbyObstacles;
                    }
                    const long score = 20L * distance2(candidateX, candidateY,
                                                        desiredX, desiredY)
                        + distance2(candidateX, candidateY,
                                    center.first, center.second)
                        + 200L * nearbyObstacles;
                    if (score < bestScore) {
                        bestScore = score;
                        x = candidateX;
                        y = candidateY;
                        found = true;
                    }
                }
            }
            return found;
        }

        const pair<int, int> anchor = preferredAnchor(info, type);
        for (int radius = 0; radius <= 15; ++radius) {
            for (int dx = -radius; dx <= radius; ++dx) {
                for (int dy = -radius; dy <= radius; ++dy) {
                    if (radius > 0 && abs(dx) != radius && abs(dy) != radius)
                        continue;
                    const int candidateX = anchor.first + dx;
                    const int candidateY = anchor.second + dy;
                    if (validSite(info, type, candidateX, candidateY)) {
                        x = candidateX;
                        y = candidateY;
                        return true;
                    }
                }
            }
        }
        return false;
    }

    bool workerIsConstructing(const tagInfo& info, const tagFarmer& farmer) const
    {
        const tagBuilding* target = findBuildingBySN(info, farmer.WorkObjectSN);
        return target != NULL && target->Percent < 100;
    }

    bool buildingNeedsWorkers(const tagBuilding& building) const
    {
        return building.Percent < 100
            || (building.Percent >= 100 && building.MaxBlood > 0
                && building.Blood < building.MaxBlood);
    }

    int activeBuilderCount(const tagInfo& info) const
    {
        int result = 0;
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER)
                continue;
            const tagBuilding* target = findBuildingBySN(info, farmer.WorkObjectSN);
            if (target != NULL && buildingNeedsWorkers(*target)
                && !workerLeaseBlocked(farmer.SN, target->SN))
                ++result;
        }
        return result;
    }

    const tagFarmer* chooseBuilder(const tagInfo& info, int x, int y,
                                   const set<int>& commanded) const
    {
        const tagFarmer* best = NULL;
        long bestScore = numeric_limits<long>::max();
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || commanded.count(farmer.SN) != 0
                || workerIsConstructing(info, farmer))
                continue;
            long score = distance2(farmer.BlockDR, farmer.BlockUR, x, y);
            if (farmer.NowState == HUMAN_STATE_IDLE)
                score -= 100000;
            const tagResource* resource = findResourceBySN(info, farmer.WorkObjectSN);
            if (resource != NULL && resource->Type == RESOURCE_BUSH)
                score += 50000;
            if (score < bestScore) {
                bestScore = score;
                best = &farmer;
            }
        }
        return best;
    }

    bool repairOrResume(const tagInfo& info, set<int>& commanded)
    {
        map<int, int> workersPerBuilding;
        int totalBuilders = 0;
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER)
                continue;
            const tagBuilding* target = findBuildingBySN(info, farmer.WorkObjectSN);
            if (target != NULL && buildingNeedsWorkers(*target)
                && !workerLeaseBlocked(farmer.SN, target->SN)) {
                ++workersPerBuilding[target->SN];
                ++totalBuilders;
                publishWorkerAction(farmer.SN, target->SN,
                                    target->Percent < 100
                                        ? WORKER_TASK_BUILD : WORKER_TASK_REPAIR,
                                    900, TOPIC_CONSTRUCTION);
                commanded.insert(farmer.SN);
            }
        }

        bool issuedOrder = false;
        while (totalBuilders < kOpeningBuilderCount) {
            const tagBuilding* target = NULL;
            int fewestWorkers = numeric_limits<int>::max();
            for (const tagBuilding& building : info.buildings) {
                if (!buildingNeedsWorkers(building))
                    continue;
                const int assigned = workersPerBuilding[building.SN];
                if (assigned < fewestWorkers) {
                    fewestWorkers = assigned;
                    target = &building;
                }
            }
            if (target == NULL)
                break;

            const tagFarmer* worker = chooseBuilder(info, target->BlockDR,
                                                    target->BlockUR, commanded);
            if (worker == NULL)
                break;
            publishWorkerAction(worker->SN, target->SN,
                                target->Percent < 100
                                    ? WORKER_TASK_BUILD : WORKER_TASK_REPAIR,
                                900, TOPIC_CONSTRUCTION);
            commanded.insert(worker->SN);
            ++workersPerBuilding[target->SN];
            ++totalBuilders;
            issuedOrder = true;
        }
        return issuedOrder;
    }

    int desiredHomeCount(const tagInfo& info) const
    {
        if (info.civilizationStage <= CIVILIZATION_STONEAGE)
            return 4;
        if (info.civilizationStage == CIVILIZATION_TOOLAGE)
            return 6;
        return kLateHomeCount;
    }

    int desiredFarmerCount(const tagInfo& info) const
    {
        if (info.civilizationStage <= CIVILIZATION_STONEAGE)
            return 13;
        if (info.civilizationStage == CIVILIZATION_TOOLAGE)
            return 14;
        return 40;
    }

    bool hasStockNearHerd(const tagInfo& info, bool finishedOnly) const
    {
        if (!huntAnchorValid)
            return false;
        for (const tagBuilding& building : info.buildings) {
            if (building.Type == BUILDING_STOCK
                && (!finishedOnly || building.Percent >= 100)
                && distance2(building.BlockDR, building.BlockUR,
                             huntAnchorX, huntAnchorY) <= 10 * 10)
                return true;
        }
        return false;
    }

    bool hasUnfinishedBuilding(const tagInfo& info, int type) const
    {
        for (const tagBuilding& building : info.buildings) {
            if (building.Type == type && building.Percent < 100)
                return true;
        }
        return false;
    }

    vector<int> desiredBuildings(const tagInfo& info) const
    {
        vector<int> desired;
        const bool needsHerdStock = huntAnchorValid
            && !hasStockNearHerd(info, false);
        if (needsHerdStock)
            desired.push_back(BUILDING_STOCK);
        const int currentHomes = countBuildings(info, BUILDING_HOME, false);
        const int targetHomes = desiredHomeCount(info);
        const double populationHeadroom = info.Human_MaxNum - info.Human_Num;
        bool homeUnderConstruction = false;
        for (const tagBuilding& building : info.buildings) {
            if (building.Type == BUILDING_HOME && building.Percent < 100) {
                homeUnderConstruction = true;
                break;
            }
        }
        const bool needsHome = currentHomes < targetHomes
            && !homeUnderConstruction
            && populationHeadroom <= 6.0;
        if (needsHome)
            desired.push_back(BUILDING_HOME);
        if (firstBuilding(info, BUILDING_GRANARY, false) == NULL)
            desired.push_back(BUILDING_GRANARY);
        if (!needsHerdStock
            && firstBuilding(info, BUILDING_STOCK, false) == NULL)
            desired.push_back(BUILDING_STOCK);
        if (firstBuilding(info, BUILDING_ARMYCAMP, false) == NULL)
            desired.push_back(BUILDING_ARMYCAMP);
        if (info.civilizationStage >= CIVILIZATION_TOOLAGE) {
            if (towerResearch.state == 3
                && countBuildings(info, BUILDING_ARROWTOWER, false) < 2)
                desired.push_back(BUILDING_ARROWTOWER);
            if (firstBuilding(info, BUILDING_MARKET, false) == NULL)
                desired.push_back(BUILDING_MARKET);
            if (firstBuilding(info, BUILDING_RANGE, false) == NULL)
                desired.push_back(BUILDING_RANGE);
            if (huntFinished
                && countBuildings(info, BUILDING_FARM, false) < kFarmTargetCount
                && !hasUnfinishedBuilding(info, BUILDING_FARM))
                desired.push_back(BUILDING_FARM);
        }
        return desired;
    }

    void constructBuildings(const tagInfo& info, set<int>& commanded)
    {
        if (!buildAttempts.empty() || info.GameFrame - lastBuildFrame < 8)
            return;
        if (activeBuilderCount(info) >= kOpeningBuilderCount)
            return;
        const vector<int> desired = desiredBuildings(info);
        for (int type : desired) {
            if (!buildUnlocked(info, type) || !buildAffordable(info, type))
                continue;
            int x = -1;
            int y = -1;
            if (!findBuildSite(info, type, x, y))
                continue;
            const tagFarmer* builder = chooseBuilder(info, x, y, commanded);
            if (builder == NULL)
                return;
            publishWorkerBuild(builder->SN, type, x, y, 900);
            commanded.insert(builder->SN);
            return;
        }
    }

    bool bronzePrerequisitesReady(const tagInfo& info) const
    {
        int count = 0;
        count += firstBuilding(info, BUILDING_MARKET, true) != NULL;
        count += firstBuilding(info, BUILDING_RANGE, true) != NULL;
        count += firstBuilding(info, BUILDING_STABLE, true) != NULL;
        return count >= 2;
    }

    int bronzeArmyFoodReserve(const tagInfo& info) const
    {
        const int missingUnits = max(0, 3 - countArmy(info, AT_BROADSWORDSMAN));
        int reserve = missingUnits * BUILDING_ARMYCAMP_CREATE_BROADSWORD_FOOD;
        if (broadswordResearch.state == 0)
            reserve += BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_FOOD;
        return reserve;
    }

    bool toolPrerequisitesReady(const tagInfo& info) const
    {
        int count = 0;
        count += firstBuilding(info, BUILDING_GRANARY, true) != NULL;
        count += firstBuilding(info, BUILDING_STOCK, true) != NULL;
        count += firstBuilding(info, BUILDING_ARMYCAMP, true) != NULL;
        return count >= 2;
    }

    void manageTechnologies(UsrAI& ai, const tagInfo& info)
    {
        updateTechnologyState(info, towerResearch);
        updateTechnologyState(info, broadswordResearch);
        updateTechnologyState(info, towerUpgrade);
        updateTechnologyState(info, woodcuttingResearch);
        updateTechnologyState(info, goldMiningResearch);
        updateTechnologyState(info, farmingResearch);
        const tagBuilding* granary = firstBuilding(info, BUILDING_GRANARY, true);
        if (info.civilizationStage >= CIVILIZATION_TOOLAGE
            && towerResearch.state == 0
            && info.Meat >= BUILDING_GRANARY_ARROWTOWER_FOOD) {
            requestTechnology(ai, info, TECH_TOWER_ENABLE, granary);
            return;
        }

        const tagBuilding* camp = firstBuilding(info, BUILDING_ARMYCAMP, true);
        if (info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && broadswordResearch.state == 0
            && info.Meat >= BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_FOOD
            && info.Gold >= BUILDING_ARMYCAMP_UPGRADE_BROADSWORD_GOLD) {
            requestTechnology(ai, info, TECH_BROADSWORD, camp);
            return;
        }

        // Optional research is forbidden before reaching Bronze Age.  Once
        // there, preserve the complete broadsword research/unit food budget so
        // economic upgrades cannot starve the primary army plan.
        if (info.civilizationStage < CIVILIZATION_BRONZEAGE
            || !info.enemy_armies.empty() || !info.enemy_farmers.empty())
            return;

        const int armyFoodReserve = bronzeArmyFoodReserve(info);
        const tagBuilding* market = firstBuilding(info, BUILDING_MARKET, true);
        if (goldAnchorValid && info.Gold < 95 && goldMiningResearch.state == 0
            && info.Meat >= armyFoodReserve
                + BUILDING_MARKET_GOLD_UPGRADE_FOOD + 50
            && info.Wood >= BUILDING_MARKET_GOLD_UPGRADE_WOOD + 30
            && requestTechnology(ai, info, TECH_GOLD_MINING, market))
            return;

        if (towerResearch.state == 3 && towerUpgrade.state == 0
            && info.Meat >= armyFoodReserve
                + BUILDING_GRANARY_UPGRADE_ARROWTOWER_FOOD
            && info.Stone >= BUILDING_GRANARY_UPGRADE_ARROWTOWER_STONE
            && requestTechnology(ai, info, TECH_TOWER_UPGRADE, granary))
            return;

        if (woodcuttingResearch.state == 0
            && info.Meat >= armyFoodReserve
                + BUILDING_MARKET_WOOD_UPGRADE_FOOD + 80
            && info.Wood >= BUILDING_MARKET_WOOD_UPGRADE_WOOD + 90
            && requestTechnology(ai, info, TECH_WOODCUTTING, market))
            return;

        if (huntFinished && countBuildings(info, BUILDING_FARM, true) > 0
            && farmingResearch.state == 0
            && info.Meat >= armyFoodReserve
                + BUILDING_MARKET_FARM_UPGRADE_FOOD + 100
            && info.Wood >= BUILDING_MARKET_FARM_UPGRADE_WOOD + 75)
            requestTechnology(ai, info, TECH_FARMING, market);
    }

    void manageCenter(UsrAI& ai, const tagInfo& info)
    {
        const tagBuilding* center = firstBuilding(info, BUILDING_CENTER, true);
        if (!buildingIdle(center) || info.GameFrame - lastCenterOrderFrame < 10)
            return;
        bool shouldUpgrade = false;
        if (info.civilizationStage == CIVILIZATION_STONEAGE)
            shouldUpgrade = toolPrerequisitesReady(info) && info.Meat >= 500;
        else if (info.civilizationStage == CIVILIZATION_TOOLAGE)
            shouldUpgrade = bronzePrerequisitesReady(info) && info.Meat >= 800;
        if (shouldUpgrade) {
            ai.BuildingAction(center->SN, BUILDING_CENTER_UPGRADE);
            lastCenterOrderFrame = info.GameFrame;
            return;
        }

        const int farmers = countFarmers(info);
        const int targetFarmers = desiredFarmerCount(info);
        const bool reserveBronzeFood = info.civilizationStage == CIVILIZATION_TOOLAGE
            && bronzePrerequisitesReady(info);
        const bool hasPopulation = info.Human_Num + 1.0 <= info.Human_MaxNum;
        const int foodFloor = info.civilizationStage >= CIVILIZATION_BRONZEAGE ? 100 : 50;
        if (farmers < targetFarmers && hasPopulation && info.Meat >= foodFloor
            && (!reserveBronzeFood || info.Meat >= 850)) {
            ai.BuildingAction(center->SN, BUILDING_CENTER_CREATEFARMER);
            lastCenterOrderFrame = info.GameFrame;
        }
    }

    void manageArmyProduction(UsrAI& ai, const tagInfo& info)
    {
        if (info.Human_Num + 1.0 > info.Human_MaxNum)
            return;
        if (info.GameFrame - lastMilitaryOrderFrame < 12)
            return;
        const tagBuilding* camp = firstBuilding(info, BUILDING_ARMYCAMP, true);
        const tagBuilding* range = firstBuilding(info, BUILDING_RANGE, true);
        if (info.civilizationStage == CIVILIZATION_TOOLAGE) {
            // The second bowman is the dedicated deer-finding scout.  The
            // first bowman and the slinger remain behind the arrow towers.
            if (countArmy(info, AT_BOWMAN) < 2 && buildingIdle(range)
                && info.Meat >= 90 && info.Wood >= 40) {
                ai.BuildingAction(range->SN, BUILDING_RANGE_CREATE_BOWMAN);
                lastMilitaryOrderFrame = info.GameFrame;
                return;
            }
            if (countArmy(info, AT_SLINGER) < 1 && buildingIdle(camp)
                && info.Meat >= 90 && info.Stone >= 20) {
                ai.BuildingAction(camp->SN, BUILDING_ARMYCAMP_CREATE_SLINGER);
                lastMilitaryOrderFrame = info.GameFrame;
            }
            return;
        }
        if (info.civilizationStage >= CIVILIZATION_BRONZEAGE
            && broadswordResearch.state == 3 && buildingIdle(camp)
            && countArmy(info, AT_BROADSWORDSMAN) < 3
            && info.Meat >= BUILDING_ARMYCAMP_CREATE_BROADSWORD_FOOD
            && info.Gold >= BUILDING_ARMYCAMP_CREATE_BROADSWORD_GOLD) {
            ai.BuildingAction(camp->SN, BUILDING_ARMYCAMP_CREATE_BROADSWORD);
            lastMilitaryOrderFrame = info.GameFrame;
        }
    }

    int nearestThreat(const tagInfo& info, int x, int y, int maxDistance2) const
    {
        int bestSN = -1;
        int bestDistance = maxDistance2 + 1;
        for (const tagArmy& enemy : info.enemy_armies) {
            const int d = distance2(x, y, enemy.BlockDR, enemy.BlockUR);
            if (d < bestDistance) {
                bestDistance = d;
                bestSN = enemy.SN;
            }
        }
        for (const tagFarmer& enemy : info.enemy_farmers) {
            const int d = distance2(x, y, enemy.BlockDR, enemy.BlockUR);
            if (d < bestDistance) {
                bestDistance = d;
                bestSN = enemy.SN;
            }
        }
        return bestSN;
    }

    void updateWaveState(const tagInfo& info)
    {
        if (firstWaveCleared)
            return;
        const pair<int, int> center = centerBlock(info);
        bool enemyArmyNearBase = false;
        for (const tagArmy& enemy : info.enemy_armies) {
            if (distance2(center.first, center.second,
                          enemy.BlockDR, enemy.BlockUR) <= 22 * 22) {
                enemyArmyNearBase = true;
                break;
            }
        }
        if (enemyArmyNearBase)
            firstWaveEngaged = true;
        else if (firstWaveEngaged && info.enemy_armies.empty())
            firstWaveCleared = true;
    }

    const tagArmy* nearestEnemyArmy(const tagInfo& info, int x, int y) const
    {
        const tagArmy* nearest = NULL;
        int nearestDistance = numeric_limits<int>::max();
        for (const tagArmy& enemy : info.enemy_armies) {
            const int d = distance2(x, y, enemy.BlockDR, enemy.BlockUR);
            if (d < nearestDistance) {
                nearestDistance = d;
                nearest = &enemy;
            }
        }
        return nearest;
    }

    void movePriestSafely(UsrAI& ai, const tagInfo& info, const tagArmy& priest,
                          const pair<int, int>& center)
    {
        bool tookDamage = false;
        map<int, int>::iterator previousBlood = lastArmyBlood.find(priest.SN);
        if (previousBlood != lastArmyBlood.end() && priest.Blood < previousBlood->second)
            tookDamage = true;
        lastArmyBlood[priest.SN] = priest.Blood;

        const bool injured = priest.MaxBlood > 0
            && priest.Blood * 100 < priest.MaxBlood * 75;
        const bool personalThreat = nearestThreat(info, priest.BlockDR,
                                                  priest.BlockUR, 20 * 20) != -1;
        const bool baseThreat = nearestThreat(info, center.first,
                                              center.second, 22 * 22) != -1;
        const bool visibleEnemyArmy = !info.enemy_armies.empty();
        const bool danger = tookDamage || personalThreat || baseThreat
            || visibleEnemyArmy;
        const bool enteredDanger = danger
            && priestsInDanger.insert(priest.SN).second;
        if (!danger)
            priestsInDanger.erase(priest.SN);
        const bool enteredDirectThreat = personalThreat
            && priestsUnderDirectThreat.insert(priest.SN).second;
        if (!personalThreat)
            priestsUnderDirectThreat.erase(priest.SN);

        // Before the first wave is confirmed dead, the priest remains inside
        // the tower screen.  Afterwards it may only make short local patrols
        // while healthy and while no enemy is visible.
        const bool mayExplore = firstWaveCleared && !injured
            && info.enemy_armies.empty() && info.enemy_farmers.empty();

        int& lastOrder = lastUnitOrderFrame[priest.SN];
        if (danger || injured || !mayExplore) {
            const tagBuilding* refugeTower = NULL;
            int nearestTowerDistance = numeric_limits<int>::max();
            for (const tagBuilding& building : info.buildings) {
                if (building.Type != BUILDING_ARROWTOWER || building.Percent < 100)
                    continue;
                const int d = distance2(priest.BlockDR, priest.BlockUR,
                                        building.BlockDR, building.BlockUR);
                if (d < nearestTowerDistance) {
                    nearestTowerDistance = d;
                    refugeTower = &building;
                }
            }

            int safeX = min(MAP_L - 1, center.first + 4);
            int safeY = min(MAP_U - 1, center.second + 4);
            const tagArmy* pursuer = nearestEnemyArmy(info, priest.BlockDR,
                                                      priest.BlockUR);
            if (refugeTower != NULL) {
                const int towerX = refugeTower->BlockDR + 1;
                const int towerY = refugeTower->BlockUR + 1;
                const int directionX = pursuer != NULL
                    ? towerX - pursuer->BlockDR : center.first - towerX;
                const int directionY = pursuer != NULL
                    ? towerY - pursuer->BlockUR : center.second - towerY;
                safeX = towerX
                    + (directionX > 0 ? 5 : (directionX < 0 ? -5 : 0));
                safeY = towerY
                    + (directionY > 0 ? 5 : (directionY < 0 ? -5 : 0));
            }

            // Once an attacker is close enough to keep its target lock, do not
            // stop at the first refuge coordinate.  Keeping the escape point
            // ahead of the priest makes it kite past the tower while it fires.
            if (pursuer != NULL && (personalThreat || tookDamage)) {
                int directionX = priest.BlockDR - pursuer->BlockDR;
                int directionY = priest.BlockUR - pursuer->BlockUR;
                if (directionX == 0)
                    directionX = center.first - pursuer->BlockDR;
                if (directionY == 0)
                    directionY = center.second - pursuer->BlockUR;
                safeX = priest.BlockDR + (directionX >= 0 ? 8 : -8);
                safeY = priest.BlockUR + (directionY >= 0 ? 8 : -8);
                safeX = max(center.first - 18, min(center.first + 18, safeX));
                safeY = max(center.second - 18, min(center.second + 18, safeY));
            }
            safeX = max(0, min(MAP_L - 1, safeX));
            safeY = max(0, min(MAP_U - 1, safeY));

            const bool urgentRetreat = tookDamage || enteredDanger
                || enteredDirectThreat;
            const int orderCooldown = personalThreat ? 8 : (danger ? 30 : 75);
            if ((priest.WorkObjectSN != -1
                 || personalThreat
                 || distance2(priest.BlockDR, priest.BlockUR, safeX, safeY) > 4)
                && (urgentRetreat
                    || info.GameFrame - lastOrder > orderCooldown)) {
                ai.HumanMove(priest.SN,
                             (safeX + 0.5) * double(BLOCKSIDELENGTH),
                             (safeY + 0.5) * double(BLOCKSIDELENGTH));
                lastOrder = info.GameFrame;
            }
            return;
        }

        if (priest.WorkObjectSN != -1 || priest.NowState != HUMAN_STATE_IDLE)
            return;
        if (info.GameFrame - lastOrder <= 125)
            return;

        static const int waypointOffsets[][2] = {
            { 5, 0 }, { 4, 3 }, { 0, 5 }, { -4, 3 },
            { -5, 0 }, { -4, -3 }, { 0, -5 }, { 4, -3 }
        };
        int& waypoint = priestWaypointIndex[priest.SN];
        const int waypointCount = int(sizeof(waypointOffsets) / sizeof(waypointOffsets[0]));
        const int targetX = max(0, min(MAP_L - 1,
                                      center.first + waypointOffsets[waypoint][0]));
        const int targetY = max(0, min(MAP_U - 1,
                                      center.second + waypointOffsets[waypoint][1]));
        waypoint = (waypoint + 1) % waypointCount;
        ai.HumanMove(priest.SN,
                     (targetX + 0.5) * double(BLOCKSIDELENGTH),
                     (targetY + 0.5) * double(BLOCKSIDELENGTH));
        lastOrder = info.GameFrame;
    }

    void selectArmyScout(const tagInfo& info)
    {
        bool currentScoutAlive = false;
        int bowmanCount = 0;
        int newestBowmanSN = -1;
        int fallbackScoutSN = -1;
        for (const tagArmy& army : info.armies) {
            if (army.SN == armyScoutSN && army.Sort != AT_PRIEST
                && army.Sort != AT_SHIP)
                currentScoutAlive = true;
            if (army.Sort == AT_BOWMAN) {
                ++bowmanCount;
                newestBowmanSN = max(newestBowmanSN, army.SN);
            }
            if (army.Sort == AT_SLINGER || army.Sort == AT_BOWMAN)
                fallbackScoutSN = max(fallbackScoutSN, army.SN);
        }
        if (!currentScoutAlive)
            armyScoutSN = -1;
        if (armyScoutSN == -1 && bowmanCount >= 2) {
            armyScoutSN = newestBowmanSN;
            armyScoutWaypoint = 0;
        }
        else if (armyScoutSN == -1 && firstWaveCleared
                 && fallbackScoutSN != -1) {
            // The latest log did not start scouting until the second bowman
            // appeared.  After wave one, the existing tool-age ranged unit is
            // safe to use immediately instead of waiting several more minutes.
            armyScoutSN = fallbackScoutSN;
            armyScoutWaypoint = 0;
        }
    }

    bool manageArmyScout(UsrAI& ai, const tagInfo& info, const tagArmy& scout,
                         const pair<int, int>& center)
    {
        if (scout.SN != armyScoutSN || (huntAnchorValid && goldAnchorValid))
            return false;

        const bool danger = !info.enemy_armies.empty()
            || nearestThreat(info, scout.BlockDR, scout.BlockUR, 16 * 16) != -1
            || (firstWaveEngaged && !firstWaveCleared);
        int& lastOrder = lastUnitOrderFrame[scout.SN];
        if (danger) {
            const tagBuilding* refuge = NULL;
            int bestDistance = numeric_limits<int>::max();
            for (const tagBuilding& building : info.buildings) {
                if (building.Type != BUILDING_ARROWTOWER || building.Percent < 100)
                    continue;
                const int d = distance2(scout.BlockDR, scout.BlockUR,
                                        building.BlockDR, building.BlockUR);
                if (d < bestDistance) {
                    bestDistance = d;
                    refuge = &building;
                }
            }
            int targetX = center.first;
            int targetY = center.second;
            if (refuge != NULL) {
                targetX = refuge->BlockDR
                    + (center.first >= refuge->BlockDR ? 4 : -2);
                targetY = refuge->BlockUR
                    + (center.second >= refuge->BlockUR ? 4 : -2);
            }
            targetX = max(0, min(MAP_L - 1, targetX));
            targetY = max(0, min(MAP_U - 1, targetY));
            if (info.GameFrame - lastOrder > 20
                && (scout.WorkObjectSN != -1
                    || distance2(scout.BlockDR, scout.BlockUR,
                                 targetX, targetY) > 4)) {
                ai.HumanMove(scout.SN,
                             (targetX + 0.5) * double(BLOCKSIDELENGTH),
                             (targetY + 0.5) * double(BLOCKSIDELENGTH));
                lastOrder = info.GameFrame;
            }
            return true;
        }

        // Search the complete near-base ring, starting on the map-centre side.
        // The unit keeps going until both a herd and a gold deposit are known;
        // villagers stay home until the herd itself is visible.
        static const int waypointOffsets[][2] = {
            { 12, 0 }, { 18, 4 }, { 22, 8 }, { 18, -6 },
            { 8, 14 }, { 8, -14 }, { -10, 10 }, { -10, -10 },
            { -16, 0 }, { 0, 18 }, { 0, -18 },
            { 28, 0 }, { 24, 16 }, { 24, -16 }, { 0, 28 },
            { 0, -28 }, { -24, 16 }, { -24, -16 }, { -30, 0 },
            { 34, 10 }, { 34, -10 }, { -34, 10 }, { -34, -10 }
        };
        const int waypointCount = int(sizeof(waypointOffsets)
                                      / sizeof(waypointOffsets[0]));
        const int directionX = MAP_L / 2 >= center.first ? 1 : -1;
        const int directionY = MAP_U / 2 >= center.second ? 1 : -1;
        int targetX = center.first
            + directionX * waypointOffsets[armyScoutWaypoint][0];
        int targetY = center.second
            + directionY * waypointOffsets[armyScoutWaypoint][1];
        targetX = max(0, min(MAP_L - 1, targetX));
        targetY = max(0, min(MAP_U - 1, targetY));

        const bool arrived = distance2(scout.BlockDR, scout.BlockUR,
                                       targetX, targetY) <= 9;
        const bool stalled = scout.NowState == HUMAN_STATE_IDLE
            && info.GameFrame - lastOrder > 120;
        if (arrived || stalled) {
            armyScoutWaypoint = (armyScoutWaypoint + 1) % waypointCount;
            targetX = center.first
                + directionX * waypointOffsets[armyScoutWaypoint][0];
            targetY = center.second
                + directionY * waypointOffsets[armyScoutWaypoint][1];
            targetX = max(0, min(MAP_L - 1, targetX));
            targetY = max(0, min(MAP_U - 1, targetY));
        }
        if (arrived || stalled || info.GameFrame - lastOrder > 120) {
            ai.HumanMove(scout.SN,
                         (targetX + 0.5) * double(BLOCKSIDELENGTH),
                         (targetY + 0.5) * double(BLOCKSIDELENGTH));
            lastOrder = info.GameFrame;
        }
        return true;
    }

    void manageDefense(UsrAI& ai, const tagInfo& info)
    {
        selectArmyScout(info);
        set<int> priestSNs;
        for (const tagArmy& army : info.armies) {
            if (army.Sort == AT_PRIEST)
                priestSNs.insert(army.SN);
        }
        set<int> claimedTowerTargets;
        for (const tagBuilding& building : info.buildings) {
            if (building.Type != BUILDING_ARROWTOWER || building.Percent < 100)
                continue;
            int target = -1;
            int bestDistance = 8 * 8 + 1;
            for (const tagArmy& enemy : info.enemy_armies) {
                if (priestSNs.count(enemy.WorkObjectSN) == 0
                    || claimedTowerTargets.count(enemy.SN) != 0)
                    continue;
                const int d = distance2(building.BlockDR, building.BlockUR,
                                        enemy.BlockDR, enemy.BlockUR);
                if (d < bestDistance) {
                    bestDistance = d;
                    target = enemy.SN;
                }
            }
            if (target == -1 && building.Project != -1
                && claimedTowerTargets.count(building.Project) == 0) {
                target = building.Project;
            }
            if (target == -1) {
                for (const tagArmy& enemy : info.enemy_armies) {
                    if (claimedTowerTargets.count(enemy.SN) != 0)
                        continue;
                    const int d = distance2(building.BlockDR, building.BlockUR,
                                            enemy.BlockDR, enemy.BlockUR);
                    if (d < bestDistance) {
                        bestDistance = d;
                        target = enemy.SN;
                    }
                }
            }
            if (target != -1 && building.Project != target)
                ai.HumanAction(building.SN, target);
            if (target != -1)
                claimedTowerTargets.insert(target);
        }

        const pair<int, int> center = centerBlock(info);
        const tagBuilding* forwardTower = NULL;
        for (const tagBuilding& building : info.buildings) {
            if (building.Type == BUILDING_ARROWTOWER && building.Percent >= 100) {
                if (forwardTower == NULL
                    || distance2(center.first, center.second, building.BlockDR, building.BlockUR)
                       > distance2(center.first, center.second,
                                   forwardTower->BlockDR, forwardTower->BlockUR))
                    forwardTower = &building;
            }
        }
        const int defendX = forwardTower != NULL ? forwardTower->BlockDR : center.first;
        const int defendY = forwardTower != NULL ? forwardTower->BlockUR : center.second;
        const int centerSideX = forwardTower == NULL ? center.first
            : forwardTower->BlockDR + (center.first >= forwardTower->BlockDR ? 3 : -1);
        const int centerSideY = forwardTower == NULL ? center.second
            : forwardTower->BlockUR + (center.second >= forwardTower->BlockUR ? 3 : -1);
        for (const tagArmy& army : info.armies) {
            if (army.Sort == AT_SHIP)
                continue;
            if (army.Sort == AT_PRIEST) {
                movePriestSafely(ai, info, army, center);
                continue;
            }
            if (manageArmyScout(ai, info, army, center))
                continue;
            if (!firstWaveCleared) {
                const bool enemyIsClose = nearestThreat(info, army.BlockDR,
                                                        army.BlockUR, 5 * 5) != -1;
                int targetX = centerSideX;
                int targetY = centerSideY;
                if (enemyIsClose && forwardTower != NULL) {
                    targetX = forwardTower->BlockDR
                        + (center.first >= forwardTower->BlockDR ? 6 : -4);
                    targetY = forwardTower->BlockUR
                        + (center.second >= forwardTower->BlockUR ? 6 : -4);
                }
                targetX = max(0, min(MAP_L - 1, targetX));
                targetY = max(0, min(MAP_U - 1, targetY));
                const int cooldown = enemyIsClose ? 20 : 150;
                if ((army.WorkObjectSN != -1
                     || (army.NowState == HUMAN_STATE_IDLE
                         && distance2(army.BlockDR, army.BlockUR,
                                      targetX, targetY) > 4))
                    && info.GameFrame - lastUnitOrderFrame[army.SN] > cooldown) {
                    ai.HumanMove(army.SN,
                                 (targetX + 0.5) * double(BLOCKSIDELENGTH),
                                 (targetY + 0.5) * double(BLOCKSIDELENGTH));
                    lastUnitOrderFrame[army.SN] = info.GameFrame;
                }
                continue;
            }
            const int target = nearestThreat(info, army.BlockDR, army.BlockUR, 12 * 12);
            if (target != -1 && army.WorkObjectSN == -1) {
                ai.HumanAction(army.SN, target);
                lastUnitOrderFrame[army.SN] = info.GameFrame;
            }
            else if (army.WorkObjectSN == -1 && army.NowState == HUMAN_STATE_IDLE
                     && distance2(army.BlockDR, army.BlockUR, defendX, defendY) > 9
                     && info.GameFrame - lastUnitOrderFrame[army.SN] > 250) {
                ai.HumanMove(army.SN,
                             (defendX + 1.5) * double(BLOCKSIDELENGTH),
                             (defendY + 1.5) * double(BLOCKSIDELENGTH));
                lastUnitOrderFrame[army.SN] = info.GameFrame;
            }
        }
    }

    vector<const tagResource*> homeBerries(const tagInfo& info) const
    {
        vector<const tagResource*> berries;
        const pair<int, int> center = centerBlock(info);
        for (const tagResource& resource : info.resources) {
            if (resource.Type == RESOURCE_BUSH && resource.Cnt > 0
                && distance2(center.first, center.second,
                             resource.BlockDR, resource.BlockUR)
                   <= kHomeBerryRadius * kHomeBerryRadius)
                berries.push_back(&resource);
        }
        sort(berries.begin(), berries.end(), [&center](const tagResource* a,
                                                       const tagResource* b) {
            return distance2(center.first, center.second, a->BlockDR, a->BlockUR)
                 < distance2(center.first, center.second, b->BlockDR, b->BlockUR);
        });
        return berries;
    }

    void updateFoodPlan(const tagInfo& info)
    {
        const vector<const tagResource*> berries = homeBerries(info);
        if (!berries.empty())
            sawHomeBerries = true;

        const pair<int, int> center = centerBlock(info);
        for (const tagFarmer& farmer : info.farmers) {
            const tagResource* target = findResourceBySN(info, farmer.WorkObjectSN);
            if (target != NULL && target->Type == RESOURCE_BUSH
                && distance2(center.first, center.second,
                             target->BlockDR, target->BlockUR)
                   <= kHomeBerryRadius * kHomeBerryRadius) {
                formerBerryWorkers.insert(farmer.SN);
            }
        }

        if (sawHomeBerries && berries.empty())
            homeBerriesDepleted = true;

        // Keep the first explored gold deposit even if fog later hides it.
        if (!goldAnchorValid) {
            int bestGoldDistance = numeric_limits<int>::max();
            for (const tagResource& resource : info.resources) {
                if (resource.Type != RESOURCE_GOLD || resource.Cnt <= 0)
                    continue;
                const int d = distance2(center.first, center.second,
                                        resource.BlockDR, resource.BlockUR);
                if (d < bestGoldDistance) {
                    bestGoldDistance = d;
                    goldAnchorX = resource.BlockDR;
                    goldAnchorY = resource.BlockUR;
                    goldAnchorValid = true;
                }
            }
        }
        if (huntFinished)
            return;

        // Remember a herd as soon as the military scout reveals it.  Hunting
        // still waits for the home berries to run out in manageHunt().
        if (!huntAnchorValid) {
            const tagResource* best = NULL;
            int bestNeighbours = 0;
            int bestCenterDistance = numeric_limits<int>::max();
            for (const tagResource& candidate : info.resources) {
                if (candidate.Type != RESOURCE_GAZELLE || candidate.Cnt <= 0
                    || candidate.Blood <= 0)
                    continue;
                int neighbours = 0;
                for (const tagResource& other : info.resources) {
                    if (other.Type == RESOURCE_GAZELLE && other.Cnt > 0
                        && distance2(candidate.BlockDR, candidate.BlockUR,
                                     other.BlockDR, other.BlockUR) <= 7 * 7)
                        ++neighbours;
                }
                const int centerDistance = distance2(center.first, center.second,
                                                     candidate.BlockDR, candidate.BlockUR);
                if (neighbours >= 2
                    && (neighbours > bestNeighbours
                        || (neighbours == bestNeighbours
                            && centerDistance < bestCenterDistance))) {
                    best = &candidate;
                    bestNeighbours = neighbours;
                    bestCenterDistance = centerDistance;
                }
            }
            if (best != NULL) {
                herdGazelles.clear();
                int sumX = 0;
                int sumY = 0;
                int count = 0;
                for (const tagResource& member : info.resources) {
                    if (member.Type == RESOURCE_GAZELLE && member.Cnt > 0
                        && distance2(best->BlockDR, best->BlockUR,
                                     member.BlockDR, member.BlockUR) <= 7 * 7) {
                        sumX += member.BlockDR;
                        sumY += member.BlockUR;
                        herdGazelles.insert(member.SN);
                        ++count;
                    }
                }
                huntAnchorX = count > 0 ? sumX / count : best->BlockDR;
                huntAnchorY = count > 0 ? sumY / count : best->BlockUR;
                huntAnchorValid = true;
            }
        }

        if (!homeBerriesDepleted || !huntAnchorValid)
            return;
        for (const tagResource& resource : info.resources) {
            if (resource.Type == RESOURCE_GAZELLE && resource.Cnt > 0
                && distance2(huntAnchorX, huntAnchorY,
                             resource.BlockDR, resource.BlockUR)
                   <= kHerdRadius * kHerdRadius)
                herdGazelles.insert(resource.SN);
        }
        bool herdResourceVisible = false;
        bool liveGazelleVisible = false;
        for (const tagResource& resource : info.resources) {
            if (resource.Type == RESOURCE_GAZELLE && resource.Cnt > 0
                && herdGazelles.count(resource.SN) != 0) {
                herdResourceVisible = true;
                if (resource.Blood > 0)
                    liveGazelleVisible = true;
                else
                    firstGazelleKilled = true;
            }
        }
        if (firstGazelleKilled && !liveGazelleVisible)
            herdKilled = true;
        if (herdKilled && !herdResourceVisible) {
            huntFinished = true;
            currentGazelleSN = -1;
        }
    }

    const tagFarmer* chooseHuntWorker(const tagInfo& info, int targetX, int targetY,
                                      const set<int>& commanded,
                                      const set<int>& excluded,
                                      bool formerBerryOnly) const
    {
        const tagFarmer* best = NULL;
        long bestScore = numeric_limits<long>::max();
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || farmer.NowState != HUMAN_STATE_IDLE
                || commanded.count(farmer.SN) != 0
                || excluded.count(farmer.SN) != 0
                || workerIsConstructing(info, farmer)
                || (formerBerryOnly && formerBerryWorkers.count(farmer.SN) == 0))
                continue;
            long score = distance2(farmer.BlockDR, farmer.BlockUR, targetX, targetY);
            if (huntWorkers.count(farmer.SN) != 0)
                score -= 100000;
            else if (formerBerryWorkers.count(farmer.SN) != 0)
                score -= 50000;
            if (score < bestScore) {
                bestScore = score;
                best = &farmer;
            }
        }
        return best;
    }

    const tagBuilding* nearestCarriedResourceDropoff(const tagInfo& info,
                                                      const tagFarmer& farmer,
                                                      int excludedSN = -1) const
    {
        const tagBuilding* best = NULL;
        int bestDistance = numeric_limits<int>::max();
        for (const tagBuilding& building : info.buildings) {
            if (building.Percent < 100)
                continue;
            if (building.SN == excludedSN)
                continue;
            const bool acceptsResource = building.Type == BUILDING_CENTER
                || (farmer.ResourceSort == HUMAN_GRANARYFOOD
                    && building.Type == BUILDING_GRANARY)
                || ((farmer.ResourceSort == HUMAN_STOCKFOOD
                     || farmer.ResourceSort == HUMAN_WOOD
                     || farmer.ResourceSort == HUMAN_STONE
                     || farmer.ResourceSort == HUMAN_GOLD)
                    && building.Type == BUILDING_STOCK);
            if (!acceptsResource)
                continue;
            const int d = distance2(farmer.BlockDR, farmer.BlockUR,
                                    building.BlockDR, building.BlockUR);
            if (d < bestDistance) {
                best = &building;
                bestDistance = d;
            }
        }
        return best;
    }

    void depositFormerBerryLoads(const tagInfo& info,
                                 set<int>& commanded)
    {
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || formerBerryWorkers.count(farmer.SN) == 0
                || farmer.Resource <= 0
                || farmer.ResourceSort != HUMAN_GRANARYFOOD)
                continue;
            const tagBuilding* dropoff = nearestCarriedResourceDropoff(info, farmer);
            if (dropoff != NULL && workerLeaseBlocked(farmer.SN, dropoff->SN))
                dropoff = nearestCarriedResourceDropoff(info, farmer, dropoff->SN);
            if (dropoff == NULL)
                continue;
            publishWorkerAction(farmer.SN, dropoff->SN,
                                WORKER_TASK_DEPOSIT, 1000, TOPIC_HUNT);
            // Do not let later assignment code replace the final delivery.
            commanded.insert(farmer.SN);
        }
    }

    void gatherHerdCarcasses(const tagInfo& info,
                             set<int>& commanded)
    {
        vector<const tagResource*> carcasses;
        for (const tagResource& resource : info.resources) {
            if (resource.Type == RESOURCE_GAZELLE && resource.Cnt > 0
                && resource.Blood <= 0
                && herdGazelles.count(resource.SN) != 0)
                carcasses.push_back(&resource);
        }
        if (carcasses.empty())
            return;

        map<int, int> gathererCounts;
        // A stale WorkObjectSN left by the killing blow is not sufficient:
        // only a non-idle villager is considered to be actively gathering.
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || (formerBerryWorkers.count(farmer.SN) == 0
                    && huntWorkers.count(farmer.SN) == 0))
                continue;
            const tagResource* work = findResourceBySN(info, farmer.WorkObjectSN);
            if (work != NULL && work->Type == RESOURCE_GAZELLE
                && work->Blood <= 0 && work->Cnt > 0
                && herdGazelles.count(work->SN) != 0
                && !workerLeaseBlocked(farmer.SN, work->SN)
                && farmer.NowState != HUMAN_STATE_IDLE) {
                ++gathererCounts[work->SN];
                publishWorkerAction(farmer.SN, work->SN,
                                    WORKER_TASK_GATHER, 750, TOPIC_HUNT);
                commanded.insert(farmer.SN);
            }
        }

        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || (formerBerryWorkers.count(farmer.SN) == 0
                    && huntWorkers.count(farmer.SN) == 0)
                || commanded.count(farmer.SN) != 0
                || workerIsConstructing(info, farmer))
                continue;

            if (farmer.Resource > 0) {
                const tagBuilding* dropoff =
                    nearestCarriedResourceDropoff(info, farmer);
                if (dropoff != NULL
                    && workerLeaseBlocked(farmer.SN, dropoff->SN))
                    dropoff = nearestCarriedResourceDropoff(info, farmer,
                                                            dropoff->SN);
                if (dropoff != NULL)
                    publishWorkerAction(farmer.SN, dropoff->SN,
                                        WORKER_TASK_DEPOSIT, 1000, TOPIC_HUNT);
                commanded.insert(farmer.SN);
                continue;
            }

            const tagResource* best = NULL;
            long bestScore = numeric_limits<long>::max();
            for (const tagResource* carcass : carcasses) {
                if (workerLeaseBlocked(farmer.SN, carcass->SN))
                    continue;
                const long score = long(gathererCounts[carcass->SN]) * 100000L
                    + distance2(farmer.BlockDR, farmer.BlockUR,
                                carcass->BlockDR, carcass->BlockUR);
                if (score < bestScore) {
                    bestScore = score;
                    best = carcass;
                }
            }
            if (best != NULL) {
                publishWorkerAction(farmer.SN, best->SN,
                                    WORKER_TASK_GATHER, 750, TOPIC_HUNT);
                ++gathererCounts[best->SN];
                commanded.insert(farmer.SN);
            }
        }
    }

    void manageHunt(const tagInfo& info, set<int>& commanded)
    {
        if (!homeBerriesDepleted || huntFinished)
            return;
        depositFormerBerryLoads(info, commanded);
        if (!huntAnchorValid)
            return;
        if (!hasStockNearHerd(info, true))
            return;
        if (herdKilled) {
            gatherHerdCarcasses(info, commanded);
            return;
        }

        const tagResource* target = findResourceBySN(info, currentGazelleSN);
        if (target == NULL || target->Type != RESOURCE_GAZELLE || target->Cnt <= 0) {
            currentGazelleSN = -1;
            target = NULL;
        }
        if (!herdKilled && target != NULL && target->Blood <= 0) {
            firstGazelleKilled = true;
            currentGazelleSN = -1;
            target = NULL;
        }
        if (target == NULL) {
            int bestDistance = numeric_limits<int>::max();
            for (const tagResource& gazelle : info.resources) {
                if (gazelle.Type != RESOURCE_GAZELLE || gazelle.Cnt <= 0
                    || herdGazelles.count(gazelle.SN) == 0)
                    continue;
                if (gazelle.Blood <= 0)
                    continue;
                const int d = distance2(huntAnchorX, huntAnchorY,
                                        gazelle.BlockDR, gazelle.BlockUR);
                if (d < bestDistance) {
                    bestDistance = d;
                    target = &gazelle;
                }
            }
            if (target != NULL)
                currentGazelleSN = target->SN;
        }
        if (target == NULL) {
            if (!herdKilled && firstGazelleKilled)
                herdKilled = true;
            return;
        }

        set<int> assigned;
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort == FARMERTYPE_FARMER
                && farmer.WorkObjectSN == target->SN
                && !workerLeaseBlocked(farmer.SN, target->SN)
                && (farmer.NowState != HUMAN_STATE_IDLE
                    || info.GameFrame - lastUnitOrderFrame[farmer.SN] <= 30))
                assigned.insert(farmer.SN);
        }

        if (!herdKilled) {
            vector<const tagFarmer*> reinforcements;
            set<int> reserved = assigned;
            const size_t needed = assigned.size() < 2 ? 2 - assigned.size() : 0;
            while (reinforcements.size() < needed) {
                const tagFarmer* hunter = chooseHuntWorker(info, target->BlockDR,
                                                           target->BlockUR,
                                                           commanded, reserved, false);
                if (hunter == NULL)
                    break;
                if (workerLeaseBlocked(hunter->SN, target->SN)) {
                    reserved.insert(hunter->SN);
                    continue;
                }
                reinforcements.push_back(hunter);
                reserved.insert(hunter->SN);
            }
            // Never start a fresh attack with only one villager.  Waiting for
            // both candidates is safer than letting a gazelle escape wounded.
            if (reinforcements.size() == needed) {
                for (const tagFarmer* hunter : reinforcements) {
                    publishWorkerAction(hunter->SN, target->SN,
                                        WORKER_TASK_HUNT, 800, TOPIC_HUNT);
                    commanded.insert(hunter->SN);
                    huntWorkers.insert(hunter->SN);
                }
            }

            // Everyone outside the attacking pair remains available to the
            // economy subscriber until all deer are dead.
            return;
        }

    }

    void assignFarms(const tagInfo& info, set<int>& commanded)
    {
        if (!huntFinished)
            return;
        set<int> claimedFarms;
        set<int> blockedFarmWorkers;
        for (const tagFarmer& farmer : info.farmers) {
            const tagBuilding* target = findBuildingBySN(info, farmer.WorkObjectSN);
            if (target != NULL && target->Type == BUILDING_FARM && target->Cnt > 0) {
                if (workerLeaseBlocked(farmer.SN, target->SN)) {
                    blockedFarmWorkers.insert(farmer.SN);
                    continue;
                }
                claimedFarms.insert(target->SN);
                publishWorkerAction(farmer.SN, target->SN,
                                    WORKER_TASK_FARM, 600, TOPIC_FARM);
                commanded.insert(farmer.SN);
            }
        }
        for (const tagBuilding& farm : info.buildings) {
            if (farm.Type != BUILDING_FARM || farm.Percent < 100 || farm.Cnt <= 0
                || claimedFarms.count(farm.SN) != 0)
                continue;
            const tagFarmer* worker = chooseHuntWorker(info, farm.BlockDR, farm.BlockUR,
                                                       commanded, blockedFarmWorkers,
                                                       false);
            if (worker == NULL)
                return;
            publishWorkerAction(worker->SN, farm.SN,
                                WORKER_TASK_FARM, 600, TOPIC_FARM);
            commanded.insert(worker->SN);
            claimedFarms.insert(farm.SN);
        }
    }

    const tagResource* nearestUnclaimedResource(const tagInfo& info,
                                                const tagFarmer& worker,
                                                ResourceRole role,
                                                const set<int>& claimed,
                                                bool excludeHomeBerries) const
    {
        const pair<int, int> center = centerBlock(info);
        const tagResource* best = NULL;
        int bestDistance = numeric_limits<int>::max();
        const bool requirePreferredDropoff = hasPreferredResource(info, role);
        for (const tagResource& resource : info.resources) {
            if (resource.Cnt <= 0 || claimed.count(resource.SN) != 0
                || resourceRole(resource) != role)
                continue;
            if (resource.Type == RESOURCE_GAZELLE
                || resource.Type == RESOURCE_ELEPHANT)
                continue;
            if (excludeHomeBerries && resource.Type == RESOURCE_BUSH
                && distance2(center.first, center.second,
                             resource.BlockDR, resource.BlockUR)
                   <= kHomeBerryRadius * kHomeBerryRadius)
                continue;
            int dropoffDistance = 0;
            const bool preferredDropoff = nearPreferredDropoff(info, resource,
                                                               &dropoffDistance);
            if (requirePreferredDropoff && !preferredDropoff)
                continue;

            // Walking to the resource still matters, but keeping the resource
            // next to its matching drop-off matters more over many round trips.
            const int d = distance2(worker.BlockDR, worker.BlockUR,
                                    resource.BlockDR, resource.BlockUR)
                        + dropoffDistance * dropoffDistance * 6;
            if (d < bestDistance) {
                bestDistance = d;
                best = &resource;
            }
        }
        return best;
    }

    void fillRoleTargets(const tagInfo& info, int berryWorkers,
                         int targets[4]) const
    {
        targets[ROLE_FOOD] = info.civilizationStage >= CIVILIZATION_BRONZEAGE
            ? 12 : max(6, berryWorkers);
        targets[ROLE_WOOD] = info.civilizationStage >= CIVILIZATION_BRONZEAGE
            ? 4 : 3;
        targets[ROLE_STONE] = 2;
        targets[ROLE_GOLD] = info.civilizationStage >= CIVILIZATION_BRONZEAGE
            ? 4 : (info.civilizationStage >= CIVILIZATION_TOOLAGE ? 2 : 0);
    }

    ResourceRole chooseRole(const tagInfo& info, const int counts[4],
                            int berryWorkers) const
    {
        int targets[4];
        fillRoleTargets(info, berryWorkers, targets);
        if (info.civilizationStage < CIVILIZATION_BRONZEAGE) {
            if (counts[ROLE_FOOD] < 6)
                return ROLE_FOOD;
            if (counts[ROLE_WOOD] < 3)
                return ROLE_WOOD;
            if (counts[ROLE_STONE] < 2)
                return ROLE_STONE;
        }

        double weights[4] = { 1.0, 1.0, 1.0, 1.0 };
        if (info.Meat < 850)
            weights[ROLE_FOOD] = 1.15;
        if (info.Wood < 350)
            weights[ROLE_WOOD] = 1.25;
        if (info.Stone < 80)
            weights[ROLE_STONE] = 1.20;
        if (info.Gold < 120)
            weights[ROLE_GOLD] = 1.18;

        ResourceRole best = ROLE_NONE;
        double bestNeed = 0.0;
        for (int role = ROLE_FOOD; role <= ROLE_GOLD; ++role) {
            const double need = weights[role]
                * double(max(0, targets[role] - counts[role]))
                / double(max(1, targets[role]));
            if (need > bestNeed) {
                bestNeed = need;
                best = static_cast<ResourceRole>(role);
            }
        }
        return best;
    }

    void assignResources(const tagInfo& info,
                         const set<int>& alreadyCommanded)
    {
        set<int> commanded = alreadyCommanded;
        set<int> claimed;
        set<int> reassignToPreferredDropoff;
        const vector<const tagResource*> berries = homeBerries(info);
        int roleTargets[4];
        fillRoleTargets(info, static_cast<int>(berries.size()), roleTargets);
        int roleCounts[4] = { 0, 0, 0, 0 };

        // Delivery is a separate high-priority task.  It prevents an idle
        // carrier from being mistaken for an active gatherer merely because
        // WorkObjectSN still points at the old resource.
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || commanded.count(farmer.SN) != 0
                || farmer.Resource <= 0
                || farmer.NowState != HUMAN_STATE_IDLE)
                continue;
            const tagBuilding* dropoff = nearestCarriedResourceDropoff(info, farmer);
            if (dropoff != NULL && workerLeaseBlocked(farmer.SN, dropoff->SN))
                dropoff = nearestCarriedResourceDropoff(info, farmer, dropoff->SN);
            if (dropoff == NULL)
                continue;
            publishWorkerAction(farmer.SN, dropoff->SN,
                                WORKER_TASK_DEPOSIT, 950, TOPIC_ECONOMY);
            commanded.insert(farmer.SN);
        }

        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || commanded.count(farmer.SN) != 0)
                continue;
            const tagResource* target = findResourceBySN(info, farmer.WorkObjectSN);
            if (target != NULL && target->Cnt > 0) {
                if (workerLeaseBlocked(farmer.SN, target->SN)) {
                    claimed.insert(target->SN);
                    reassignToPreferredDropoff.insert(farmer.SN);
                    continue;
                }
                const ResourceRole targetRole = resourceRole(*target);
                if (target->Type == RESOURCE_ELEPHANT) {
                    reassignToPreferredDropoff.insert(farmer.SN);
                    continue;
                }
                if (targetRole != ROLE_NONE && targetRole != ROLE_FOOD
                    && roleCounts[targetRole] >= roleTargets[targetRole]) {
                    reassignToPreferredDropoff.insert(farmer.SN);
                    continue;
                }
                if (targetRole == ROLE_WOOD && hasPreferredResource(info, ROLE_WOOD)
                    && !nearPreferredDropoff(info, *target)) {
                    reassignToPreferredDropoff.insert(farmer.SN);
                    continue;
                }
                claimed.insert(target->SN);
                const ResourceRole role = targetRole;
                if (role != ROLE_NONE)
                    ++roleCounts[role];
                publishWorkerAction(farmer.SN, target->SN,
                                    WORKER_TASK_GATHER, 400, TOPIC_ECONOMY);
                commanded.insert(farmer.SN);
            }
        }

        vector<const tagFarmer*> idle;
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort == FARMERTYPE_FARMER
                && (farmer.NowState == HUMAN_STATE_IDLE
                    || reassignToPreferredDropoff.count(farmer.SN) != 0)
                && commanded.count(farmer.SN) == 0)
                idle.push_back(&farmer);
        }

        for (const tagResource* berry : berries) {
            if (claimed.count(berry->SN) != 0 || idle.empty())
                continue;
            vector<const tagFarmer*>::iterator best = idle.begin();
            int bestDistance = numeric_limits<int>::max();
            for (vector<const tagFarmer*>::iterator it = idle.begin(); it != idle.end(); ++it) {
                const int d = distance2((*it)->BlockDR, (*it)->BlockUR,
                                        berry->BlockDR, berry->BlockUR);
                if (d < bestDistance) {
                    bestDistance = d;
                    best = it;
                }
            }
            publishWorkerAction((*best)->SN, berry->SN,
                                WORKER_TASK_GATHER, 500, TOPIC_ECONOMY);
            commanded.insert((*best)->SN);
            claimed.insert(berry->SN);
            ++roleCounts[ROLE_FOOD];
            idle.erase(best);
        }

        while (!idle.empty()) {
            const tagFarmer* worker = idle.back();
            idle.pop_back();
            ResourceRole preferred = chooseRole(info, roleCounts,
                                                static_cast<int>(berries.size()));
            const tagResource* target = NULL;
            for (int attempt = 0; attempt < 4 && target == NULL; ++attempt) {
                const ResourceRole role = static_cast<ResourceRole>((preferred + attempt) % 4);
                if (roleCounts[role] >= roleTargets[role])
                    continue;
                target = nearestUnclaimedResource(info, *worker, role, claimed, true);
                if (target != NULL)
                    preferred = role;
            }
            if (target == NULL) {
                if (reassignToPreferredDropoff.count(worker->SN) != 0) {
                    const pair<int, int> center = centerBlock(info);
                    if (distance2(worker->BlockDR, worker->BlockUR,
                                  center.first, center.second) > 4)
                        publishWorkerMove(worker->SN, center.first, center.second,
                                          100, TOPIC_ECONOMY);
                }
                continue;
            }
            publishWorkerAction(worker->SN, target->SN,
                                WORKER_TASK_GATHER, 400, TOPIC_ECONOMY);
            claimed.insert(target->SN);
            ++roleCounts[preferred];
        }
    }

    void telemetry(UsrAI& ai, const tagInfo& info)
    {
        if (info.GameFrame - lastTelemetryFrame < 500)
            return;
        lastTelemetryFrame = info.GameFrame;
        int blockedWorkerTasks = 0;
        for (map<int, WorkerLease>::const_iterator it = workerLeases.begin();
             it != workerLeases.end(); ++it) {
            if (it->second.state == 3)
                ++blockedWorkerTasks;
        }
        ostringstream text;
        text << "strategy frame=" << info.GameFrame
             << " age=" << info.civilizationStage
             << " res=" << info.Wood << '/' << info.Meat << '/'
             << info.Stone << '/' << info.Gold
             << " farmers=" << countFarmers(info)
             << " pop=" << info.Human_Num << '/' << info.Human_MaxNum
             << " tower=" << countBuildings(info, BUILDING_ARROWTOWER, true)
             << " prereq=" << (bronzePrerequisitesReady(info) ? 1 : 0)
             << " bronzeArmy=" << countArmy(info, AT_BROADSWORDSMAN)
             << " firstWave="
             << (firstWaveCleared ? "cleared"
                 : (firstWaveEngaged ? "engaged" : "waiting"))
             << " berries=" << (homeBerriesDepleted ? "depleted" : "active")
             << " hunt=" << (huntFinished ? "finished"
                 : (huntAnchorValid ? "active" : "searching"))
             << " scout=" << armyScoutSN
             << " gold=" << (goldAnchorValid ? "found" : "searching")
             << " tech=" << broadswordResearch.state << '/'
             << towerUpgrade.state << '/' << goldMiningResearch.state << '/'
             << woodcuttingResearch.state << '/' << farmingResearch.state
             << " bb=" << workerBlackboard.intents.size() << '/'
             << workerLeases.size()
             << " blocked=" << blockedWorkerTasks;
        ai.DebugText(text.str());
    }

    void constructionSubscriber(const tagInfo& info, set<int>& reserved)
    {
        const bool filledExistingBuild = repairOrResume(info, reserved);
        if (!filledExistingBuild)
            constructBuildings(info, reserved);
    }

    void deliverySubscriber(const tagInfo& info, set<int>& reserved)
    {
        for (const tagFarmer& farmer : info.farmers) {
            if (farmer.FarmerSort != FARMERTYPE_FARMER
                || farmer.Resource <= 0
                || farmer.NowState != HUMAN_STATE_IDLE)
                continue;
            const tagBuilding* dropoff = nearestCarriedResourceDropoff(info, farmer);
            if (dropoff != NULL && workerLeaseBlocked(farmer.SN, dropoff->SN))
                dropoff = nearestCarriedResourceDropoff(info, farmer, dropoff->SN);
            if (dropoff == NULL)
                continue;
            publishWorkerAction(farmer.SN, dropoff->SN,
                                WORKER_TASK_DEPOSIT, 1000, TOPIC_DELIVERY);
            // Do not reserve here: other subscribers may still publish, and
            // the arbiter must demonstrate that delivery wins by priority.
        }
        (void)reserved;
    }

    void huntSubscriber(const tagInfo& info, set<int>& reserved)
    {
        manageHunt(info, reserved);
    }

    void farmSubscriber(const tagInfo& info, set<int>& reserved)
    {
        assignFarms(info, reserved);
    }

    void economySubscriber(const tagInfo& info, set<int>& reserved)
    {
        assignResources(info, reserved);
    }

    void publishWorkerSubscriptions()
    {
        if (workerBlackboard.snapshot == NULL)
            return;
        const tagInfo& info = *workerBlackboard.snapshot;
        typedef void (Impl::*Subscriber)(const tagInfo&, set<int>&);
        struct Subscription {
            int topic;
            Subscriber handler;
        };
        static const Subscription subscriptions[] = {
            { TOPIC_DELIVERY, &Impl::deliverySubscriber },
            { TOPIC_CONSTRUCTION, &Impl::constructionSubscriber },
            { TOPIC_HUNT, &Impl::huntSubscriber },
            { TOPIC_FARM, &Impl::farmSubscriber },
            { TOPIC_ECONOMY, &Impl::economySubscriber }
        };
        set<int> reserved;
        const int count = int(sizeof(subscriptions) / sizeof(subscriptions[0]));
        for (int i = 0; i < count; ++i) {
            if ((workerBlackboard.publishedTopics & subscriptions[i].topic) != 0)
                (this->*subscriptions[i].handler)(info, reserved);
        }
    }

    void process(UsrAI& ai, const tagInfo& info)
    {
        if (lastFrame >= 0 && info.GameFrame < lastFrame)
            reset();
        lastFrame = info.GameFrame;
        consumeResults(info);
        updateFoodPlan(info);
        updateWaveState(info);
        beginWorkerBlackboard(info);
        manageDefense(ai, info);
        manageTechnologies(ai, info);
        manageCenter(ai, info);
        publishWorkerSubscriptions();
        manageArmyProduction(ai, info);
        dispatchWorkerBlackboard(ai, info);
        telemetry(ai, info);
    }
};

UsrAIStrategy::UsrAIStrategy()
    : impl(new Impl)
{
}

UsrAIStrategy::~UsrAIStrategy()
{
    delete impl;
}

void UsrAIStrategy::process(UsrAI& ai, const tagInfo& info)
{
    impl->process(ai, info);
}
/* ============================== 主入口 ============================== */
void UsrAI::processData()
{
    static UsrAIStrategy strategy;
    const tagInfo info = getInfo();
    strategy.process(*this, info);
    clearInsRet();
}
