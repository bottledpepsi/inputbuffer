// ============================================================
//  Input Buffer v3 (Geode, Windows)
//
//  1. Input buffer: a jump tapped in the air is replayed on the
//     physics tick you land, if it was within the buffer window.
//  2. Trajectory: two invisible "ghost" players are cloned from
//     you every frame and run through GD's own physics/collision:
//       green  = path if you click / keep holding
//       red    = path if you don't click / let go
//       yellow = part of the red path where a tap gets buffered
//  3. Smart Click (uses the two paths):
//       - your click is ignored if clicking dies and not clicking is safe
//       - it clicks for you if not clicking dies and clicking is safe
//       - both die, or you're on an orb: you're in full control
// ============================================================
#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/PlayerObject.hpp>
#include <algorithm>
#include <cmath>
#include <vector>

using namespace geode::prelude;

// ------------------------------------------------------------
// cached settings (read once, updated by listeners)
// ------------------------------------------------------------
struct Config {
    bool enabled = true;
    int64_t bufferWindow = 150;
    bool platformerOnly = false;
    bool showTrajectory = true;
    int64_t trajectoryLength = 1000;
    bool showBufferZone = true;
    bool trajectoryPracticeOnly = false;
    bool smartClick = true;
    int64_t smartLookahead = 350;
    bool smartPracticeOnly = false;
    bool smartIndicator = true;
};
static Config g_cfg;

template <class T, class V>
static void bindSetting(char const* key, V& target) {
    target = static_cast<V>(Mod::get()->getSettingValue<T>(key));
    V* ptr = &target; // g_cfg is static, so this outlives the listener
    listenForSettingChanges<T>(key, [ptr](T value) { *ptr = static_cast<V>(value); });
}

$on_mod(Loaded) {
    bindSetting<bool>("enabled", g_cfg.enabled);
    bindSetting<int64_t>("buffer-window", g_cfg.bufferWindow);
    bindSetting<bool>("platformer-only", g_cfg.platformerOnly);
    bindSetting<bool>("show-trajectory", g_cfg.showTrajectory);
    bindSetting<int64_t>("trajectory-length", g_cfg.trajectoryLength);
    bindSetting<bool>("show-buffer-zone", g_cfg.showBufferZone);
    bindSetting<bool>("trajectory-practice-only", g_cfg.trajectoryPracticeOnly);
    bindSetting<bool>("smart-click", g_cfg.smartClick);
    bindSetting<int64_t>("smart-click-lookahead", g_cfg.smartLookahead);
    bindSetting<bool>("smart-click-practice-only", g_cfg.smartPracticeOnly);
    bindSetting<bool>("smart-click-indicator", g_cfg.smartIndicator);
}

// ------------------------------------------------------------
// shared state
// ------------------------------------------------------------
static constexpr int JUMP = 1;

static PlayerObject* g_ghostA = nullptr;   // "click / hold" ghost
static PlayerObject* g_ghostB = nullptr;   // "no click / release" ghost
static bool g_simulating = false;
static bool g_ghostStopped = false;        // ghost touched orb/pad/portal
static bool g_ghostDied = false;

// GD passes PlayerObject::update a per-step dt measured in 1/60 s units
// (0.25 at 240 TPS). Ticks are counted by P1's real update calls, so the
// buffer window and every lookahead are measured in game time.
static float g_lastRealDt = 0.25f;
static uint64_t g_physTick = 0;

static bool isGhost(PlayerObject* p) {
    return p && (p == g_ghostA || p == g_ghostB);
}

static int msToTicks(int64_t ms) {
    double dt = std::clamp((double)g_lastRealDt, 0.01, 2.0);
    long ticks = std::lround((double)ms * 60.0 / (1000.0 * dt));
    return (int)std::clamp(ticks, 1L, 2000L);
}

// Ground-jump modes. Ship/UFO/wave/swing respond mid-air, so no buffering there.
static bool isGroundJumpMode(PlayerObject* p) {
    return !p->m_isShip && !p->m_isBird && !p->m_isDart && !p->m_isSwing;
}
// Modes where holding (not tapping) is the input.
static bool isHoldMode(PlayerObject* p) {
    return p->m_isShip || p->m_isDart || p->m_isRobot;
}
static bool isTouchingOrb(PlayerObject* p) {
    return p->m_touchingRings && p->m_touchingRings->count() > 0;
}

// ------------------------------------------------------------
// Input state for player 1 (buffer + smart click)
// ------------------------------------------------------------
struct BufferState {
    bool pending = false;
    uint64_t pressTick = 0;
    bool held = false;            // physically held
    bool releaseNextTick = false;
};
static BufferState g_p1, g_p2;

struct SmartState {
    bool gdPressed = false;       // what GD currently thinks (P1 jump)
    bool suppressed = false;      // user is holding but we hid it from GD
    bool autoHeld = false;        // we are holding for the user (hold modes)
    bool tapRelease = false;      // release our auto-tap on the next tick
    bool pressDies = false;       // last frame: clicking now dies soon
    bool releaseDies = false;     // last frame: not clicking dies soon
    bool valid = false;           // the two flags above are fresh
    int autoClicks = 0;
    int blockedClicks = 0;
};
static SmartState g_smart;
static bool g_injecting = false;
static bool g_smartActiveNow = false; // set by PlayLayer each frame

static void resetInputState() {
    g_p1 = {};
    g_p2 = {};
    int a = g_smart.autoClicks, b = g_smart.blockedClicks;
    g_smart = {};
    g_smart.autoClicks = a;
    g_smart.blockedClicks = b;
}

// ------------------------------------------------------------
// PlayerObject hooks: count real ticks, mute ghost effects/stats
// ------------------------------------------------------------
class $modify(IBPlayerObject, PlayerObject) {
    void update(float dt) {
        if (!g_simulating && !isGhost(this)) {
            if (dt > 0.f) g_lastRealDt = dt;
            if (!m_isSecondPlayer) ++g_physTick;
        }
        PlayerObject::update(dt);
    }
    void incrementJumps() {
        if (isGhost(this)) return;
        PlayerObject::incrementJumps();
    }
    void playerDestroyed(bool noEffects) {
        if (isGhost(this)) return;
        PlayerObject::playerDestroyed(noEffects);
    }
    void playSpawnEffect() {
        if (isGhost(this)) return;
        PlayerObject::playSpawnEffect();
    }
    void spawnCircle() {
        if (isGhost(this)) return;
        PlayerObject::spawnCircle();
    }
    void spawnDualCircle() {
        if (isGhost(this)) return;
        PlayerObject::spawnDualCircle();
    }
    void playSpiderDashEffect(CCPoint from, CCPoint to) {
        if (isGhost(this)) return;
        PlayerObject::playSpiderDashEffect(from, to);
    }
    void playDynamicSpiderRun() {
        if (isGhost(this)) return;
        PlayerObject::playDynamicSpiderRun();
    }
    void createFadeOutDartStreak() {
        if (isGhost(this)) return;
        PlayerObject::createFadeOutDartStreak();
    }
    void playBumpEffect(int type, GameObject* obj) {
        if (isGhost(this)) return;
        PlayerObject::playBumpEffect(type, obj);
    }
};

// ------------------------------------------------------------
// GJBaseGameLayer hooks: input, buffer, ghost side-effect blockers
// ------------------------------------------------------------
class $modify(IBGameLayer, GJBaseGameLayer) {
    bool bufferActive() {
        if (!g_cfg.enabled) return false;
        if (m_isEditor) return false;
        if (g_cfg.platformerOnly && !m_isPlatformer) return false;
        return true;
    }

public:
    // Send a P1/P2 jump press/release straight to GD (bypasses our logic).
    void sendJump(bool down, bool isPlayer1) {
        g_injecting = true;
        GJBaseGameLayer::handleButton(down, JUMP, isPlayer1);
        g_injecting = false;
        if (isPlayer1) g_smart.gdPressed = down;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        if (g_injecting || button != JUMP) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        auto& st = isPlayer1 ? g_p1 : g_p2;
        st.held = down;

        // ---- Smart Click: filter player 1's real input ----
        if (isPlayer1 && g_smartActiveNow) {
            auto& s = g_smart;
            if (down) {
                if (s.autoHeld) {          // we were holding: the user takes over
                    s.autoHeld = false;
                    return;                // GD is already pressed
                }
                if (!s.gdPressed && s.valid && s.pressDies && !s.releaseDies) {
                    s.suppressed = true;   // this click would kill you: ignore it
                    s.blockedClicks++;
                    return;
                }
            } else {
                if (s.suppressed) {        // GD never saw the press
                    s.suppressed = false;
                    return;
                }
            }
        }

        GJBaseGameLayer::handleButton(down, button, isPlayer1);
        if (isPlayer1) g_smart.gdPressed = down;

        // ---- Input buffer bookkeeping ----
        if (!down || !bufferActive()) return;
        auto player = isPlayer1 ? m_player1 : m_player2;
        if (!player || player->m_isDead) return;

        if (!player->m_isOnGround && isGroundJumpMode(player) && !isTouchingOrb(player)) {
            st.pending = true;
            st.pressTick = g_physTick;
        } else {
            st.pending = false;
        }
    }

    void tickBuffer(bool isPlayer1) {
        auto& st = isPlayer1 ? g_p1 : g_p2;
        auto player = isPlayer1 ? m_player1 : m_player2;

        if (st.releaseNextTick) {
            st.releaseNextTick = false;
            if (!st.held) sendJump(false, isPlayer1);
        }
        if (!st.pending) return;

        if (!player || player->m_isDead || !bufferActive() || !isGroundJumpMode(player)) {
            st.pending = false;
            return;
        }
        if (g_physTick - st.pressTick > (uint64_t)msToTicks(g_cfg.bufferWindow)) {
            st.pending = false;
            return;
        }
        if (player->m_isOnGround) {
            st.pending = false;
            if (st.held) return; // still holding: GD jumps on landing by itself
            sendJump(true, isPlayer1);
            st.releaseNextTick = true;
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        if (!g_simulating) {
            if (g_smart.tapRelease) {
                g_smart.tapRelease = false;
                if (!g_p1.held && !g_smart.autoHeld) sendJump(false, true);
            }
            tickBuffer(true);
            if (m_gameState.m_isDualMode) tickBuffer(false);
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }

    void resetLevelVariables() {
        resetInputState();
        GJBaseGameLayer::resetLevelVariables();
    }

    // ---- ghost side-effect blockers ----
    void playerTouchedRing(PlayerObject* p, RingObject* o) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::playerTouchedRing(p, o);
    }
    void bumpPlayer(PlayerObject* p, EffectGameObject* o) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::bumpPlayer(p, o);
    }
    void playerWillSwitchMode(PlayerObject* p, GameObject* o) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::playerWillSwitchMode(p, o);
    }
    void flipGravity(PlayerObject* p, bool flip, bool noEffects) {
        if (isGhost(p)) { if (g_simulating) g_ghostStopped = true; return; }
        GJBaseGameLayer::flipGravity(p, flip, noEffects);
    }
    void teleportPlayer(TeleportPortalObject* o, PlayerObject* p) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::teleportPlayer(o, p);
    }
    void toggleDualMode(GameObject* o, bool dual, PlayerObject* p, bool noEffects) {
        if (isGhost(p)) { g_ghostStopped = true; return; }
        GJBaseGameLayer::toggleDualMode(o, dual, p, noEffects);
    }
    void playerTouchedTrigger(PlayerObject* p, EffectGameObject* o) {
        if (isGhost(p)) return;
        GJBaseGameLayer::playerTouchedTrigger(p, o);
    }
    void pickupItem(EffectGameObject* o) {
        if (g_simulating) return;
        GJBaseGameLayer::pickupItem(o);
    }
    void destroyObject(GameObject* o) {
        if (g_simulating) return;
        GJBaseGameLayer::destroyObject(o);
    }
    void gameEventTriggered(GJGameEvent e, int material, int playerID) {
        if (g_simulating) return;
        GJBaseGameLayer::gameEventTriggered(e, material, playerID);
    }
};

// ------------------------------------------------------------
// PlayLayer: ghosts, prediction, smart click, drawing
// ------------------------------------------------------------
enum class SimInput { Release, Hold, Tap };

struct PathResult {
    std::vector<CCPoint> points;
    bool died = false;
    bool stopped = false;
    int deathIndex = -1;
    int landingIndex = -1;
};

class $modify(IBPlayLayer, PlayLayer) {
    struct Fields {
        Ref<PlayerObject> ghostA;
        Ref<PlayerObject> ghostB;
        Ref<CCDrawNode> draw;
        Ref<CCLabelBMFont> label;
    };

    void destroyPlayer(PlayerObject* p, GameObject* o) {
        if (isGhost(p)) {
            g_ghostDied = true;
            g_ghostStopped = true;
            return;
        }
        PlayLayer::destroyPlayer(p, o);
    }

    void onQuit() {
        g_ghostA = g_ghostB = nullptr;
        g_smartActiveNow = false;
        resetInputState();
        PlayLayer::onQuit();
    }

    // ---------- ghosts ----------
    PlayerObject* makeGhost() {
        auto layer = m_objectLayer;
        auto before = layer->getChildrenCount();
        auto ghost = PlayerObject::create(1, 1, this, layer, false);
        if (!ghost) return nullptr;
        ghost->m_playEffects = false;
        layer->addChild(ghost);
        ghost->setVisible(false);
        auto children = layer->getChildren();
        for (auto i = before; children && i < children->count(); i++) {
            if (auto node = typeinfo_cast<CCNode*>(children->objectAtIndex(i))) node->setVisible(false);
        }
        if (ghost->m_regularTrail) ghost->m_regularTrail->setVisible(false);
        if (ghost->m_waveTrail) ghost->m_waveTrail->setVisible(false);
        return ghost;
    }

    void ensureGhosts() {
        auto f = m_fields.self();
        if (!f->ghostA) f->ghostA = makeGhost();
        if (!f->ghostB) f->ghostB = makeGhost();
        if (!f->draw) {
            f->draw = CCDrawNode::create();
            f->draw->setZOrder(9999);
            m_objectLayer->addChild(f->draw);
        }
        g_ghostA = f->ghostA;
        g_ghostB = f->ghostB;
    }

    static void syncMode(PlayerObject* g, PlayerObject* p) {
        if (g->m_isShip && !p->m_isShip) g->toggleFlyMode(false, true);
        if (g->m_isBall && !p->m_isBall) g->toggleRollMode(false, true);
        if (g->m_isBird && !p->m_isBird) g->toggleBirdMode(false, true);
        if (g->m_isDart && !p->m_isDart) g->toggleDartMode(false, true);
        if (g->m_isRobot && !p->m_isRobot) g->toggleRobotMode(false, true);
        if (g->m_isSpider && !p->m_isSpider) g->toggleSpiderMode(false, true);
        if (g->m_isSwing && !p->m_isSwing) g->toggleSwingMode(false, true);

        if (p->m_isShip && !g->m_isShip) g->toggleFlyMode(true, true);
        if (p->m_isBall && !g->m_isBall) g->toggleRollMode(true, true);
        if (p->m_isBird && !g->m_isBird) g->toggleBirdMode(true, true);
        if (p->m_isDart && !g->m_isDart) g->toggleDartMode(true, true);
        if (p->m_isRobot && !g->m_isRobot) g->toggleRobotMode(true, true);
        if (p->m_isSpider && !g->m_isSpider) g->toggleSpiderMode(true, true);
        if (p->m_isSwing && !g->m_isSwing) g->toggleSwingMode(true, true);
    }

    static void copyState(PlayerObject* g, PlayerObject* p) {
        syncMode(g, p);
        bool mini = p->m_vehicleSize != 1.0f;
        if ((g->m_vehicleSize != 1.0f) != mini) g->togglePlayerScale(mini, true);
        g->m_isUpsideDown = p->m_isUpsideDown; // set directly: flipGravity can call back into the level

        g->copyAttributes(p);
        g->setPosition(p->getPosition());
        g->m_position = p->m_position;
        g->m_yVelocity = p->m_yVelocity;
        g->m_isOnGround = p->m_isOnGround;
        g->m_playerSpeed = p->m_playerSpeed;
        g->m_gravityMod = p->m_gravityMod;
        g->m_isPlatformer = p->m_isPlatformer;
        g->m_isGoingLeft = p->m_isGoingLeft;
        g->m_isSideways = p->m_isSideways;
        g->m_holdingButtons = p->m_holdingButtons;
        g->m_isDead = false;
        g->setVisible(false);
    }

    // Run a ghost forward `steps` physics ticks with the given input.
    PathResult simulate(PlayerObject* ghost, PlayerObject* real, SimInput input, int steps) {
        PathResult r;
        copyState(ghost, real);

        ghost->releaseButton(PlayerButton::Jump);
        if (input != SimInput::Release) ghost->pushButton(PlayerButton::Jump);

        g_simulating = true;
        g_ghostStopped = false;
        g_ghostDied = false;

        bool wasAirborne = !ghost->m_isOnGround;
        float dt = g_lastRealDt;
        r.points.reserve(steps + 1);
        r.points.push_back(ghost->getPosition());

        for (int i = 0; i < steps; i++) {
            ghost->update(dt);
            this->checkCollisions(ghost, dt, false);
            r.points.push_back(ghost->getPosition());

            if (i == 0 && input == SimInput::Tap) ghost->releaseButton(PlayerButton::Jump);

            if (wasAirborne && ghost->m_isOnGround && r.landingIndex < 0) r.landingIndex = (int)r.points.size() - 1;
            if (!ghost->m_isOnGround) wasAirborne = true;

            if (g_ghostStopped || ghost->m_isDead) break;
        }

        r.died = g_ghostDied || ghost->m_isDead;
        r.stopped = g_ghostStopped && !r.died;
        if (r.died) r.deathIndex = (int)r.points.size() - 1;
        g_simulating = false;
        g_ghostStopped = g_ghostDied = false;

        ghost->releaseButton(PlayerButton::Jump);
        ghost->setVisible(false);
        return r;
    }

    // ---------- drawing ----------
    static void drawPath(CCDrawNode* d, PathResult const& r, ccColor4F color, int from, int to) {
        from = std::max(from, 0);
        to = std::min(to, (int)r.points.size() - 1);
        for (int i = from; i < to; i++) d->drawSegment(r.points[i], r.points[i + 1], 1.2f, color);
    }

    static void drawEnd(CCDrawNode* d, PathResult const& r) {
        if (r.points.empty()) return;
        auto end = r.points.back();
        if (r.died) {
            auto red = ccc4f(1.f, 0.15f, 0.15f, 1.f);
            d->drawSegment(end + ccp(-6, -6), end + ccp(6, 6), 1.5f, red);
            d->drawSegment(end + ccp(-6, 6), end + ccp(6, -6), 1.5f, red);
        } else if (r.stopped) {
            d->drawDot(end, 4.f, ccc4f(0.3f, 0.8f, 1.f, 1.f));
        }
    }

    bool smartWanted() {
        if (!g_cfg.smartClick) return false;
        if (g_cfg.smartPracticeOnly && !m_isPracticeMode) return false;
        if (m_gameState.m_isDualMode) return false; // player 1 only
        return true;
    }

    void updateLabel(bool smartOn) {
        auto f = m_fields.self();
        bool show = smartOn && g_cfg.smartIndicator;
        if (!f->label) {
            if (!show) return;
            f->label = CCLabelBMFont::create("Smart Click", "bigFont.fnt");
            f->label->setScale(0.3f);
            f->label->setOpacity(140);
            f->label->setAnchorPoint({0.f, 1.f});
            auto win = CCDirector::get()->getWinSize();
            f->label->setPosition({5.f, win.height - 5.f});
            this->addChild(f->label, 1000);
        }
        f->label->setVisible(show);
    }

    // ---------- smart click decision ----------
    void applySmartClick(PlayerObject* real, PathResult const& a, PathResult const& b, int horizon) {
        auto layer = static_cast<IBGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        auto& s = g_smart;

        bool aDies = a.died && a.deathIndex <= horizon;
        bool bDies = b.died && b.deathIndex <= horizon;
        bool hold = isHoldMode(real);

        // remembered for filtering the user's next click
        if (!s.gdPressed) {
            s.pressDies = aDies;
            s.releaseDies = bDies;
            s.valid = true;
        } else {
            s.valid = false;
        }

        if (s.gdPressed) {
            if (aDies && !bDies) {
                // holding / jumping kills you, letting go is safe: let go
                layer->sendJump(false, true);
                if (g_p1.held) { s.suppressed = true; s.blockedClicks++; }
                s.autoHeld = false;
            } else if (s.autoHeld && !g_p1.held && !bDies) {
                // our auto-hold isn't needed anymore
                layer->sendJump(false, true);
                s.autoHeld = false;
            }
        } else {
            if (bDies && !aDies) {
                // not clicking kills you, clicking is safe: click
                layer->sendJump(true, true);
                s.autoClicks++;
                if (g_p1.held) {
                    s.suppressed = false;        // the user's own hold is now live
                } else if (hold) {
                    s.autoHeld = true;
                } else {
                    s.tapRelease = true;
                }
            } else if (s.suppressed && g_p1.held && !aDies) {
                // the user is still holding and it's safe now: let it through
                layer->sendJump(true, true);
                s.suppressed = false;
            }
        }
    }

    void releaseSmartControl() {
        auto layer = static_cast<IBGameLayer*>(static_cast<GJBaseGameLayer*>(this));
        auto& s = g_smart;
        if (s.autoHeld && !g_p1.held) layer->sendJump(false, true);
        if (s.suppressed && g_p1.held) layer->sendJump(true, true); // give the user's hold back
        s.autoHeld = s.suppressed = false;
        s.valid = false;
    }

    // ---------- per frame ----------
    void updatePrediction() {
        auto f = m_fields.self();
        if (f->draw) f->draw->clear();

        bool showTraj = g_cfg.showTrajectory &&
            !(g_cfg.trajectoryPracticeOnly && !m_isPracticeMode);
        bool smart = smartWanted();
        updateLabel(smart);

        auto real = m_player1;
        bool alive = real && !real->m_isDead && !m_hasCompletedLevel;
        bool smartUsable = smart && alive && !isTouchingOrb(real);

        if (!smartUsable && g_smartActiveNow) releaseSmartControl();
        g_smartActiveNow = smartUsable;

        if (!alive || (!showTraj && !smart)) return;

        ensureGhosts();
        if (!f->ghostA || !f->ghostB || !f->draw) return;

        int drawSteps = showTraj ? msToTicks(g_cfg.trajectoryLength) : 0;
        int horizon = msToTicks(g_cfg.smartLookahead);
        int steps = std::max(drawSteps, smart ? horizon : 0);

        // A = click (or keep holding if GD is pressed), B = no click / let go
        SimInput aInput = g_smart.gdPressed ? SimInput::Hold
                        : (isHoldMode(real) ? SimInput::Hold : SimInput::Tap);
        auto b = simulate(f->ghostB, real, SimInput::Release, steps);
        auto a = simulate(f->ghostA, real, aInput, steps);

        if (smartUsable) applySmartClick(real, a, b, horizon);

        if (!showTraj) return;
        auto d = f->draw.data();
        drawPath(d, b, ccc4f(1.f, 0.3f, 0.3f, 0.85f), 0, drawSteps);
        drawPath(d, a, ccc4f(0.3f, 1.f, 0.4f, 0.9f), 0, drawSteps);

        if (g_cfg.showBufferZone && g_cfg.enabled &&
            b.landingIndex > 0 && b.landingIndex <= drawSteps &&
            isGroundJumpMode(real) && !real->m_isOnGround) {
            int start = b.landingIndex - msToTicks(g_cfg.bufferWindow);
            auto yellow = ccc4f(1.f, 0.9f, 0.2f, 1.f);
            drawPath(d, b, yellow, start, b.landingIndex);
            d->drawDot(b.points[b.landingIndex], 3.f, yellow);
        }
        if (b.died ? b.deathIndex <= drawSteps : true) drawEnd(d, b);
        if (a.died ? a.deathIndex <= drawSteps : true) drawEnd(d, a);
    }

    void postUpdate(float dt) {
        PlayLayer::postUpdate(dt);
        updatePrediction();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        auto f = m_fields.self();
        if (f->draw) f->draw->clear();
        resetInputState();
    }
};
