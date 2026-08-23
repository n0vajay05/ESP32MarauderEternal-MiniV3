#include "BreakoutGame.h"

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

namespace BreakoutGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t GAME_HEIGHT = TFT_HEIGHT;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t PLAY_TOP = HEADER_HEIGHT + 1;

constexpr uint8_t BRICK_ROWS = 5;
constexpr uint8_t BRICK_COLUMNS = 8;
constexpr int16_t BRICK_MARGIN_X = 4;
constexpr int16_t BRICK_TOP = 23;
constexpr int16_t BRICK_GAP_X = 1;
constexpr int16_t BRICK_GAP_Y = 2;
constexpr int16_t BRICK_HEIGHT = 6;
constexpr int16_t BRICK_WIDTH =
    (GAME_WIDTH - (2 * BRICK_MARGIN_X) -
     ((BRICK_COLUMNS - 1) * BRICK_GAP_X)) /
    BRICK_COLUMNS;

constexpr int16_t PADDLE_WIDTH = 27;
constexpr int16_t PADDLE_HEIGHT = 4;
constexpr int16_t PADDLE_Y = GAME_HEIGHT - PADDLE_HEIGHT - 4;
constexpr int16_t PADDLE_SPEED = 3;
constexpr int16_t BALL_SIZE = 3;

constexpr int16_t FIXED_SCALE = 32;
constexpr int16_t STARTING_BALL_X_SPEED = 32;
constexpr int16_t STARTING_BALL_Y_SPEED = 48;
constexpr int16_t MINIMUM_BALL_X_SPEED = 18;
constexpr int16_t MAXIMUM_BALL_X_SPEED = 58;
constexpr uint16_t FRAME_INTERVAL_MS = 18;
constexpr uint8_t STARTING_LIVES = 3;

enum class GameState : uint8_t {
  Ready,
  Playing,
  Won,
  Lost,
};

uint8_t bricks[BRICK_ROWS]{};
uint8_t bricksRemaining = 0;
uint8_t lives = STARTING_LIVES;
uint16_t score = 0;
int16_t paddleX = 0;
int16_t ballX = 0;
int16_t ballY = 0;
int16_t ballVelocityX = STARTING_BALL_X_SPEED;
int16_t ballVelocityY = -STARTING_BALL_Y_SPEED;
GameState gameState = GameState::Ready;
GameFrameBuffer* frameBuffer = nullptr;

TFT_eSprite& canvas() {
  return frameBuffer->canvas();
}

void presentFrame() {
  frameBuffer->present();
}

const uint16_t BRICK_COLORS[BRICK_ROWS] = {
    TFT_RED, TFT_ORANGE, TFT_YELLOW, TFT_GREEN, TFT_CYAN};

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

int16_t brickX(uint8_t column) {
  return BRICK_MARGIN_X + column * (BRICK_WIDTH + BRICK_GAP_X);
}

int16_t brickY(uint8_t row) {
  return BRICK_TOP + row * (BRICK_HEIGHT + BRICK_GAP_Y);
}

bool brickAlive(uint8_t row, uint8_t column) {
  return (bricks[row] & (1U << column)) != 0;
}

void removeBrick(uint8_t row, uint8_t column) {
  bricks[row] &= static_cast<uint8_t>(~(1U << column));
  bricksRemaining--;
  score += 10;
}

int16_t clampPaddle(int16_t x) {
  constexpr int16_t MINIMUM_X = 2;
  constexpr int16_t MAXIMUM_X = GAME_WIDTH - PADDLE_WIDTH - 2;
  if (x < MINIMUM_X)
    return MINIMUM_X;
  return x > MAXIMUM_X ? MAXIMUM_X : x;
}

void placeBallOnPaddle() {
  ballX = (paddleX + ((PADDLE_WIDTH - BALL_SIZE) / 2)) * FIXED_SCALE;
  ballY = (PADDLE_Y - BALL_SIZE - 1) * FIXED_SCALE;
}

void drawHeader() {
  canvas().fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_ORANGE, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("BREAKOUT"));
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(55, 4);
  canvas().print(F("S:"));
  canvas().print(score);
  canvas().setCursor(106, 4);
  canvas().print(F("L:"));
  canvas().print(lives);
}

void drawFrame() {
  canvas().fillRect(0, PLAY_TOP, GAME_WIDTH,
                    GAME_HEIGHT - PLAY_TOP, TFT_BLACK);

  for (uint8_t row = 0; row < BRICK_ROWS; row++) {
    for (uint8_t column = 0; column < BRICK_COLUMNS; column++) {
      if (brickAlive(row, column)) {
        canvas().fillRect(brickX(column), brickY(row), BRICK_WIDTH,
                          BRICK_HEIGHT, BRICK_COLORS[row]);
      }
    }
  }

  canvas().fillRect(paddleX, PADDLE_Y, PADDLE_WIDTH,
                    PADDLE_HEIGHT, TFT_WHITE);
  canvas().fillRect(ballX / FIXED_SCALE, ballY / FIXED_SCALE,
                    BALL_SIZE, BALL_SIZE, TFT_ORANGE);

  if (gameState == GameState::Ready) {
    canvas().setTextDatum(TC_DATUM);
    canvas().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    canvas().drawString("UP: LAUNCH", GAME_WIDTH / 2, 101, 1);
    canvas().setTextDatum(TL_DATUM);
  }
  presentFrame();
}

void drawEndScreen() {
  const bool won = gameState == GameState::Won;
  canvas().fillRect(8, 43, GAME_WIDTH - 16, 56, TFT_BLACK);
  canvas().drawRect(8, 43, GAME_WIDTH - 16, 56,
                    won ? TFT_GREEN : TFT_RED);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(won ? TFT_GREEN : TFT_RED, TFT_BLACK);
  canvas().drawString(won ? "YOU WIN" : "GAME OVER",
                      GAME_WIDTH / 2, 48, 2);
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().drawString(String(F("Score ")) + score,
                      GAME_WIDTH / 2, 68, 1);
  canvas().drawString("Direction: retry", GAME_WIDTH / 2, 79, 1);
  canvas().drawString("Center: exit", GAME_WIDTH / 2, 89, 1);
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void resetBall() {
  paddleX = clampPaddle((GAME_WIDTH - PADDLE_WIDTH) / 2);
  placeBallOnPaddle();
  ballVelocityX = random(0, 2) == 0 ? -STARTING_BALL_X_SPEED
                                    : STARTING_BALL_X_SPEED;
  ballVelocityY = -STARTING_BALL_Y_SPEED;
  gameState = GameState::Ready;
}

void resetGame() {
  for (uint8_t row = 0; row < BRICK_ROWS; row++)
    bricks[row] = 0xFF;
  bricksRemaining = BRICK_ROWS * BRICK_COLUMNS;
  lives = STARTING_LIVES;
  score = 0;
  resetBall();
  canvas().fillScreen(TFT_BLACK);
  drawHeader();
  drawFrame();
}

void launchBall() {
  if (gameState == GameState::Ready)
    gameState = GameState::Playing;
}

void movePaddle() {
  const bool movingLeft = buttonDown(l_btn);
  const bool movingRight = buttonDown(r_btn);
  if (movingLeft != movingRight) {
    paddleX = clampPaddle(paddleX + (movingLeft ? -PADDLE_SPEED
                                                : PADDLE_SPEED));
  }
  if (gameState == GameState::Ready)
    placeBallOnPaddle();
}

void bounceHorizontally(bool towardRight) {
  if (towardRight) {
    if (ballVelocityX < 0)
      ballVelocityX = -ballVelocityX;
  }
  else if (ballVelocityX > 0) {
    ballVelocityX = -ballVelocityX;
  }
}

void bounceVertically(bool downward) {
  if (downward) {
    if (ballVelocityY < 0)
      ballVelocityY = -ballVelocityY;
  }
  else if (ballVelocityY > 0) {
    ballVelocityY = -ballVelocityY;
  }
}

void hitBrick(int16_t oldPixelX, int16_t oldPixelY) {
  const int16_t pixelX = ballX / FIXED_SCALE;
  const int16_t pixelY = ballY / FIXED_SCALE;
  const int16_t ballRight = pixelX + BALL_SIZE;
  const int16_t ballBottom = pixelY + BALL_SIZE;
  const int16_t oldRight = oldPixelX + BALL_SIZE;
  const int16_t oldBottom = oldPixelY + BALL_SIZE;

  for (uint8_t row = 0; row < BRICK_ROWS; row++) {
    for (uint8_t column = 0; column < BRICK_COLUMNS; column++) {
      if (!brickAlive(row, column))
        continue;

      const int16_t left = brickX(column);
      const int16_t top = brickY(row);
      const int16_t right = left + BRICK_WIDTH;
      const int16_t bottom = top + BRICK_HEIGHT;
      if (ballRight <= left || pixelX >= right || ballBottom <= top ||
          pixelY >= bottom) {
        continue;
      }

      removeBrick(row, column);
      drawHeader();
      if (oldBottom <= top) {
        ballY = (top - BALL_SIZE) * FIXED_SCALE;
        bounceVertically(false);
      }
      else if (oldPixelY >= bottom) {
        ballY = bottom * FIXED_SCALE;
        bounceVertically(true);
      }
      else if (oldRight <= left) {
        ballX = (left - BALL_SIZE) * FIXED_SCALE;
        bounceHorizontally(false);
      }
      else if (oldPixelX >= right) {
        ballX = right * FIXED_SCALE;
        bounceHorizontally(true);
      }
      else {
        const int16_t horizontalOverlap =
            min(ballRight - left, right - pixelX);
        const int16_t verticalOverlap =
            min(ballBottom - top, bottom - pixelY);
        if (horizontalOverlap < verticalOverlap)
          ballVelocityX = -ballVelocityX;
        else
          ballVelocityY = -ballVelocityY;
      }
      return;
    }
  }
}

void bounceFromPaddle(int16_t oldPixelY) {
  if (ballVelocityY <= 0)
    return;

  const int16_t pixelX = ballX / FIXED_SCALE;
  const int16_t pixelY = ballY / FIXED_SCALE;
  if (oldPixelY + BALL_SIZE > PADDLE_Y ||
      pixelY + BALL_SIZE < PADDLE_Y ||
      pixelX + BALL_SIZE <= paddleX ||
      pixelX >= paddleX + PADDLE_WIDTH) {
    return;
  }

  ballY = (PADDLE_Y - BALL_SIZE) * FIXED_SCALE;
  ballVelocityY = -STARTING_BALL_Y_SPEED;

  const int16_t ballCenter = pixelX + (BALL_SIZE / 2);
  const int16_t paddleCenter = paddleX + (PADDLE_WIDTH / 2);
  int16_t newVelocityX = (ballCenter - paddleCenter) * 4;
  if (newVelocityX > MAXIMUM_BALL_X_SPEED)
    newVelocityX = MAXIMUM_BALL_X_SPEED;
  if (newVelocityX < -MAXIMUM_BALL_X_SPEED)
    newVelocityX = -MAXIMUM_BALL_X_SPEED;
  if (newVelocityX > -MINIMUM_BALL_X_SPEED &&
      newVelocityX < MINIMUM_BALL_X_SPEED) {
    newVelocityX = ballVelocityX < 0 ? -MINIMUM_BALL_X_SPEED
                                     : MINIMUM_BALL_X_SPEED;
  }
  ballVelocityX = newVelocityX;
}

void finishGame(GameState result) {
  gameState = result;
  drawHeader();
  drawEndScreen();
}

void loseBall() {
  if (lives > 0)
    lives--;
  drawHeader();
  if (lives == 0) {
    finishGame(GameState::Lost);
    return;
  }
  resetBall();
}

void moveBall() {
  const int16_t oldPixelX = ballX / FIXED_SCALE;
  const int16_t oldPixelY = ballY / FIXED_SCALE;
  ballX += ballVelocityX;
  ballY += ballVelocityY;

  const int16_t maximumX = (GAME_WIDTH - BALL_SIZE) * FIXED_SCALE;
  const int16_t minimumY = PLAY_TOP * FIXED_SCALE;
  if (ballX < 0) {
    ballX = 0;
    bounceHorizontally(true);
  }
  else if (ballX > maximumX) {
    ballX = maximumX;
    bounceHorizontally(false);
  }
  if (ballY < minimumY) {
    ballY = minimumY;
    bounceVertically(true);
  }

  hitBrick(oldPixelX, oldPixelY);
  if (bricksRemaining == 0) {
    finishGame(GameState::Won);
    return;
  }

  bounceFromPaddle(oldPixelY);
  if (ballY / FIXED_SCALE >= GAME_HEIGHT)
    loseBall();
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
  // The Mini V3 remains in its configured fixed orientation.
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

    if (gameState == GameState::Ready && u_btn.justPressed())
      launchBall();

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextFrameAt) >= 0) {
      movePaddle();
      if (gameState == GameState::Playing)
        moveBall();
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

}  // namespace BreakoutGame

#else

namespace BreakoutGame {
void run() {}
}  // namespace BreakoutGame

#endif
