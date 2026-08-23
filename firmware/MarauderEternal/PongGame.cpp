#include "PongGame.h"

#include "configs.h"

#if defined(HAS_SCREEN) && defined(HAS_BUTTONS) && \
    (U_BTN >= 0) && (D_BTN >= 0) && (C_BTN >= 0)

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

namespace PongGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t GAME_HEIGHT = TFT_HEIGHT;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t PLAY_TOP = HEADER_HEIGHT + 1;
constexpr int16_t PADDLE_WIDTH = 3;
constexpr int16_t PADDLE_HEIGHT = 19;
constexpr int16_t PLAYER_X = 3;
constexpr int16_t AI_X = GAME_WIDTH - PADDLE_WIDTH - 3;
constexpr int16_t BALL_SIZE = 4;
constexpr int16_t PLAYER_SPEED = 1;
constexpr int16_t FIXED_SCALE = 16;
constexpr int16_t STARTING_BALL_SPEED = 22;
constexpr int16_t MAXIMUM_BALL_SPEED = 34;
constexpr uint16_t FRAME_INTERVAL_MS = 24;
constexpr uint16_t PLAYER_MOVE_INTERVAL_MS = 18;
constexpr uint16_t AI_INTERVAL_MS = 28;
constexpr uint16_t SERVE_DELAY_MS = 650;
constexpr uint8_t WINNING_SCORE = 7;

int16_t playerY = 0;
int16_t aiY = 0;
int16_t ballX = 0;
int16_t ballY = 0;
int16_t ballVelocityX = STARTING_BALL_SPEED;
int16_t ballVelocityY = 10;
int8_t aiAimError = 0;
uint8_t playerScore = 0;
uint8_t aiScore = 0;
uint32_t serveAt = 0;
uint32_t nextPlayerMoveAt = 0;
uint32_t nextAiMoveAt = 0;
bool matchFinished = false;
bool playerWon = false;
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

int16_t clampPaddle(int16_t y) {
  if (y < PLAY_TOP)
    return PLAY_TOP;
  const int16_t maximum = GAME_HEIGHT - PADDLE_HEIGHT;
  return y > maximum ? maximum : y;
}

void drawHeader() {
  canvas().fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_GREEN, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("YOU "));
  canvas().print(playerScore);
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(51, 4);
  canvas().print(F("PONG"));
  canvas().setTextColor(TFT_RED, TFT_NAVY);
  canvas().setCursor(96, 4);
  canvas().print(F("AI "));
  canvas().print(aiScore);
}

void drawFrame() {
  canvas().fillRect(0, PLAY_TOP, GAME_WIDTH,
                    GAME_HEIGHT - PLAY_TOP, TFT_BLACK);
  for (int16_t y = PLAY_TOP + 3; y < GAME_HEIGHT; y += 9)
    canvas().fillRect((GAME_WIDTH / 2) - 1, y, 2, 4, TFT_DARKGREY);

  canvas().fillRect(PLAYER_X, playerY, PADDLE_WIDTH,
                    PADDLE_HEIGHT, TFT_GREEN);
  canvas().fillRect(AI_X, aiY, PADDLE_WIDTH,
                    PADDLE_HEIGHT, TFT_RED);
  canvas().fillRect(ballX / FIXED_SCALE, ballY / FIXED_SCALE,
                    BALL_SIZE, BALL_SIZE, TFT_WHITE);
  presentFrame();
}

void chooseVerticalVelocity() {
  ballVelocityY = static_cast<int16_t>(random(-18, 19));
  if (ballVelocityY >= -6 && ballVelocityY <= 6)
    ballVelocityY = ballVelocityY < 0 ? -10 : 10;
}

void prepareServe(int8_t horizontalDirection) {
  ballX = ((GAME_WIDTH - BALL_SIZE) / 2) * FIXED_SCALE;
  ballY = ((PLAY_TOP + GAME_HEIGHT - BALL_SIZE) / 2) * FIXED_SCALE;
  ballVelocityX = horizontalDirection * STARTING_BALL_SPEED;
  chooseVerticalVelocity();
  aiAimError = static_cast<int8_t>(random(-8, 9));
  serveAt = millis() + SERVE_DELAY_MS;
}

void resetMatch() {
  playerScore = 0;
  aiScore = 0;
  matchFinished = false;
  playerWon = false;
  playerY = clampPaddle((GAME_HEIGHT - PADDLE_HEIGHT + PLAY_TOP) / 2);
  aiY = playerY;
  nextPlayerMoveAt = millis();
  canvas().fillScreen(TFT_BLACK);
  drawHeader();
  prepareServe(random(0, 2) == 0 ? -1 : 1);
  drawFrame();
}

void setBounceFromPaddle(int16_t paddleY) {
  const int16_t ballCenter = (ballY / FIXED_SCALE) + (BALL_SIZE / 2);
  const int16_t paddleCenter = paddleY + (PADDLE_HEIGHT / 2);
  ballVelocityY = (ballCenter - paddleCenter) * 3;
  if (ballVelocityY >= -5 && ballVelocityY <= 5)
    ballVelocityY = ballVelocityY < 0 ? -6 : 6;
  if (ballVelocityY > 30) ballVelocityY = 30;
  if (ballVelocityY < -30) ballVelocityY = -30;
}

void finishMatch(bool won) {
  matchFinished = true;
  playerWon = won;
  canvas().fillRect(12, 43, GAME_WIDTH - 24, 49, TFT_BLACK);
  canvas().drawRect(12, 43, GAME_WIDTH - 24, 49,
                    won ? TFT_GREEN : TFT_RED);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(won ? TFT_GREEN : TFT_RED, TFT_BLACK);
  canvas().drawString(won ? "YOU WIN" : "AI WINS",
                      GAME_WIDTH / 2, 48, 2);
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().drawString("Direction: retry", GAME_WIDTH / 2, 69, 1);
  canvas().drawString("Center: exit", GAME_WIDTH / 2, 80, 1);
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void scorePoint(bool playerPoint) {
  if (playerPoint)
    playerScore++;
  else
    aiScore++;
  drawHeader();

  if (playerScore >= WINNING_SCORE || aiScore >= WINNING_SCORE) {
    finishMatch(playerScore >= WINNING_SCORE);
    return;
  }
  prepareServe(playerPoint ? 1 : -1);
}

void moveComputerPaddle(uint32_t now) {
  if (static_cast<int32_t>(now - nextAiMoveAt) < 0)
    return;
  nextAiMoveAt = now + AI_INTERVAL_MS;

  int16_t target = (PLAY_TOP + GAME_HEIGHT) / 2;
  if (ballVelocityX > 0 && static_cast<int32_t>(now - serveAt) >= 0)
    target = (ballY / FIXED_SCALE) + (BALL_SIZE / 2) + aiAimError;

  const int16_t center = aiY + (PADDLE_HEIGHT / 2);
  if (center < target - 2)
    aiY = clampPaddle(aiY + 1);
  else if (center > target + 2)
    aiY = clampPaddle(aiY - 1);
}

void movePlayerPaddle(uint32_t now) {
  if (static_cast<int32_t>(now - nextPlayerMoveAt) < 0)
    return;

  const bool movingUp = buttonDown(u_btn);
  const bool movingDown = buttonDown(d_btn);
  if (movingUp != movingDown) {
    playerY = clampPaddle(playerY + (movingUp ? -PLAYER_SPEED : PLAYER_SPEED));
    nextPlayerMoveAt = now + PLAYER_MOVE_INTERVAL_MS;
  }
}

void moveBall(uint32_t now) {
  if (static_cast<int32_t>(now - serveAt) < 0)
    return;

  ballX += ballVelocityX;
  ballY += ballVelocityY;

  const int16_t topLimit = PLAY_TOP * FIXED_SCALE;
  const int16_t bottomLimit = (GAME_HEIGHT - BALL_SIZE) * FIXED_SCALE;
  if (ballY < topLimit) {
    ballY = topLimit;
    ballVelocityY = -ballVelocityY;
  }
  else if (ballY > bottomLimit) {
    ballY = bottomLimit;
    ballVelocityY = -ballVelocityY;
  }

  int16_t pixelX = ballX / FIXED_SCALE;
  const int16_t pixelY = ballY / FIXED_SCALE;
  const bool overlapsPlayer = pixelY + BALL_SIZE >= playerY &&
                              pixelY <= playerY + PADDLE_HEIGHT;
  if (ballVelocityX < 0 && pixelX <= PLAYER_X + PADDLE_WIDTH &&
      pixelX + BALL_SIZE >= PLAYER_X && overlapsPlayer) {
    ballX = (PLAYER_X + PADDLE_WIDTH) * FIXED_SCALE;
    ballVelocityX = -ballVelocityX;
    if (ballVelocityX < MAXIMUM_BALL_SPEED) ballVelocityX++;
    setBounceFromPaddle(playerY);
    aiAimError = static_cast<int8_t>(random(-9, 10));
  }

  pixelX = ballX / FIXED_SCALE;
  const bool overlapsAi = pixelY + BALL_SIZE >= aiY &&
                          pixelY <= aiY + PADDLE_HEIGHT;
  if (ballVelocityX > 0 && pixelX + BALL_SIZE >= AI_X &&
      pixelX <= AI_X + PADDLE_WIDTH && overlapsAi) {
    ballX = (AI_X - BALL_SIZE) * FIXED_SCALE;
    ballVelocityX = -ballVelocityX;
    if (ballVelocityX > -MAXIMUM_BALL_SPEED) ballVelocityX--;
    setBounceFromPaddle(aiY);
  }

  pixelX = ballX / FIXED_SCALE;
  if (pixelX + BALL_SIZE < 0)
    scorePoint(false);
  else if (pixelX >= GAME_WIDTH)
    scorePoint(true);
}

bool retryPressed() {
  return u_btn.justPressed() || d_btn.justPressed() ||
         l_btn.justPressed() || r_btn.justPressed();
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
  resetMatch();
  uint32_t nextFrameAt = millis();
  nextAiMoveAt = nextFrameAt;

  while (true) {
    if (c_btn.justPressed())
      break;

    if (matchFinished) {
      if (retryPressed()) {
        resetMatch();
        nextFrameAt = millis();
      }
      delay(5);
      continue;
    }

    const uint32_t now = millis();
    movePlayerPaddle(now);
    moveComputerPaddle(now);
    if (static_cast<int32_t>(now - nextFrameAt) >= 0) {
      moveBall(now);
      if (!matchFinished)
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

}  // namespace PongGame

#else

namespace PongGame {
void run() {}
}

#endif
