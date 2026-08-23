#include "EightBallGame.h"

#include "configs.h"

#if defined(HAS_SCREEN) && defined(HAS_BUTTONS) && \
    (U_BTN >= 0) && (D_BTN >= 0) && (L_BTN >= 0) && (R_BTN >= 0) && \
    (C_BTN >= 0)

#include <math.h>

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

namespace EightBallGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t GAME_HEIGHT = TFT_HEIGHT;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t TABLE_X = 3;
constexpr int16_t TABLE_Y = 18;
constexpr int16_t TABLE_WIDTH = GAME_WIDTH - 6;
constexpr int16_t TABLE_HEIGHT = GAME_HEIGHT - TABLE_Y - 2;
constexpr float LEFT_CUSHION = 9.0f;
constexpr float RIGHT_CUSHION = 119.0f;
constexpr float TOP_CUSHION = 24.0f;
constexpr float BOTTOM_CUSHION = 119.0f;
constexpr float BALL_RADIUS = 3.0f;
constexpr float BALL_DIAMETER = BALL_RADIUS * 2.0f;
constexpr uint8_t BALL_COUNT = 9;
constexpr uint16_t FRAME_INTERVAL_MS = 20;
constexpr uint16_t AIM_REPEAT_MS = 55;
constexpr uint16_t FELT_COLOR = 0x0340;
constexpr uint16_t RAIL_COLOR = 0x8200;

const float SHOT_SPEEDS[5] = {2.10f, 2.75f, 3.50f, 4.40f, 6.20f};
const uint8_t GUIDE_LENGTHS[5] = {17, 25, 33, 42, 52};
const uint16_t GUIDE_COLORS[5] = {
    TFT_GREEN, TFT_CYAN, TFT_YELLOW, TFT_ORANGE, TFT_RED};

struct Ball {
  float x;
  float y;
  float velocityX;
  float velocityY;
  uint16_t color;
  bool active;
};

enum class GameState : uint8_t {
  Aiming,
  Rolling,
  Won,
  Lost,
};

Ball balls[BALL_COUNT]{};
GameState gameState = GameState::Aiming;
uint16_t aimDegrees = 0;
uint8_t shotPower = 3;
uint8_t targetsRemaining = 7;
uint16_t shots = 0;
uint32_t nextAimStepAt = 0;
GameFrameBuffer* frameBuffer = nullptr;

const uint16_t BALL_COLORS[BALL_COUNT] = {
    TFT_WHITE, TFT_YELLOW, TFT_BLUE, TFT_RED, TFT_PURPLE,
    TFT_ORANGE, TFT_GREEN, TFT_MAROON, TFT_BLACK};

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

template <typename Surface>
void drawHeader(Surface& surface) {
  surface.fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  surface.setTextDatum(TL_DATUM);
  surface.setTextSize(1);
  surface.setTextColor(TFT_WHITE, TFT_NAVY);
  surface.setCursor(2, 4);
  surface.print(F("8 BALL"));
  surface.setCursor(48, 4);
  surface.print(F("B:"));
  surface.print(targetsRemaining);
  surface.setCursor(78, 4);
  surface.print(F("P:"));
  surface.print(shotPower);
  surface.setCursor(106, 4);
  surface.print(F("S:"));
  surface.print(shots);
}

template <typename Surface>
void drawTable(Surface& surface) {
  surface.fillRect(0, HEADER_HEIGHT, GAME_WIDTH,
                   GAME_HEIGHT - HEADER_HEIGHT, TFT_BLACK);
  surface.fillRect(TABLE_X, TABLE_Y, TABLE_WIDTH, TABLE_HEIGHT, RAIL_COLOR);
  surface.fillRect(TABLE_X + 5, TABLE_Y + 5,
                   TABLE_WIDTH - 10, TABLE_HEIGHT - 10, FELT_COLOR);

  const int16_t pocketX[6] = {7, GAME_WIDTH / 2, GAME_WIDTH - 7,
                              7, GAME_WIDTH / 2, GAME_WIDTH - 7};
  const int16_t pocketY[6] = {22, 22, 22,
                              GAME_HEIGHT - 7, GAME_HEIGHT - 7,
                              GAME_HEIGHT - 7};
  for (uint8_t pocket = 0; pocket < 6; pocket++)
    surface.fillCircle(pocketX[pocket], pocketY[pocket], 6, TFT_BLACK);
}

template <typename Surface>
void drawBall(Surface& surface, uint8_t index) {
  if (!balls[index].active)
    return;
  const int16_t x = static_cast<int16_t>(balls[index].x + 0.5f);
  const int16_t y = static_cast<int16_t>(balls[index].y + 0.5f);
  if (index == 8) {
    surface.fillCircle(x, y, 3, TFT_BLACK);
    surface.drawPixel(x, y, TFT_WHITE);
  }
  else {
    surface.fillCircle(x, y, 3, balls[index].color);
    if (index != 0)
      surface.drawPixel(x, y, TFT_WHITE);
  }
}

template <typename Surface>
void drawAimGuide(Surface& surface) {
  if (gameState != GameState::Aiming || !balls[0].active)
    return;
  const float radians = aimDegrees * DEG_TO_RAD;
  const float directionX = cosf(radians);
  const float directionY = sinf(radians);
  const float guideLength = GUIDE_LENGTHS[shotPower - 1];
  const uint16_t guideColor = GUIDE_COLORS[shotPower - 1];
  const int16_t startX = static_cast<int16_t>(balls[0].x);
  const int16_t startY = static_cast<int16_t>(balls[0].y);
  const int16_t lineStartX = startX + static_cast<int16_t>(directionX * 5.0f);
  const int16_t lineStartY = startY + static_cast<int16_t>(directionY * 5.0f);
  const int16_t endX = startX + static_cast<int16_t>(directionX * guideLength);
  const int16_t endY = startY + static_cast<int16_t>(directionY * guideLength);
  surface.drawLine(lineStartX, lineStartY, endX, endY, guideColor);

  const float baseX = endX - directionX * 7.0f;
  const float baseY = endY - directionY * 7.0f;
  const float perpendicularX = -directionY * 4.0f;
  const float perpendicularY = directionX * 4.0f;
  surface.drawLine(endX, endY,
                   static_cast<int16_t>(baseX + perpendicularX),
                   static_cast<int16_t>(baseY + perpendicularY), guideColor);
  surface.drawLine(endX, endY,
                   static_cast<int16_t>(baseX - perpendicularX),
                   static_cast<int16_t>(baseY - perpendicularY), guideColor);
  surface.fillCircle(endX, endY, 1, guideColor);
}

template <typename Surface>
void composeFrame(Surface& surface) {
  drawHeader(surface);
  drawTable(surface);
  for (uint8_t index = 0; index < BALL_COUNT; index++)
    drawBall(surface, index);
  drawAimGuide(surface);
}

void drawFrame() {
  composeFrame(frameBuffer->canvas());
  frameBuffer->present();
}

void drawEndScreen() {
  const bool won = gameState == GameState::Won;
  const uint16_t color = won ? TFT_GREEN : TFT_RED;
  TFT_eSprite& surface = frameBuffer->canvas();
  surface.fillRect(9, 48, GAME_WIDTH - 18, 55, TFT_BLACK);
  surface.drawRect(9, 48, GAME_WIDTH - 18, 55, color);
  surface.setTextDatum(TC_DATUM);
  surface.setTextColor(color, TFT_BLACK);
  surface.drawString(won ? "TABLE CLEARED" : "8 BALL EARLY",
                     GAME_WIDTH / 2, 53, 2);
  surface.setTextColor(TFT_WHITE, TFT_BLACK);
  surface.drawString(String(F("Shots ")) + shots,
                     GAME_WIDTH / 2, 74, 1);
  surface.drawString("Direction: retry", GAME_WIDTH / 2, 85, 1);
  surface.drawString("Hold center: exit", GAME_WIDTH / 2, 94, 1);
  surface.setTextDatum(TL_DATUM);
  frameBuffer->present();
}

void placeBall(uint8_t index, float x, float y) {
  balls[index].x = x;
  balls[index].y = y;
  balls[index].velocityX = 0.0f;
  balls[index].velocityY = 0.0f;
  balls[index].color = BALL_COLORS[index];
  balls[index].active = true;
}

void resetGame() {
  placeBall(0, 28.0f, 72.0f);
  placeBall(1, 83.0f, 72.0f);
  placeBall(2, 90.0f, 68.0f);
  placeBall(3, 90.0f, 76.0f);
  placeBall(4, 97.0f, 64.0f);
  placeBall(5, 97.0f, 80.0f);
  placeBall(6, 104.0f, 68.0f);
  placeBall(7, 104.0f, 76.0f);
  placeBall(8, 97.0f, 72.0f);
  gameState = GameState::Aiming;
  aimDegrees = 0;
  shotPower = 3;
  targetsRemaining = 7;
  shots = 0;
  nextAimStepAt = millis();
  drawFrame();
}

bool nearPocket(float x, float y) {
  const float pocketX[6] = {7.0f, GAME_WIDTH / 2.0f, GAME_WIDTH - 7.0f,
                            7.0f, GAME_WIDTH / 2.0f, GAME_WIDTH - 7.0f};
  const float pocketY[6] = {22.0f, 22.0f, 22.0f,
                            GAME_HEIGHT - 7.0f, GAME_HEIGHT - 7.0f,
                            GAME_HEIGHT - 7.0f};
  for (uint8_t pocket = 0; pocket < 6; pocket++) {
    const float dx = x - pocketX[pocket];
    const float dy = y - pocketY[pocket];
    if (dx * dx + dy * dy <= 48.0f)
      return true;
  }
  return false;
}

void pocketBall(uint8_t index) {
  balls[index].active = false;
  balls[index].velocityX = 0.0f;
  balls[index].velocityY = 0.0f;
  if (index >= 1 && index <= 7) {
    if (targetsRemaining > 0)
      targetsRemaining--;
  }
  else if (index == 8) {
    gameState = targetsRemaining == 0 ? GameState::Won : GameState::Lost;
  }
}

void bounceFromCushions(Ball& ball) {
  if (ball.x < LEFT_CUSHION) {
    ball.x = LEFT_CUSHION;
    if (ball.velocityX < 0.0f)
      ball.velocityX = -ball.velocityX;
  }
  else if (ball.x > RIGHT_CUSHION) {
    ball.x = RIGHT_CUSHION;
    if (ball.velocityX > 0.0f)
      ball.velocityX = -ball.velocityX;
  }
  if (ball.y < TOP_CUSHION) {
    ball.y = TOP_CUSHION;
    if (ball.velocityY < 0.0f)
      ball.velocityY = -ball.velocityY;
  }
  else if (ball.y > BOTTOM_CUSHION) {
    ball.y = BOTTOM_CUSHION;
    if (ball.velocityY > 0.0f)
      ball.velocityY = -ball.velocityY;
  }
}

void collideBalls(Ball& first, Ball& second) {
  float dx = second.x - first.x;
  float dy = second.y - first.y;
  const float distanceSquared = dx * dx + dy * dy;
  if (distanceSquared >= BALL_DIAMETER * BALL_DIAMETER ||
      distanceSquared < 0.01f) {
    return;
  }

  const float distance = sqrtf(distanceSquared);
  dx /= distance;
  dy /= distance;
  const float overlap = BALL_DIAMETER - distance;
  first.x -= dx * overlap * 0.5f;
  first.y -= dy * overlap * 0.5f;
  second.x += dx * overlap * 0.5f;
  second.y += dy * overlap * 0.5f;

  const float relativeSpeed =
      (second.velocityX - first.velocityX) * dx +
      (second.velocityY - first.velocityY) * dy;
  if (relativeSpeed < 0.0f) {
    first.velocityX += relativeSpeed * dx;
    first.velocityY += relativeSpeed * dy;
    second.velocityX -= relativeSpeed * dx;
    second.velocityY -= relativeSpeed * dy;
  }
}

bool allBallsStopped() {
  for (const Ball& ball : balls) {
    if (ball.active &&
        (fabsf(ball.velocityX) > 0.035f || fabsf(ball.velocityY) > 0.035f)) {
      return false;
    }
  }
  return true;
}

bool cuePositionClear(float x, float y) {
  for (uint8_t index = 1; index < BALL_COUNT; index++) {
    if (!balls[index].active)
      continue;
    const float dx = balls[index].x - x;
    const float dy = balls[index].y - y;
    if (dx * dx + dy * dy < 49.0f)
      return false;
  }
  return true;
}

void respotCueBall() {
  float y = 72.0f;
  for (uint8_t attempt = 0; attempt < 7; attempt++) {
    if (cuePositionClear(28.0f, y)) {
      placeBall(0, 28.0f, y);
      return;
    }
    y += attempt % 2 == 0 ? 7.0f : -14.0f;
  }
  placeBall(0, 20.0f, 72.0f);
}

void updatePhysics() {
  // Two smaller simulation steps keep the stronger level-five shot from
  // tunneling through another six-pixel ball between displayed frames.
  for (uint8_t substep = 0; substep < 2; substep++) {
    for (uint8_t index = 0; index < BALL_COUNT; index++) {
      Ball& ball = balls[index];
      if (!ball.active)
        continue;
      ball.x += ball.velocityX * 0.5f;
      ball.y += ball.velocityY * 0.5f;
      if (nearPocket(ball.x, ball.y)) {
        pocketBall(index);
        continue;
      }
      bounceFromCushions(ball);
    }

    for (uint8_t first = 0; first < BALL_COUNT; first++) {
      if (!balls[first].active)
        continue;
      for (uint8_t second = first + 1; second < BALL_COUNT; second++) {
        if (balls[second].active)
          collideBalls(balls[first], balls[second]);
      }
    }
  }

  for (Ball& ball : balls) {
    if (!ball.active)
      continue;
    ball.velocityX *= 0.982f;
    ball.velocityY *= 0.982f;
    if (fabsf(ball.velocityX) < 0.025f)
      ball.velocityX = 0.0f;
    if (fabsf(ball.velocityY) < 0.025f)
      ball.velocityY = 0.0f;
  }

  if (gameState == GameState::Rolling && allBallsStopped()) {
    if (!balls[0].active)
      respotCueBall();
    gameState = GameState::Aiming;
  }
}

void adjustAim(uint32_t now) {
  const bool left = buttonDown(l_btn);
  const bool right = buttonDown(r_btn);
  // Keep the switch edge state synchronized while using raw levels for repeat.
  l_btn.justPressed();
  r_btn.justPressed();
  if (left == right || static_cast<int32_t>(now - nextAimStepAt) < 0)
    return;
  if (left)
    aimDegrees = (aimDegrees + 357) % 360;
  else
    aimDegrees = (aimDegrees + 3) % 360;
  nextAimStepAt = now + AIM_REPEAT_MS;
}

void shoot() {
  if (gameState != GameState::Aiming || !balls[0].active)
    return;
  const float speed = SHOT_SPEEDS[shotPower - 1];
  const float radians = aimDegrees * DEG_TO_RAD;
  balls[0].velocityX = cosf(radians) * speed;
  balls[0].velocityY = sinf(radians) * speed;
  shots++;
  gameState = GameState::Rolling;
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
  GameInput::TapHoldButton centerButton(c_btn);
  centerButton.reset();
  resetGame();
  uint32_t nextFrameAt = millis();

  while (true) {
    const uint32_t now = millis();
    const GameInput::CenterEvent centerEvent = centerButton.poll(now);
    if (centerEvent == GameInput::CenterEvent::Hold)
      break;

    if (gameFinished()) {
      if (retryPressed()) {
        resetGame();
        nextFrameAt = now;
      }
      delay(4);
      continue;
    }

    if (gameState == GameState::Aiming) {
      if (u_btn.justPressed() && shotPower < 5)
        shotPower++;
      if (d_btn.justPressed() && shotPower > 1)
        shotPower--;
      adjustAim(now);
      if (centerEvent == GameInput::CenterEvent::Tap)
        shoot();
    }

    if (static_cast<int32_t>(now - nextFrameAt) >= 0) {
      if (gameState == GameState::Rolling)
        updatePhysics();
      drawFrame();
      if (gameFinished())
        drawEndScreen();
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

}  // namespace EightBallGame

#else

namespace EightBallGame {
void run() {}
}  // namespace EightBallGame

#endif
