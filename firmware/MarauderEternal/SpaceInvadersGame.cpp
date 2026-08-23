#include "SpaceInvadersGame.h"

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

namespace SpaceInvadersGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t GAME_HEIGHT = TFT_HEIGHT;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t PLAY_TOP = HEADER_HEIGHT + 1;
constexpr uint8_t ALIEN_ROWS = 4;
constexpr uint8_t ALIEN_COLUMNS = 7;
constexpr int16_t ALIEN_WIDTH = 9;
constexpr int16_t ALIEN_HEIGHT = 6;
constexpr int16_t ALIEN_SPACING_X = 15;
constexpr int16_t ALIEN_SPACING_Y = 11;
constexpr int16_t PLAYER_WIDTH = 11;
constexpr int16_t PLAYER_HEIGHT = 5;
constexpr int16_t PLAYER_Y = GAME_HEIGHT - PLAYER_HEIGHT - 4;
constexpr int16_t PLAYER_SPEED = 2;
constexpr uint8_t PLAYER_BULLET_COUNT = 2;
constexpr uint8_t ALIEN_BULLET_COUNT = 3;
constexpr uint16_t FRAME_INTERVAL_MS = 28;
constexpr uint16_t ALIEN_STARTING_STEP_MS = 260;
constexpr uint16_t ALIEN_MINIMUM_STEP_MS = 85;
constexpr uint16_t PLAYER_SHOT_INTERVAL_MS = 190;
constexpr uint8_t STARTING_LIVES = 3;

struct Bullet {
  int16_t x;
  int16_t y;
  bool active;
};

enum class GameState : uint8_t {
  Playing,
  Won,
  Lost,
};

uint8_t aliens[ALIEN_ROWS]{};
Bullet playerBullets[PLAYER_BULLET_COUNT]{};
Bullet alienBullets[ALIEN_BULLET_COUNT]{};
int16_t formationX = 0;
int16_t formationY = 0;
int8_t formationDirection = 1;
int16_t playerX = 0;
uint8_t aliensRemaining = 0;
uint8_t lives = STARTING_LIVES;
uint16_t score = 0;
uint32_t nextAlienMoveAt = 0;
uint32_t nextAlienShotAt = 0;
uint32_t nextPlayerShotAt = 0;
uint32_t invulnerableUntil = 0;
GameState gameState = GameState::Playing;
GameFrameBuffer* frameBuffer = nullptr;

TFT_eSprite& canvas() {
  return frameBuffer->canvas();
}

void presentFrame() {
  frameBuffer->present();
}

const uint16_t ALIEN_COLORS[ALIEN_ROWS] = {
    TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN};

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

bool alienAlive(uint8_t row, uint8_t column) {
  return (aliens[row] & (1U << column)) != 0;
}

int16_t alienX(uint8_t column) {
  return formationX + column * ALIEN_SPACING_X;
}

int16_t alienY(uint8_t row) {
  return formationY + row * ALIEN_SPACING_Y;
}

void clearBullets() {
  for (Bullet& bullet : playerBullets)
    bullet.active = false;
  for (Bullet& bullet : alienBullets)
    bullet.active = false;
}

void drawHeader() {
  canvas().fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_CYAN, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("INVADERS"));
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(55, 4);
  canvas().print(F("S:"));
  canvas().print(score);
  canvas().setCursor(106, 4);
  canvas().print(F("L:"));
  canvas().print(lives);
}

void drawAlien(int16_t x, int16_t y, uint16_t color) {
  canvas().fillRect(x + 2, y, 5, 1, color);
  canvas().fillRect(x + 1, y + 1, 7, 1, color);
  canvas().fillRect(x, y + 2, ALIEN_WIDTH, 2, color);
  canvas().fillRect(x + 1, y + 4, 2, 1, color);
  canvas().fillRect(x + 6, y + 4, 2, 1, color);
  canvas().drawPixel(x + 2, y + 2, TFT_BLACK);
  canvas().drawPixel(x + 6, y + 2, TFT_BLACK);
}

void drawPlayer(uint32_t now) {
  if (static_cast<int32_t>(now - invulnerableUntil) < 0 &&
      ((now / 100) & 1U) != 0) {
    return;
  }
  canvas().fillRect(playerX + 4, PLAYER_Y, 3, 1, TFT_CYAN);
  canvas().fillRect(playerX + 2, PLAYER_Y + 1, 7, 2, TFT_CYAN);
  canvas().fillRect(playerX, PLAYER_Y + 3, PLAYER_WIDTH, 2, TFT_CYAN);
}

void drawFrame(uint32_t now) {
  canvas().fillRect(0, PLAY_TOP, GAME_WIDTH,
                    GAME_HEIGHT - PLAY_TOP, TFT_BLACK);
  for (uint8_t row = 0; row < ALIEN_ROWS; row++) {
    for (uint8_t column = 0; column < ALIEN_COLUMNS; column++) {
      if (alienAlive(row, column))
        drawAlien(alienX(column), alienY(row), ALIEN_COLORS[row]);
    }
  }
  for (const Bullet& bullet : playerBullets) {
    if (bullet.active)
      canvas().fillRect(bullet.x, bullet.y, 1, 4, TFT_WHITE);
  }
  for (const Bullet& bullet : alienBullets) {
    if (bullet.active)
      canvas().fillRect(bullet.x, bullet.y, 2, 4, TFT_RED);
  }
  drawPlayer(now);
  canvas().drawFastHLine(0, GAME_HEIGHT - 1, GAME_WIDTH, TFT_DARKGREY);
  presentFrame();
}

void drawEndScreen() {
  const bool won = gameState == GameState::Won;
  canvas().fillRect(8, 42, GAME_WIDTH - 16, 57, TFT_BLACK);
  canvas().drawRect(8, 42, GAME_WIDTH - 16, 57,
                    won ? TFT_GREEN : TFT_RED);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(won ? TFT_GREEN : TFT_RED, TFT_BLACK);
  canvas().drawString(won ? "EARTH SAVED" : "GAME OVER",
                      GAME_WIDTH / 2, 47, 2);
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().drawString(String(F("Score ")) + score,
                      GAME_WIDTH / 2, 68, 1);
  canvas().drawString("Direction: retry", GAME_WIDTH / 2, 80, 1);
  canvas().drawString("Center: exit", GAME_WIDTH / 2, 90, 1);
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void finishGame(GameState result) {
  gameState = result;
  drawHeader();
  drawEndScreen();
}

void resetGame() {
  for (uint8_t row = 0; row < ALIEN_ROWS; row++)
    aliens[row] = 0x7F;
  clearBullets();
  aliensRemaining = ALIEN_ROWS * ALIEN_COLUMNS;
  formationX = 9;
  formationY = 23;
  formationDirection = 1;
  playerX = (GAME_WIDTH - PLAYER_WIDTH) / 2;
  lives = STARTING_LIVES;
  score = 0;
  gameState = GameState::Playing;
  const uint32_t now = millis();
  nextAlienMoveAt = now + ALIEN_STARTING_STEP_MS;
  nextAlienShotAt = now + 800;
  nextPlayerShotAt = now;
  invulnerableUntil = 0;
  canvas().fillScreen(TFT_BLACK);
  drawHeader();
  drawFrame(now);
}

void movePlayer() {
  const bool left = buttonDown(l_btn);
  const bool right = buttonDown(r_btn);
  if (left != right)
    playerX += left ? -PLAYER_SPEED : PLAYER_SPEED;
  if (playerX < 1)
    playerX = 1;
  if (playerX > GAME_WIDTH - PLAYER_WIDTH - 1)
    playerX = GAME_WIDTH - PLAYER_WIDTH - 1;
}

void firePlayerBullet(uint32_t now) {
  if (!buttonDown(u_btn) ||
      static_cast<int32_t>(now - nextPlayerShotAt) < 0) {
    return;
  }
  for (Bullet& bullet : playerBullets) {
    if (!bullet.active) {
      bullet = Bullet{static_cast<int16_t>(playerX + PLAYER_WIDTH / 2),
                      static_cast<int16_t>(PLAYER_Y - 4), true};
      nextPlayerShotAt = now + PLAYER_SHOT_INTERVAL_MS;
      return;
    }
  }
}

void moveAliens(uint32_t now) {
  if (static_cast<int32_t>(now - nextAlienMoveAt) < 0)
    return;

  int16_t leftEdge = GAME_WIDTH;
  int16_t rightEdge = 0;
  int16_t bottomEdge = PLAY_TOP;
  for (uint8_t row = 0; row < ALIEN_ROWS; row++) {
    for (uint8_t column = 0; column < ALIEN_COLUMNS; column++) {
      if (!alienAlive(row, column))
        continue;
      leftEdge = min(leftEdge, alienX(column));
      const int16_t right = alienX(column) + ALIEN_WIDTH;
      const int16_t bottom = alienY(row) + ALIEN_HEIGHT;
      if (right > rightEdge)
        rightEdge = right;
      if (bottom > bottomEdge)
        bottomEdge = bottom;
    }
  }

  const int16_t step = formationDirection * 3;
  if (leftEdge + step < 2 || rightEdge + step > GAME_WIDTH - 2) {
    formationDirection = -formationDirection;
    formationY += 5;
    bottomEdge += 5;
  }
  else {
    formationX += step;
  }

  if (bottomEdge >= PLAYER_Y)
    finishGame(GameState::Lost);

  const uint16_t speedGain = (ALIEN_ROWS * ALIEN_COLUMNS - aliensRemaining) * 6;
  uint16_t interval = ALIEN_STARTING_STEP_MS;
  if (speedGain < ALIEN_STARTING_STEP_MS - ALIEN_MINIMUM_STEP_MS)
    interval -= speedGain;
  else
    interval = ALIEN_MINIMUM_STEP_MS;
  nextAlienMoveAt = now + interval;
}

bool createAlienBullet(int16_t x, int16_t y) {
  for (Bullet& bullet : alienBullets) {
    if (!bullet.active) {
      bullet = Bullet{x, y, true};
      return true;
    }
  }
  return false;
}

void fireAlienBullet(uint32_t now) {
  if (static_cast<int32_t>(now - nextAlienShotAt) < 0)
    return;

  const uint8_t firstColumn = static_cast<uint8_t>(random(0, ALIEN_COLUMNS));
  for (uint8_t offset = 0; offset < ALIEN_COLUMNS; offset++) {
    const uint8_t column = (firstColumn + offset) % ALIEN_COLUMNS;
    for (int8_t row = ALIEN_ROWS - 1; row >= 0; row--) {
      if (alienAlive(row, column)) {
        createAlienBullet(alienX(column) + ALIEN_WIDTH / 2,
                          alienY(row) + ALIEN_HEIGHT);
        nextAlienShotAt = now + random(650, 1150);
        return;
      }
    }
  }
}

void hitAlienWith(Bullet& bullet) {
  for (uint8_t row = 0; row < ALIEN_ROWS; row++) {
    for (uint8_t column = 0; column < ALIEN_COLUMNS; column++) {
      if (!alienAlive(row, column))
        continue;
      const int16_t x = alienX(column);
      const int16_t y = alienY(row);
      if (bullet.x >= x && bullet.x < x + ALIEN_WIDTH &&
          bullet.y <= y + ALIEN_HEIGHT && bullet.y + 4 >= y) {
        aliens[row] &= static_cast<uint8_t>(~(1U << column));
        aliensRemaining--;
        score += (ALIEN_ROWS - row) * 10;
        bullet.active = false;
        drawHeader();
        if (aliensRemaining == 0)
          finishGame(GameState::Won);
        return;
      }
    }
  }
}

void hitPlayer(uint32_t now) {
  if (static_cast<int32_t>(now - invulnerableUntil) < 0)
    return;
  if (lives > 0)
    lives--;
  drawHeader();
  if (lives == 0) {
    finishGame(GameState::Lost);
    return;
  }
  playerX = (GAME_WIDTH - PLAYER_WIDTH) / 2;
  invulnerableUntil = now + 900;
  for (Bullet& bullet : alienBullets)
    bullet.active = false;
}

void updateBullets(uint32_t now) {
  for (Bullet& bullet : playerBullets) {
    if (!bullet.active)
      continue;
    bullet.y -= 4;
    if (bullet.y < PLAY_TOP)
      bullet.active = false;
    else
      hitAlienWith(bullet);
    if (gameState != GameState::Playing)
      return;
  }

  for (Bullet& bullet : alienBullets) {
    if (!bullet.active)
      continue;
    bullet.y += 3;
    if (bullet.y >= GAME_HEIGHT) {
      bullet.active = false;
      continue;
    }
    if (bullet.y + 4 >= PLAYER_Y && bullet.y <= PLAYER_Y + PLAYER_HEIGHT &&
        bullet.x + 2 >= playerX && bullet.x <= playerX + PLAYER_WIDTH) {
      bullet.active = false;
      hitPlayer(now);
      if (gameState != GameState::Playing)
        return;
    }
  }
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
    if (c_btn.justPressed())
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
    if (static_cast<int32_t>(now - nextFrameAt) >= 0) {
      movePlayer();
      firePlayerBullet(now);
      moveAliens(now);
      if (!gameFinished()) {
        fireAlienBullet(now);
        updateBullets(now);
      }
      if (!gameFinished())
        drawFrame(now);
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

}  // namespace SpaceInvadersGame

#else

namespace SpaceInvadersGame {
void run() {}
}  // namespace SpaceInvadersGame

#endif
