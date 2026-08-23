#include "MissileCommandGame.h"

#include "configs.h"

#if defined(HAS_SCREEN) && defined(HAS_BUTTONS) && \
    (U_BTN >= 0) && (D_BTN >= 0) && (L_BTN >= 0) && (R_BTN >= 0) && \
    (C_BTN >= 0)

#include "Display.h"
#include "GameFrameBuffer.h"
#include "GameInput.h"
#include "Switches.h"

extern Display display_obj;
extern Switches u_btn;
extern Switches d_btn;
extern Switches l_btn;
extern Switches r_btn;
extern Switches c_btn;

namespace MissileCommandGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t GAME_HEIGHT = TFT_HEIGHT;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t PLAY_TOP = HEADER_HEIGHT + 1;
constexpr int16_t GROUND_Y = GAME_HEIGHT - 7;
constexpr int16_t LAUNCHER_X = GAME_WIDTH / 2;
constexpr int16_t LAUNCHER_Y = GROUND_Y - 2;
constexpr uint8_t CITY_COUNT = 4;
constexpr uint8_t ENEMY_MISSILE_COUNT = 6;
constexpr uint8_t PLAYER_MISSILE_COUNT = 3;
constexpr uint8_t EXPLOSION_COUNT = 6;
constexpr uint8_t TOTAL_ENEMY_MISSILES = 14;
constexpr uint8_t STARTING_AMMO = 16;
constexpr uint16_t FRAME_INTERVAL_MS = 34;

const int16_t CITY_X[CITY_COUNT] = {18, 40, 88, 110};

struct EnemyMissile {
  int16_t startX;
  int16_t targetX;
  uint16_t progress;
  uint8_t speed;
  uint8_t targetCity;
  bool active;
};

struct PlayerMissile {
  int16_t targetX;
  int16_t targetY;
  uint16_t progress;
  bool active;
};

struct Explosion {
  int16_t x;
  int16_t y;
  int8_t radius;
  int8_t maximumRadius;
  bool growing;
  bool active;
};

enum class GameState : uint8_t {
  Playing,
  Won,
  Lost,
};

EnemyMissile enemyMissiles[ENEMY_MISSILE_COUNT]{};
PlayerMissile playerMissiles[PLAYER_MISSILE_COUNT]{};
Explosion explosions[EXPLOSION_COUNT]{};
bool cities[CITY_COUNT]{};
int16_t cursorX = GAME_WIDTH / 2;
int16_t cursorY = 65;
uint8_t ammo = STARTING_AMMO;
uint8_t spawnedMissiles = 0;
uint16_t score = 0;
uint32_t nextEnemySpawnAt = 0;
uint32_t nextPlayerShotAt = 0;
GameState gameState = GameState::Playing;
GameFrameBuffer* frameBuffer = nullptr;

TFT_eSprite& canvas() {
  return frameBuffer->canvas();
}

void presentFrame() {
  frameBuffer->present();
}

bool buttonDown(Switches& button) {
  const bool level = digitalRead(button.getPin());
  return button.getPullup() ? level == LOW : level == HIGH;
}

void releaseButton(Switches& button) {
  GameInput::waitForRelease(button);
}

void releaseControls() {
  releaseButton(u_btn);
  releaseButton(d_btn);
  releaseButton(l_btn);
  releaseButton(r_btn);
  releaseButton(c_btn);
}

int16_t enemyX(const EnemyMissile& missile) {
  return missile.startX +
         ((missile.targetX - missile.startX) * missile.progress) / 1000;
}

int16_t enemyY(const EnemyMissile& missile) {
  return PLAY_TOP + ((GROUND_Y - PLAY_TOP) * missile.progress) / 1000;
}

int16_t playerMissileX(const PlayerMissile& missile) {
  return LAUNCHER_X +
         ((missile.targetX - LAUNCHER_X) * missile.progress) / 1000;
}

int16_t playerMissileY(const PlayerMissile& missile) {
  return LAUNCHER_Y +
         ((missile.targetY - LAUNCHER_Y) * missile.progress) / 1000;
}

uint8_t livingCities() {
  uint8_t count = 0;
  for (bool city : cities) {
    if (city)
      count++;
  }
  return count;
}

void drawHeader() {
  canvas().fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_ORANGE, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("MISSILE"));
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(48, 4);
  canvas().print(F("S:"));
  canvas().print(score);
  canvas().setCursor(96, 4);
  canvas().print(F("A:"));
  canvas().print(ammo);
}

void drawCity(int16_t x, bool alive) {
  if (!alive) {
    canvas().fillRect(x - 6, GROUND_Y - 3, 12, 3, TFT_DARKGREY);
    return;
  }
  canvas().fillRect(x - 6, GROUND_Y - 7, 4, 7, TFT_CYAN);
  canvas().fillRect(x - 1, GROUND_Y - 10, 4, 10, TFT_CYAN);
  canvas().fillRect(x + 4, GROUND_Y - 5, 3, 5, TFT_CYAN);
  canvas().drawPixel(x, GROUND_Y - 7, TFT_YELLOW);
  canvas().drawPixel(x - 4, GROUND_Y - 4, TFT_YELLOW);
  canvas().drawPixel(x + 5, GROUND_Y - 3, TFT_YELLOW);
}

void drawLauncher() {
  canvas().fillTriangle(LAUNCHER_X, LAUNCHER_Y - 5,
                        LAUNCHER_X - 7, GROUND_Y,
                        LAUNCHER_X + 7, GROUND_Y, TFT_GREEN);
  canvas().fillRect(LAUNCHER_X - 1, LAUNCHER_Y - 8, 3, 6, TFT_WHITE);
}

void drawCursor() {
  canvas().drawFastHLine(cursorX - 4, cursorY, 9, TFT_WHITE);
  canvas().drawFastVLine(cursorX, cursorY - 4, 9, TFT_WHITE);
  canvas().drawPixel(cursorX, cursorY, TFT_BLACK);
}

void drawFrame() {
  canvas().fillRect(0, PLAY_TOP, GAME_WIDTH,
                    GAME_HEIGHT - PLAY_TOP, TFT_BLACK);
  canvas().drawFastHLine(0, GROUND_Y, GAME_WIDTH, TFT_GREEN);

  for (uint8_t index = 0; index < CITY_COUNT; index++)
    drawCity(CITY_X[index], cities[index]);
  drawLauncher();

  for (const EnemyMissile& missile : enemyMissiles) {
    if (!missile.active)
      continue;
    const int16_t x = enemyX(missile);
    const int16_t y = enemyY(missile);
    canvas().drawLine(missile.startX, PLAY_TOP, x, y, TFT_RED);
    canvas().fillCircle(x, y, 1, TFT_YELLOW);
  }

  for (const PlayerMissile& missile : playerMissiles) {
    if (!missile.active)
      continue;
    const int16_t x = playerMissileX(missile);
    const int16_t y = playerMissileY(missile);
    canvas().drawLine(LAUNCHER_X, LAUNCHER_Y, x, y, TFT_CYAN);
    canvas().drawPixel(x, y, TFT_WHITE);
  }

  for (const Explosion& explosion : explosions) {
    if (explosion.active)
      canvas().drawCircle(explosion.x, explosion.y,
                          explosion.radius, TFT_YELLOW);
  }
  drawCursor();
  presentFrame();
}

void drawEndScreen() {
  const bool won = gameState == GameState::Won;
  canvas().fillRect(7, 42, GAME_WIDTH - 14, 60, TFT_BLACK);
  canvas().drawRect(7, 42, GAME_WIDTH - 14, 60,
                    won ? TFT_GREEN : TFT_RED);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(won ? TFT_GREEN : TFT_RED, TFT_BLACK);
  canvas().drawString(won ? "CITIES SAVED" : "CITIES LOST",
                      GAME_WIDTH / 2, 47, 2);
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().drawString(String(F("Score ")) + score,
                      GAME_WIDTH / 2, 69, 1);
  canvas().drawString("Direction: retry", GAME_WIDTH / 2, 81, 1);
  canvas().drawString("Hold center: exit", GAME_WIDTH / 2, 91, 1);
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void finishGame(GameState result) {
  gameState = result;
  drawHeader();
  drawEndScreen();
}

void clearObjects() {
  for (EnemyMissile& missile : enemyMissiles)
    missile.active = false;
  for (PlayerMissile& missile : playerMissiles)
    missile.active = false;
  for (Explosion& explosion : explosions)
    explosion.active = false;
}

void resetGame() {
  clearObjects();
  for (bool& city : cities)
    city = true;
  cursorX = GAME_WIDTH / 2;
  cursorY = 65;
  ammo = STARTING_AMMO;
  spawnedMissiles = 0;
  score = 0;
  gameState = GameState::Playing;
  const uint32_t now = millis();
  nextEnemySpawnAt = now + 700;
  nextPlayerShotAt = now;
  canvas().fillScreen(TFT_BLACK);
  drawHeader();
  drawFrame();
}

bool createExplosion(int16_t x, int16_t y, int8_t maximumRadius) {
  for (Explosion& explosion : explosions) {
    if (!explosion.active) {
      explosion = Explosion{x, y, 2, maximumRadius, true, true};
      return true;
    }
  }
  return false;
}

int8_t chooseLivingCity() {
  if (livingCities() == 0)
    return -1;
  const uint8_t first = static_cast<uint8_t>(random(0, CITY_COUNT));
  for (uint8_t offset = 0; offset < CITY_COUNT; offset++) {
    const uint8_t index = (first + offset) % CITY_COUNT;
    if (cities[index])
      return index;
  }
  return -1;
}

void spawnEnemyMissile(uint32_t now) {
  if (spawnedMissiles >= TOTAL_ENEMY_MISSILES ||
      static_cast<int32_t>(now - nextEnemySpawnAt) < 0) {
    return;
  }
  const int8_t target = chooseLivingCity();
  if (target < 0)
    return;
  for (EnemyMissile& missile : enemyMissiles) {
    if (!missile.active) {
      missile.startX = static_cast<int16_t>(random(4, GAME_WIDTH - 4));
      missile.targetX = CITY_X[target];
      missile.progress = 0;
      missile.speed = static_cast<uint8_t>(random(4, 8));
      missile.targetCity = target;
      missile.active = true;
      spawnedMissiles++;
      nextEnemySpawnAt = now + random(520, 900);
      return;
    }
  }
}

void moveCursor() {
  const bool left = buttonDown(l_btn);
  const bool right = buttonDown(r_btn);
  const bool up = buttonDown(u_btn);
  const bool down = buttonDown(d_btn);
  if (left != right)
    cursorX += left ? -2 : 2;
  if (up != down)
    cursorY += up ? -2 : 2;
  if (cursorX < 4)
    cursorX = 4;
  if (cursorX > GAME_WIDTH - 5)
    cursorX = GAME_WIDTH - 5;
  if (cursorY < PLAY_TOP + 5)
    cursorY = PLAY_TOP + 5;
  if (cursorY > GROUND_Y - 15)
    cursorY = GROUND_Y - 15;
}

void firePlayerMissile(uint32_t now) {
  if (ammo == 0 || static_cast<int32_t>(now - nextPlayerShotAt) < 0)
    return;
  for (PlayerMissile& missile : playerMissiles) {
    if (!missile.active) {
      missile = PlayerMissile{cursorX, cursorY, 0, true};
      ammo--;
      nextPlayerShotAt = now + 180;
      drawHeader();
      return;
    }
  }
}

void updatePlayerMissiles() {
  for (PlayerMissile& missile : playerMissiles) {
    if (!missile.active)
      continue;
    missile.progress += 75;
    if (missile.progress >= 1000) {
      missile.progress = 1000;
      createExplosion(missile.targetX, missile.targetY, 16);
      missile.active = false;
    }
  }
}

bool pointInExplosion(int16_t x, int16_t y) {
  for (const Explosion& explosion : explosions) {
    if (!explosion.active)
      continue;
    const int16_t deltaX = x - explosion.x;
    const int16_t deltaY = y - explosion.y;
    if (deltaX * deltaX + deltaY * deltaY <=
        explosion.radius * explosion.radius) {
      return true;
    }
  }
  return false;
}

void updateEnemyMissiles() {
  for (EnemyMissile& missile : enemyMissiles) {
    if (!missile.active)
      continue;
    missile.progress += missile.speed;
    if (missile.progress > 1000)
      missile.progress = 1000;

    const int16_t x = enemyX(missile);
    const int16_t y = enemyY(missile);
    if (pointInExplosion(x, y)) {
      missile.active = false;
      score += 25;
      createExplosion(x, y, 11);
      drawHeader();
      continue;
    }

    if (missile.progress >= 1000) {
      missile.active = false;
      cities[missile.targetCity] = false;
      createExplosion(missile.targetX, GROUND_Y - 3, 9);
      if (livingCities() == 0) {
        finishGame(GameState::Lost);
        return;
      }
    }
  }
}

void updateExplosions() {
  for (Explosion& explosion : explosions) {
    if (!explosion.active)
      continue;
    if (explosion.growing) {
      explosion.radius += 2;
      if (explosion.radius >= explosion.maximumRadius) {
        explosion.radius = explosion.maximumRadius;
        explosion.growing = false;
      }
    }
    else {
      explosion.radius -= 2;
      if (explosion.radius <= 0)
        explosion.active = false;
    }
  }
}

bool enemiesActive() {
  for (const EnemyMissile& missile : enemyMissiles) {
    if (missile.active)
      return true;
  }
  return false;
}

bool retryPressed() {
  return u_btn.justPressed() || d_btn.justPressed() ||
         l_btn.justPressed() || r_btn.justPressed();
}

bool gameFinished() {
  return gameState == GameState::Won || gameState == GameState::Lost;
}

}  // namespace

void run() {
  releaseControls();
  randomSeed(micros());
  GameFrameBuffer bufferedFrame(display_obj.tft);
  if (!bufferedFrame.begin(GAME_WIDTH, GAME_HEIGHT)) {
    display_obj.tft.fillScreen(TFT_BLACK);
    display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
    display_obj.tft.drawString("Frame buffer error", 8, 56, 1);
    releaseControls();
    return;
  }
  frameBuffer = &bufferedFrame;
  resetGame();
  uint32_t nextFrameAt = millis();

  while (true) {
    const bool centerPressed = c_btn.justPressed();
    if (c_btn.isHeld())
      break;

    if (gameFinished()) {
      if (retryPressed()) {
        resetGame();
        nextFrameAt = millis();
      }
      delay(5);
      continue;
    }

    const uint32_t now = millis();
    if (centerPressed)
      firePlayerMissile(now);

    if (static_cast<int32_t>(now - nextFrameAt) >= 0) {
      moveCursor();
      spawnEnemyMissile(now);
      updatePlayerMissiles();
      updateEnemyMissiles();
      if (!gameFinished()) {
        updateExplosions();
        if (spawnedMissiles >= TOTAL_ENEMY_MISSILES && !enemiesActive())
          finishGame(GameState::Won);
      }
      if (!gameFinished())
        drawFrame();
      nextFrameAt = now + FRAME_INTERVAL_MS;
    }
    delay(3);
  }

  frameBuffer = nullptr;
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace MissileCommandGame

#else

namespace MissileCommandGame {
void run() {}
}  // namespace MissileCommandGame

#endif
