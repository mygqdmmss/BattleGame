#include <SFML/Graphics.hpp>
#include <vector>
#include <cmath>
#include <mutex>
#include <thread>
#include <iostream>
#include <random>
#include <atomic>
#include <queue>
#include <map>
#include <set>

// ==========================================
// 0. 基础配置与常量
// ==========================================
const int TILE_SIZE = 80; // 格子尺寸
const int GRID_WIDTH = 24; // 格子数量适配1920
const int GRID_HEIGHT = 12; // 适配1080p减去UI高度 (960/80 = 12)
const int UI_HEIGHT = 120; // UI区域高度 (1080 - 960 = 120)
const int WINDOW_WIDTH = GRID_WIDTH * TILE_SIZE;
const int WINDOW_HEIGHT = GRID_HEIGHT * TILE_SIZE + UI_HEIGHT;

enum EntityType { WARRIOR, ARCHER, TANK, BASE };
enum Faction { RED, BLUE }; // Red是电脑/敌方, Blue是玩家/我方
enum TileType { GROUND, MOUNTAIN, RIVER };

// 线程安全锁，用于保护共享数据（Model和View都会访问）
std::mutex gameMutex;

// ==========================================
// 1. Model (数据与逻辑层)
// ==========================================

// 士兵/单位基类
struct Unit {
    int id;
    Faction faction;
    EntityType type;
    float x, y;          // 坐标
    float speed;         // 移动速度
    int hp;              // 血量
    int maxHp;
    int attack;          // 攻击力
    float range;         // 攻击范围
    float sight;         // 视野范围
    int armor;           // 护甲
    bool isDead = false;
    
    // 目标（移动或攻击）
    float targetX, targetY;
    bool hasTarget = false;
    bool isManualControl = false; // 是否被人为控制
    int targetUnitId = -1; // 攻击目标的ID
    
    // 寻路缓存
    int nextPathX = -1;
    int nextPathY = -1;

    Unit(int _id, Faction _f, EntityType _t, float _x, float _y) 
        : id(_id), faction(_f), type(_t), x(_x), y(_y), targetX(_x), targetY(_y) {
        
        // 3. 初始化不同兵种属性
        if (type == BASE) {
            maxHp = hp = 5000; speed = 0; attack = 0; range = 0; armor = 20; sight = 2000;
        } else if (type == WARRIOR) {
            maxHp = hp = 600; speed = 2.5f; attack = 25; range = 2.0f * TILE_SIZE; armor = 10; sight = 5 * TILE_SIZE;
        } else if (type == ARCHER) {
            maxHp = hp = 400; speed = 3.0f; attack = 30; range = 2.5f * TILE_SIZE; armor = 0; sight = 7 * TILE_SIZE;
        } else if (type == TANK) {
            maxHp = hp = 1000; speed = 2.0f; attack = 40; range = 1.5f * TILE_SIZE; armor = 20; sight = 4 * TILE_SIZE;
        }
    }
};

struct VisualEffect {
    enum Type { ARROW, CANNON, SLASH, HEAL_RING, DAMAGE_RING };
    Type type;
    float x, y;
    float tx, ty;
    float speed;
    float life;     // 剩余寿命
    float maxLife;  // 总寿命
    bool active = true;
};

class GameModel {
public:
    TileType grid[GRID_WIDTH][GRID_HEIGHT];
    std::vector<Unit> units;
    std::vector<VisualEffect> effects; // 新增特效列表
    std::atomic<bool> gameOver{false};
    std::string winnerMessage;
    int unitIdCounter = 0;

    // 时间与速度控制
    float gameTime = 0.0f;
    float gameSpeed = 1.0f;
    bool isPaused = false;

    // 能量系统
    double blueEnergy = 2.0; // 初始给一点能量方便测试
    double redEnergy = 2.0;
    const double MAX_ENERGY = 10.0;
    std::chrono::steady_clock::time_point lastEnergyTime;

    struct Point {
        int x, y;
        bool operator<(const Point& other) const {
            if (x != other.x) return x < other.x;
            return y < other.y;
        }
        bool operator==(const Point& other) const {
            return x == other.x && y == other.y;
        }
    };

    GameModel() {
        initMap();
        // 2. 初始化基地
        spawnUnit(BLUE, BASE, 2, 2); // 我方基地
        spawnUnit(RED, BASE, GRID_WIDTH - 3, GRID_HEIGHT - 3); // 敌方基地
        lastEnergyTime = std::chrono::steady_clock::now();
    }

    // 消耗能量生成单位
    bool trySpawnUnit(Faction f, EntityType type) {
        std::lock_guard<std::mutex> lock(gameMutex);
        double cost = 0;
        if (type == WARRIOR) cost = 3;
        else if (type == ARCHER) cost = 4;
        else if (type == TANK) cost = 5;

        double& energy = (f == BLUE) ? blueEnergy : redEnergy;
        if (energy >= cost) {
            energy -= cost;
            // 在基地附近生成
            float baseX = (f == BLUE) ? 2 * TILE_SIZE : (GRID_WIDTH - 3) * TILE_SIZE;
            float baseY = (f == BLUE) ? 2 * TILE_SIZE : (GRID_HEIGHT - 3) * TILE_SIZE;
            // 简单的随机偏移防止重叠，并加上中心偏移
            float spawnX = baseX + (rand() % 3 - 1) * TILE_SIZE + TILE_SIZE/2.0f;
            float spawnY = baseY + (rand() % 3 - 1) * TILE_SIZE + TILE_SIZE/2.0f;
            
            units.emplace_back(unitIdCounter++, f, type, spawnX, spawnY);
            return true;
        }
        return false;
    }

    // 释放技能
    bool castSkill(Faction f, std::string skillName, float x, float y) {
        std::lock_guard<std::mutex> lock(gameMutex);
        double& energy = (f == BLUE) ? blueEnergy : redEnergy;
        double cost = 6.0; // 技能统一消耗6点
        
        if (energy >= cost) {
            energy -= cost;
            float radius = 3.0f * TILE_SIZE; // 技能半径
            
            // 添加技能特效
            VisualEffect eff;
            eff.x = x; eff.y = y;
            eff.life = eff.maxLife = 0.5f;
            eff.type = (skillName == "HEAL") ? VisualEffect::HEAL_RING : VisualEffect::DAMAGE_RING;
            effects.push_back(eff);

            for (auto& u : units) {
                if (getDistance(u.x, u.y, x, y) <= radius) {
                    if (skillName == "HEAL" && u.faction == f) {
                        u.hp = std::min(u.maxHp, u.hp + 300); // 治疗300
                    } else if (skillName == "DAMAGE" && u.faction != f) {
                        u.hp -= 300; // 伤害300
                        if (u.hp <= 0) u.isDead = true;
                    }
                }
            }
            return true;
        }
        return false;
    }

    void initMap() {
        // 1. 全图初始化为障碍 (山)
        for (int i = 0; i < GRID_WIDTH; i++) {
            for (int j = 0; j < GRID_HEIGHT; j++) {
                grid[i][j] = MOUNTAIN;
            }
        }

        // 2. 迷宫生成 (递归回溯法 / DFS)
        std::vector<Point> stack;
        Point start = {1, 1};
        grid[start.x][start.y] = GROUND;
        stack.push_back(start);

        // 随机数生成器
        std::random_device rd;
        std::mt19937 g(rd());

        while (!stack.empty()) {
            Point current = stack.back();
            std::vector<int> neighbors;
            
            // 检查四个方向 (步长为2)
            int dx[] = {0, 0, 2, -2};
            int dy[] = {2, -2, 0, 0};
            
            for (int i = 0; i < 4; i++) {
                int nx = current.x + dx[i];
                int ny = current.y + dy[i];
                
                // 确保在范围内且是墙(未访问过)
                // 保留边界为墙
                if (nx > 0 && nx < GRID_WIDTH - 1 && ny > 0 && ny < GRID_HEIGHT - 1) {
                    if (grid[nx][ny] == MOUNTAIN) {
                        neighbors.push_back(i);
                    }
                }
            }

            if (!neighbors.empty()) {
                // 随机选择一个邻居
                std::uniform_int_distribution<> dis(0, neighbors.size() - 1);
                int dir = neighbors[dis(g)];
                
                int nx = current.x + dx[dir];
                int ny = current.y + dy[dir];
                
                // 打通中间的墙
                grid[current.x + dx[dir]/2][current.y + dy[dir]/2] = GROUND;
                // 打通目标点
                grid[nx][ny] = GROUND;
                
                stack.push_back({nx, ny});
            } else {
                stack.pop_back();
            }
        }

        // 3. 确保基地周围空旷
        auto clearArea = [&](int cx, int cy, int radius) {
            for (int i = cx - radius; i <= cx + radius; i++) {
                for (int j = cy - radius; j <= cy + radius; j++) {
                    if (i >= 0 && i < GRID_WIDTH && j >= 0 && j < GRID_HEIGHT) {
                        grid[i][j] = GROUND;
                    }
                }
            }
        };
        
        clearArea(2, 2, 2); // 蓝方基地周围
        clearArea(GRID_WIDTH - 3, GRID_HEIGHT - 3, 2); // 红方基地周围

        // 4. 随机打通一些墙壁，形成环路 (Braiding)，避免死胡同太多
        for (int i = 1; i < GRID_WIDTH - 1; i++) {
            for (int j = 1; j < GRID_HEIGHT - 1; j++) {
                if (grid[i][j] == MOUNTAIN) {
                    // 20%概率移除墙壁
                    if (rand() % 5 == 0) {
                        grid[i][j] = GROUND;
                    }
                }
            }
        }

        // 5. 将部分剩余的山脉转换为河流，增加多样性
        for (int i = 0; i < GRID_WIDTH; i++) {
            for (int j = 0; j < GRID_HEIGHT; j++) {
                if (grid[i][j] == MOUNTAIN) {
                    if (rand() % 3 == 0) {
                        grid[i][j] = RIVER;
                    }
                }
            }
        }
    }

    void spawnUnit(Faction f, EntityType t, float gx, float gy) {
        std::lock_guard<std::mutex> lock(gameMutex);
        // 使用格子中心坐标，确保单位在格子中间
        units.emplace_back(unitIdCounter++, f, t, gx * TILE_SIZE + TILE_SIZE/2.0f, gy * TILE_SIZE + TILE_SIZE/2.0f);
    }

    // 8. 决策能力的核心 (简单的状态机)
    void updateLogic() {
        gameTime += 0.033f; // 累加游戏时间

        // 能量恢复逻辑
        // 为了简单，我们在大锁里做
        {
            std::lock_guard<std::mutex> lock(gameMutex);
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastEnergyTime).count() >= 1500) {
                if (blueEnergy < MAX_ENERGY) blueEnergy += 1;
                if (redEnergy < MAX_ENERGY) redEnergy += 1;
                lastEnergyTime = now;
                
                // 敌方AI造兵逻辑 (每秒检查一次)
                if (redEnergy >= 5) {
                     // 简单的AI：能量够了就造
                     EntityType type = WARRIOR;
                     double cost = 3;
                     int r = rand() % 10;
                     if (r < 4) { type = WARRIOR; cost = 3; }
                     else if (r < 7) { type = ARCHER; cost = 4; }
                     else { type = TANK; cost = 5; }

                     if (redEnergy >= cost) {
                         redEnergy -= cost;
                         float baseX = (GRID_WIDTH - 3) * TILE_SIZE;
                         float baseY = (GRID_HEIGHT - 3) * TILE_SIZE;
                         float spawnX = baseX + (rand() % 3 - 1) * TILE_SIZE + TILE_SIZE/2.0f;
                         float spawnY = baseY + (rand() % 3 - 1) * TILE_SIZE + TILE_SIZE/2.0f;
                         units.emplace_back(unitIdCounter++, RED, type, spawnX, spawnY);
                     }
                }
            }
        }

        
        std::lock_guard<std::mutex> lock(gameMutex);
        
        // 检查胜利条件
        bool blueBaseAlive = false;
        bool redBaseAlive = false;

        for (auto& u : units) {
            if (u.isDead) continue;
            if (u.type == BASE) {
                if (u.faction == BLUE) blueBaseAlive = true;
                else redBaseAlive = true;
                
                // 移除旧的自动造兵逻辑
            }

            // 士兵AI
            if (u.type != BASE) {
                Unit* target = findNearestEnemy(u);
                bool isAttacking = false;
                
                // 决策：有敌人在射程内 -> 攻击 (最高优先级)
                if (target && getDistance(u, *target) <= u.range) {
                    // 添加攻击特效
                    VisualEffect eff;
                    eff.x = u.x; eff.y = u.y;
                    eff.tx = target->x; eff.ty = target->y;
                    eff.maxLife = eff.life = 0.5f; 
                    
                    if(u.type == WARRIOR) {
                        eff.type = VisualEffect::SLASH;
                        eff.x = target->x; eff.y = target->y; // 砍击在目标身上
                        eff.life = 0.2f;
                    } else if(u.type == ARCHER) {
                        eff.type = VisualEffect::ARROW;
                        eff.speed = 25.0f; 
                        eff.life = 1.0f;
                    } else if(u.type == TANK) {
                        eff.type = VisualEffect::CANNON;
                        eff.speed = 20.0f;
                        eff.life = 1.0f;
                    } else {
                        eff.active = false;
                    }
                    if(eff.active) effects.push_back(eff);

                    target->hp -= std::max(1, u.attack - target->armor); // 伤害计算
                    if (target->hp <= 0) target->isDead = true;
                    isAttacking = true;
                }

                // 如果正在攻击，通常停止移动
                if (isAttacking) continue;

                // 移动逻辑
                if (u.isManualControl) {
                    // 人为控制模式：只听指令
                    if (u.hasTarget) {
                        moveTowards(u, u.targetX, u.targetY);
                        // 到达目的地停止
                        if (getDistance(u.x, u.y, u.targetX, u.targetY) < 5.0f) u.hasTarget = false;
                    }
                } else {
                    // 自主模式
                    if (target && getDistance(u, *target) <= u.sight) {
                        // 决策：有敌人在视野内 -> 追击
                        moveTowards(u, target->x, target->y);
                    } else {
                        // 决策：无敌人 -> 进攻敌方基地
                        float enemyBaseX = (u.faction == BLUE) ? (GRID_WIDTH - 3) * TILE_SIZE : 2 * TILE_SIZE;
                        float enemyBaseY = (u.faction == BLUE) ? (GRID_HEIGHT - 3) * TILE_SIZE : 2 * TILE_SIZE;
                        moveTowards(u, enemyBaseX, enemyBaseY);
                    }
                }
            }
        }

        // 清理尸体
        units.erase(std::remove_if(units.begin(), units.end(), [](const Unit& u){ return u.isDead; }), units.end());

        // 更新特效
        for(auto& eff : effects) {
            eff.life -= 0.033f; 
            if(eff.life <= 0) {
                eff.active = false;
                continue;
            }
            if(eff.type == VisualEffect::ARROW || eff.type == VisualEffect::CANNON) {
                float dx = eff.tx - eff.x;
                float dy = eff.ty - eff.y;
                float dist = std::sqrt(dx*dx + dy*dy);
                if(dist > eff.speed) {
                    eff.x += (dx/dist) * eff.speed;
                    eff.y += (dy/dist) * eff.speed;
                } else {
                    eff.x = eff.tx; eff.y = eff.ty;
                    eff.active = false; 
                }
            }
        }
        effects.erase(std::remove_if(effects.begin(), effects.end(), [](const VisualEffect& e){ return !e.active; }), effects.end());

        // 4. 胜利判定
        if (!blueBaseAlive && !gameOver) { gameOver = true; winnerMessage = "RED WINS!"; }
        if (!redBaseAlive && !gameOver) { gameOver = true; winnerMessage = "BLUE WINS!"; }
    }

private:
    float getDistance(float x1, float y1, float x2, float y2) {
        return std::sqrt(std::pow(x2 - x1, 2) + std::pow(y2 - y1, 2));
    }

    float getDistance(const Unit& a, const Unit& b) {
        return getDistance(a.x, a.y, b.x, b.y);
    }

    Unit* findNearestEnemy(Unit& me) {
        Unit* nearest = nullptr;
        float minDist = 99999.0f;
        for (auto& other : units) {
            if (other.faction != me.faction && !other.isDead) {
                float d = getDistance(me, other);
                if (d < minDist && d <= me.sight) {
                    minDist = d;
                    nearest = &other;
                }
            }
        }
        return nearest;
    }

    // 1. 移动逻辑（使用A*寻路算法实现全图避障与平滑移动）
    void moveTowards(Unit& u, float tx, float ty) {
        int startX = (int)(u.x / TILE_SIZE);
        int startY = (int)(u.y / TILE_SIZE);
        int endX = (int)(tx / TILE_SIZE);
        int endY = (int)(ty / TILE_SIZE);

        // 边界检查
        startX = std::max(0, std::min(GRID_WIDTH - 1, startX));
        startY = std::max(0, std::min(GRID_HEIGHT - 1, startY));
        endX = std::max(0, std::min(GRID_WIDTH - 1, endX));
        endY = std::max(0, std::min(GRID_HEIGHT - 1, endY));

        // 如果已经在目标格子，直接微调位置
        if (startX == endX && startY == endY) {
            float dx = tx - u.x;
            float dy = ty - u.y;
            float dist = std::sqrt(dx*dx + dy*dy);
            if (dist > u.speed) {
                u.x += (dx/dist) * u.speed;
                u.y += (dy/dist) * u.speed;
            } else {
                u.x = tx;
                u.y = ty;
            }
            u.nextPathX = -1; // 清除缓存
            return;
        }

        // 检查缓存的下一步是否有效
        if (u.nextPathX != -1) {
            // 如果目标变了（比如人为点击了新地方），或者已经到达了缓存点，就需要重新计算
            // 这里简单判断：如果已经到达缓存点中心附近，则视为完成这一步
            float centerX = u.nextPathX * TILE_SIZE + TILE_SIZE / 2.0f;
            float centerY = u.nextPathY * TILE_SIZE + TILE_SIZE / 2.0f;
            if (getDistance(u.x, u.y, centerX, centerY) < u.speed) {
                u.x = centerX;
                u.y = centerY;
                u.nextPathX = -1; // 需要计算下一步
            }
        }

        // 如果没有缓存的下一步，计算它
        if (u.nextPathX == -1) {
            Point nextTile = getNextMove(startX, startY, endX, endY);
            u.nextPathX = nextTile.x;
            u.nextPathY = nextTile.y;
        }
        
        // 目标是下一个格子的中心
        float targetWorldX = u.nextPathX * TILE_SIZE + TILE_SIZE / 2.0f;
        float targetWorldY = u.nextPathY * TILE_SIZE + TILE_SIZE / 2.0f;

        // 平滑移动向下一个格子中心
        float dx = targetWorldX - u.x;
        float dy = targetWorldY - u.y;
        float dist = std::sqrt(dx*dx + dy*dy);

        if (dist > 0) {
            float vx = (dx / dist) * u.speed;
            float vy = (dy / dist) * u.speed;
            u.x += vx;
            u.y += vy;
        }
    }

    // A* 寻路算法 (优化版：使用数组代替Map)
    Point getNextMove(int startX, int startY, int endX, int endY) {
        if (grid[endX][endY] == MOUNTAIN || grid[endX][endY] == RIVER) {
            return {startX, startY}; 
        }

        // 使用静态数组避免频繁内存分配
        static int cost_so_far[GRID_WIDTH][GRID_HEIGHT];
        static Point came_from[GRID_WIDTH][GRID_HEIGHT];
        static int visitedToken[GRID_WIDTH][GRID_HEIGHT]; // 用于标记是否访问过，避免每次memset
        static int token = 0;
        
        token++; // 每次调用增加token，相当于清空数组

        std::priority_queue<std::pair<int, Point>, std::vector<std::pair<int, Point>>, std::greater<std::pair<int, Point>>> frontier;

        frontier.push({0, {startX, startY}});
        came_from[startX][startY] = {startX, startY};
        cost_so_far[startX][startY] = 0;
        visitedToken[startX][startY] = token;

        Point current;
        bool found = false;

        while (!frontier.empty()) {
            current = frontier.top().second;
            frontier.pop();

            if (current.x == endX && current.y == endY) {
                found = true;
                break;
            }

            int dx[] = {0, 0, 1, -1};
            int dy[] = {1, -1, 0, 0};
            
            for (int i = 0; i < 4; i++) {
                Point next = {current.x + dx[i], current.y + dy[i]};
                
                if (next.x >= 0 && next.x < GRID_WIDTH && next.y >= 0 && next.y < GRID_HEIGHT) {
                    if (grid[next.x][next.y] != MOUNTAIN && grid[next.x][next.y] != RIVER) {
                        int new_cost = cost_so_far[current.x][current.y] + 1;
                        
                        // 如果未访问过 或者 找到了更短路径
                        if (visitedToken[next.x][next.y] != token || new_cost < cost_so_far[next.x][next.y]) {
                            cost_so_far[next.x][next.y] = new_cost;
                            visitedToken[next.x][next.y] = token;
                            int priority = new_cost + std::abs(endX - next.x) + std::abs(endY - next.y);
                            frontier.push({priority, next});
                            came_from[next.x][next.y] = current;
                        }
                    }
                }
            }
        }

        if (!found) return {startX, startY};

        Point curr = {endX, endY};
        // 防止死循环回溯
        int safety = 0;
        while (!(came_from[curr.x][curr.y] == Point{startX, startY}) && !(curr == Point{startX, startY})) {
            curr = came_from[curr.x][curr.y];
            if(++safety > 1000) break;
        }
        return curr;
    }
};

// ==========================================
// 2. View (视图层)
// ==========================================
class GameView {
    sf::RenderWindow& window;
    std::map<std::string, sf::Texture> textures;
    sf::Font font;

public:
    GameView(sf::RenderWindow& win) : window(win) {
        // 加载字体 (Windows默认路径)
        if (!font.loadFromFile("C:/Windows/Fonts/arial.ttf")) {
            // 如果加载失败，可以尝试其他路径或忽略
            std::cout << "Failed to load font!" << std::endl;
        }

        // 加载纹理，将图片放入 assets 文件夹
        loadTexture("victory", "assets/victory.png");
        loadTexture("defeat", "assets/defeat.png");
        loadTexture("ground", "assets/ground.png");
        loadTexture("mountain", "assets/mountain.png");
        loadTexture("river", "assets/river.png");
        
        loadTexture("warrior_blue", "assets/warrior_blue.png");
        loadTexture("warrior_red", "assets/warrior_red.png");
        loadTexture("archer_blue", "assets/archer_blue.png");
        loadTexture("archer_red", "assets/archer_red.png");
        loadTexture("tank_blue", "assets/tank_blue.png");
        loadTexture("tank_red", "assets/tank_red.png");
        loadTexture("base_blue", "assets/base_blue.png");
        loadTexture("base_red", "assets/base_red.png");
        
        // UI图标
        loadTexture("icon_warrior", "assets/icon_warrior.png");
        loadTexture("icon_archer", "assets/icon_archer.png");
        loadTexture("icon_tank", "assets/icon_tank.png");
        loadTexture("icon_heal", "assets/icon_heal.png");
        loadTexture("icon_damage", "assets/icon_damage.png");
    }

    void loadTexture(const std::string& name, const std::string& path) {
        sf::Texture tex;
        if (tex.loadFromFile(path)) {
            textures[name] = tex;
        } else {
            // std::cout << "Failed to load texture: " << path << " (Using fallback)" << std::endl;
            // 创建一个默认的纯色纹理作为后备
            // tex.create(40, 40);
            // textures[name] = tex;
        }
    }

    sf::Texture* getTexture(const std::string& name) {
        if (textures.find(name) != textures.end()) {
            return &textures[name];
        }
        return nullptr;
    }

    void draw(GameModel& model) {
        window.clear(sf::Color(50, 50, 50)); // 深灰色背景

        // 绘制地图
        for (int i = 0; i < GRID_WIDTH; i++) {
            for (int j = 0; j < GRID_HEIGHT; j++) {
                sf::Sprite sprite;
                sprite.setPosition(i * TILE_SIZE, j * TILE_SIZE);
                
                std::string texName = "ground";
                if (model.grid[i][j] == MOUNTAIN) texName = "mountain";
                else if (model.grid[i][j] == RIVER) texName = "river";
                
                if (auto* tex = getTexture(texName)) {
                    sprite.setTexture(*tex);
                    // 缩放以适应格子大小
                    sf::Vector2u size = tex->getSize();
                    sprite.setScale((float)TILE_SIZE / size.x, (float)TILE_SIZE / size.y);
                    window.draw(sprite);
                } else {
                    // Fallback shape
                    sf::RectangleShape tile(sf::Vector2f(TILE_SIZE - 2, TILE_SIZE - 2));
                    tile.setPosition(i * TILE_SIZE, j * TILE_SIZE);
                    if (model.grid[i][j] == MOUNTAIN) tile.setFillColor(sf::Color(100, 100, 100));
                    else if (model.grid[i][j] == RIVER) tile.setFillColor(sf::Color(0, 100, 255));
                    else tile.setFillColor(sf::Color(34, 139, 34));
                    window.draw(tile);
                }
            }
        }

        // 绘制单位
        {
            std::lock_guard<std::mutex> lock(gameMutex); // 渲染时也要加锁
            for (const auto& u : model.units) {
                sf::Sprite sprite;
                std::string texName;
                
                if (u.type == BASE) texName = (u.faction == BLUE) ? "base_blue" : "base_red";
                else if (u.type == WARRIOR) texName = (u.faction == BLUE) ? "warrior_blue" : "warrior_red";
                else if (u.type == ARCHER) texName = (u.faction == BLUE) ? "archer_blue" : "archer_red";
                else if (u.type == TANK) texName = (u.faction == BLUE) ? "tank_blue" : "tank_red";

            // 绘制特效
            for(const auto& eff : model.effects) {
                if(eff.type == VisualEffect::HEAL_RING) {
                    sf::CircleShape ring(3.0f * TILE_SIZE * (1.0f - eff.life/eff.maxLife));
                    ring.setOrigin(ring.getRadius(), ring.getRadius());
                    ring.setPosition(eff.x, eff.y);
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineColor(sf::Color(0, 255, 0, 200 * (eff.life/eff.maxLife)));
                    ring.setOutlineThickness(5);
                    window.draw(ring);
                } else if(eff.type == VisualEffect::DAMAGE_RING) {
                    sf::CircleShape ring(3.0f * TILE_SIZE * (1.0f - eff.life/eff.maxLife));
                    ring.setOrigin(ring.getRadius(), ring.getRadius());
                    ring.setPosition(eff.x, eff.y);
                    ring.setFillColor(sf::Color::Transparent);
                    ring.setOutlineColor(sf::Color(255, 0, 0, 200 * (eff.life/eff.maxLife)));
                    ring.setOutlineThickness(5);
                    window.draw(ring);
                } else if(eff.type == VisualEffect::SLASH) {
                    sf::RectangleShape line(sf::Vector2f(60, 8));
                    line.setOrigin(30, 4);
                    line.setPosition(eff.x, eff.y);
                    line.setFillColor(sf::Color::White);
                    line.setRotation(rand() % 360); 
                    window.draw(line);
                } else if(eff.type == VisualEffect::ARROW) {
                    sf::RectangleShape line(sf::Vector2f(40, 5));
                    line.setOrigin(20, 2.5f);
                    line.setPosition(eff.x, eff.y);
                    line.setFillColor(sf::Color::Yellow);
                    float dx = eff.tx - eff.x;
                    float dy = eff.ty - eff.y;
                    float angle = std::atan2(dy, dx) * 180 / 3.14159f;
                    line.setRotation(angle);
                    window.draw(line);
                } else if(eff.type == VisualEffect::CANNON) {
                    sf::CircleShape ball(10);
                    ball.setOrigin(10, 10);
                    ball.setPosition(eff.x, eff.y);
                    ball.setFillColor(sf::Color::Black);
                    window.draw(ball);
                }
            }

                if (auto* tex = getTexture(texName)) {
                    sprite.setTexture(*tex);
                    sf::Vector2u size = tex->getSize();
                    sprite.setOrigin(size.x / 2.0f, size.y / 2.0f);
                    sprite.setPosition(u.x, u.y);
                    
                    float targetSize = (u.type == BASE) ? TILE_SIZE : (TILE_SIZE * 0.8f);
                    sprite.setScale(targetSize / size.x, targetSize / size.y);
                    
                    window.draw(sprite);
                } else {
                    // Fallback shape
                    sf::Shape* shape;
                    sf::CircleShape circle(15);
                    sf::RectangleShape rect(sf::Vector2f(30, 30));
                    
                    if (u.type == BASE) {
                        rect.setSize(sf::Vector2f(TILE_SIZE, TILE_SIZE));
                        rect.setOrigin(TILE_SIZE/2.0f, TILE_SIZE/2.0f);
                        shape = &rect;
                    } else {
                        circle.setOrigin(15, 15);
                        shape = &circle;
                    }

                    shape->setPosition(u.x, u.y);
                    
                    // 阵营颜色
                    if (u.faction == BLUE) shape->setFillColor(sf::Color::Cyan);
                    else shape->setFillColor(sf::Color::Red);

                    // 兵种区分 (通过轮廓颜色)
                    if (u.type == TANK) shape->setOutlineThickness(3);
                    
                    window.draw(*shape);
                }

                // 绘制血条 (简单红线)
                sf::RectangleShape hpBar(sf::Vector2f(30 * ((float)u.hp / u.maxHp), 5));
                hpBar.setOrigin(15, 0); // Center horizontally
                hpBar.setPosition(u.x, u.y - 20);
                hpBar.setFillColor(sf::Color::Green);
                window.draw(hpBar);
            }
            
            // 绘制UI面板
            sf::RectangleShape uiPanel(sf::Vector2f(WINDOW_WIDTH, UI_HEIGHT));
            uiPanel.setPosition(0, GRID_HEIGHT * TILE_SIZE);
            uiPanel.setFillColor(sf::Color(30, 30, 30));
            window.draw(uiPanel);

            // UI Buttons
            struct Btn { float x; std::string name; sf::Color fallback; int cost; };
            Btn btns[] = { 
                {10, "icon_warrior", sf::Color::Red, 3}, 
                {100, "icon_archer", sf::Color::Green, 4}, 
                {190, "icon_tank", sf::Color::Blue, 5}, 
                {280, "icon_heal", sf::Color::Yellow, 6}, 
                {370, "icon_damage", sf::Color::Magenta, 6} 
            };
            
            for(int i=0; i<5; ++i) {
                sf::Sprite btnSprite;
                btnSprite.setPosition(btns[i].x, GRID_HEIGHT * TILE_SIZE + 10);
                
                if (auto* tex = getTexture(btns[i].name)) {
                    btnSprite.setTexture(*tex);
                    sf::Vector2u size = tex->getSize();
                    btnSprite.setScale(80.0f / size.x, 80.0f / size.y);
                    window.draw(btnSprite);
                } else {
                    sf::RectangleShape btn(sf::Vector2f(80, 80));
                    btn.setPosition(btns[i].x, GRID_HEIGHT * TILE_SIZE + 10);
                    btn.setFillColor(btns[i].fallback);
                    window.draw(btn);
                }

                // 绘制消耗数值
                sf::Text costText;
                costText.setFont(font);
                costText.setString(std::to_string(btns[i].cost));
                costText.setCharacterSize(20);
                costText.setFillColor(sf::Color::Yellow);
                costText.setOutlineColor(sf::Color::Black);
                costText.setOutlineThickness(2);
                // 右下角显示
                costText.setPosition(btns[i].x + 60, GRID_HEIGHT * TILE_SIZE + 65);
                window.draw(costText);
            }

            // 绘制能量条
            float energyRatio = model.blueEnergy / model.MAX_ENERGY;
            float barWidth = 350.0f; // 增加长度
            sf::RectangleShape energyBar(sf::Vector2f(barWidth * energyRatio, 20));
            energyBar.setPosition(500, GRID_HEIGHT * TILE_SIZE + 40);
            energyBar.setFillColor(sf::Color(255, 215, 0)); // 金色 (Gold)
            window.draw(energyBar);

            // 绘制能量条背景框
            sf::RectangleShape energyBarBg(sf::Vector2f(barWidth, 20));
            energyBarBg.setPosition(500, GRID_HEIGHT * TILE_SIZE + 40);
            energyBarBg.setFillColor(sf::Color::Transparent);
            energyBarBg.setOutlineColor(sf::Color::White);
            energyBarBg.setOutlineThickness(2);
            window.draw(energyBarBg);

            // 绘制能量格子线
            for (int i = 1; i < model.MAX_ENERGY; ++i) {
                sf::RectangleShape line(sf::Vector2f(2, 20));
                line.setPosition(500 + i * (barWidth / model.MAX_ENERGY), GRID_HEIGHT * TILE_SIZE + 40);
                line.setFillColor(sf::Color(0, 0, 0, 100)); // 半透明黑色分割线
                window.draw(line);
            }

            // 绘制能量数值
            sf::Text energyText;
            energyText.setFont(font);
            energyText.setCharacterSize(18);
            energyText.setFillColor(sf::Color::White);
            std::string energyStr = std::to_string((int)model.blueEnergy) + "/" + std::to_string((int)model.MAX_ENERGY);
            energyText.setString(energyStr);
            energyText.setPosition(500 + barWidth + 10, GRID_HEIGHT * TILE_SIZE + 40);
            window.draw(energyText);

            // 绘制时间
            sf::Text timeText;
            timeText.setFont(font);
            timeText.setCharacterSize(24);
            timeText.setFillColor(sf::Color::White);
            int minutes = (int)model.gameTime / 60;
            int seconds = (int)model.gameTime % 60;
            std::string timeStr = "Time: " + (minutes < 10 ? std::string("0") : "") + std::to_string(minutes) + ":" + (seconds < 10 ? std::string("0") : "") + std::to_string(seconds);
            timeText.setString(timeStr);
            timeText.setPosition(950, GRID_HEIGHT * TILE_SIZE + 40);
            window.draw(timeText);

            // 绘制速度控制按钮
            struct SpeedBtn { float x; std::string label; float speed; };
            SpeedBtn spdBtns[] = {
                {1150, "||", 0.0f},
                {1210, "0.5x", 0.5f},
                {1270, "1x", 1.0f},
                {1330, "2x", 2.0f},
                {1390, "4x", 4.0f}
            };

            for(const auto& btn : spdBtns) {
                sf::RectangleShape rect(sf::Vector2f(50, 30));
                rect.setPosition(btn.x, GRID_HEIGHT * TILE_SIZE + 40);
                if (model.isPaused && btn.speed == 0.0f) rect.setFillColor(sf::Color::Green);
                else if (!model.isPaused && model.gameSpeed == btn.speed && btn.speed != 0.0f) rect.setFillColor(sf::Color::Green);
                else rect.setFillColor(sf::Color(100, 100, 100));
                window.draw(rect);

                sf::Text btnText;
                btnText.setFont(font);
                btnText.setString(btn.label);
                btnText.setCharacterSize(18);
                btnText.setFillColor(sf::Color::White);
                btnText.setPosition(btn.x + 10, GRID_HEIGHT * TILE_SIZE + 42);
                window.draw(btnText);
            }

            // 绘制游戏结束画面
            if (model.gameOver) {
                sf::RectangleShape overlay(sf::Vector2f(WINDOW_WIDTH, WINDOW_HEIGHT));
                overlay.setFillColor(sf::Color(0, 0, 0, 150));
                window.draw(overlay);

                std::string texName = (model.winnerMessage == "BLUE WINS!") ? "victory" : "defeat";
                if (auto* tex = getTexture(texName)) {
                    sf::Sprite sprite;
                    sprite.setTexture(*tex);
                    sf::Vector2u size = tex->getSize();
                    sprite.setOrigin(size.x / 2.0f, size.y / 2.0f);
                    sprite.setPosition(WINDOW_WIDTH / 2.0f, WINDOW_HEIGHT / 2.0f);
                    window.draw(sprite);
                }
            }
            window.draw(energyBar);
        }
        window.display();
    }
};

// ==========================================
// 3. Controller (控制层)
// ==========================================
class GameController {
    GameModel& model;
    int selectedUnitId = -1; // 使用ID代替指针
    std::string selectedSkill = ""; // 当前选中的技能

public:
    GameController(GameModel& m) : model(m) {}

    void handleInput(sf::Event& event, sf::RenderWindow& window) {
        // 7. 人为操控 (点击选择我方单位，右键移动)
        if (event.type == sf::Event::MouseButtonPressed) {
            sf::Vector2i pixelPos = sf::Mouse::getPosition(window);
            float mx = (float)pixelPos.x;
            float my = (float)pixelPos.y;

            // 检查是否点击了UI区域
            if (my > GRID_HEIGHT * TILE_SIZE) {
                if (event.mouseButton.button == sf::Mouse::Left) {
                    // 简单的按钮点击检测
                    if (mx >= 10 && mx <= 90) model.trySpawnUnit(BLUE, WARRIOR);
                    else if (mx >= 100 && mx <= 180) model.trySpawnUnit(BLUE, ARCHER);
                    else if (mx >= 190 && mx <= 270) model.trySpawnUnit(BLUE, TANK);
                    else if (mx >= 280 && mx <= 360) { selectedSkill = "HEAL"; std::cout << "Skill Selected: HEAL" << std::endl; }
                    else if (mx >= 370 && mx <= 450) { selectedSkill = "DAMAGE"; std::cout << "Skill Selected: DAMAGE" << std::endl; }
                    
                    // Speed controls
                    if (my > GRID_HEIGHT * TILE_SIZE + 40 && my < GRID_HEIGHT * TILE_SIZE + 70) {
                        if (mx >= 1150 && mx <= 1200) { model.isPaused = true; }
                        else if (mx >= 1210 && mx <= 1260) { model.isPaused = false; model.gameSpeed = 0.5f; }
                        else if (mx >= 1270 && mx <= 1320) { model.isPaused = false; model.gameSpeed = 1.0f; }
                        else if (mx >= 1330 && mx <= 1380) { model.isPaused = false; model.gameSpeed = 2.0f; }
                        else if (mx >= 1390 && mx <= 1440) { model.isPaused = false; model.gameSpeed = 4.0f; }
                    }
                }
                return;
            }

            // 地图区域点击
            if (event.mouseButton.button == sf::Mouse::Left) {
                if (!selectedSkill.empty()) {
                    // 释放技能
                    if (model.castSkill(BLUE, selectedSkill, mx, my)) {
                        std::cout << "Skill Casted!" << std::endl;
                    } else {
                        std::cout << "Not enough energy!" << std::endl;
                    }
                    selectedSkill = ""; // 重置技能选择
                } else {
                    // 选择单位
                    selectedUnitId = -1;
                    std::lock_guard<std::mutex> lock(gameMutex);
                    for (auto& u : model.units) {
                        if (u.faction == BLUE && std::abs(u.x - mx) < 20 && std::abs(u.y - my) < 20) {
                            selectedUnitId = u.id;
                            std::cout << "Unit Selected: " << u.id << std::endl;
                            break;
                        }
                    }
                }
            } else if (event.mouseButton.button == sf::Mouse::Right) {
                // 取消技能选择
                if (!selectedSkill.empty()) {
                    selectedSkill = "";
                    std::cout << "Skill Cancelled" << std::endl;
                    return;
                }

                // 移动指令
                if (selectedUnitId != -1) {
                    std::lock_guard<std::mutex> lock(gameMutex);
                    // 查找ID对应的单位
                    Unit* selectedUnit = nullptr;
                    for (auto& u : model.units) {
                        if (u.id == selectedUnitId) {
                            selectedUnit = &u;
                            break;
                        }
                    }

                    if (selectedUnit) {
                        selectedUnit->targetX = mx;
                        selectedUnit->targetY = my;
                        selectedUnit->hasTarget = true;
                        selectedUnit->isManualControl = true; // 标记为人为控制
                        selectedUnit->nextPathX = -1; // 目标改变，重置路径缓存
                        std::cout << "Move Order: " << mx << "," << my << std::endl;
                    } else {
                        // 单位可能已死亡或消失
                        selectedUnitId = -1;
                    }
                }
            }
        }
    }
};

// ==========================================
// Main & 线程管理
// ==========================================
int main() {
    // 5. 图形化显示技术 (SFML窗口)
    // 使用全屏模式 (1920x1080)
    sf::RenderWindow window(sf::VideoMode(WINDOW_WIDTH, WINDOW_HEIGHT), "Let's Battle!!!");
    window.setFramerateLimit(60);

    GameModel model;
    GameView view(window);
    GameController controller(model);

    // 6. 多线程技术
    // 启动逻辑线程：负责AI计算、位置更新、状态判定
    // 这与主线程（负责渲染和输入）分离
    bool appRunning = true;
    std::thread logicThread([&]() {
        while (appRunning) {
            if (!model.gameOver && !model.isPaused) {
                model.updateLogic();
            }
            // 模拟计算负载，控制逻辑帧率 (约30FPS)
            // 动态调整休眠时间
            int sleepTime = 33;
            if (!model.isPaused && model.gameSpeed > 0) {
                sleepTime = (int)(33 / model.gameSpeed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleepTime));
        }
    });

    // 主循环 (渲染与输入)
    while (window.isOpen()) {
        sf::Event event;
        while (window.pollEvent(event)) {
            if (event.type == sf::Event::Closed) window.close();
            controller.handleInput(event, window);
        }

        if (model.gameOver) {
            // 简单的控制台输出胜利信息
            static bool printed = false;
            if(!printed) {
                std::cout << "GAME OVER: " << model.winnerMessage << std::endl;
                printed = true;
            }
        }
        
        view.draw(model);
    }

    appRunning = false;
    if (logicThread.joinable()) logicThread.join();

    return 0;
}