#include "SnakeGame.h"

#include "configs.h"

#if defined(HAS_SCREEN) && defined(HAS_BUTTONS) && \
    (U_BTN >= 0) && (D_BTN >= 0) && (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)

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

namespace SnakeGame {
namespace {

// Original fixed-orientation Mini V3 implementation. Gameplay concept inspired
// by https://github.com/derdacavga/Esp32-Snake-Game; no source was copied.
constexpr int16_t GAME_SCREEN_WIDTH = TFT_WIDTH;
constexpr int16_t GAME_SCREEN_HEIGHT = TFT_HEIGHT;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t CELL_SIZE = 6;
constexpr int16_t GRID_WIDTH = (GAME_SCREEN_WIDTH - 4) / CELL_SIZE;
constexpr int16_t GRID_HEIGHT = (GAME_SCREEN_HEIGHT - HEADER_HEIGHT - 4) / CELL_SIZE;
constexpr int16_t BOARD_WIDTH = GRID_WIDTH * CELL_SIZE;
constexpr int16_t BOARD_HEIGHT = GRID_HEIGHT * CELL_SIZE;
constexpr int16_t BOARD_X = (GAME_SCREEN_WIDTH - BOARD_WIDTH) / 2;
constexpr int16_t BOARD_Y = HEADER_HEIGHT + 2;
constexpr uint16_t MAX_SNAKE_LENGTH = GRID_WIDTH * GRID_HEIGHT;
constexpr uint16_t STARTING_MOVE_MS = 165;
constexpr uint16_t MINIMUM_MOVE_MS = 55;

constexpr uint16_t COLOR_BACKGROUND = TFT_BLACK;
constexpr uint16_t COLOR_BORDER = TFT_DARKGREY;
constexpr uint16_t COLOR_SNAKE = TFT_GREEN;
constexpr uint16_t COLOR_HEAD = TFT_YELLOW;
constexpr uint16_t COLOR_FOOD = TFT_RED;

struct Point {
  uint8_t x;
  uint8_t y;
};

enum class Direction : uint8_t {
  Up,
  Down,
  Left,
  Right,
};

Point snake[MAX_SNAKE_LENGTH];
uint16_t snakeLength = 0;
Point food{};
Direction direction = Direction::Right;
Direction travelDirection = Direction::Right;
uint16_t score = 0;
uint16_t moveInterval = STARTING_MOVE_MS;
bool gameOver = false;
bool boardFilled = false;
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

int16_t cellX(uint8_t x) {
  return BOARD_X + (x * CELL_SIZE);
}

int16_t cellY(uint8_t y) {
  return BOARD_Y + (y * CELL_SIZE);
}

void drawCell(const Point& point, uint16_t color, bool inset = true) {
  const int16_t insetAmount = inset ? 1 : 0;
  canvas().fillRect(cellX(point.x) + insetAmount,
                    cellY(point.y) + insetAmount,
                    CELL_SIZE - insetAmount,
                    CELL_SIZE - insetAmount,
                    color);
}

void drawHeader() {
  canvas().fillRect(0, 0, GAME_SCREEN_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_GREEN, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("SNAKE"));
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(68, 4);
  canvas().print(F("Score:"));
  canvas().print(score);
}

void drawBoard() {
  canvas().fillRect(0, HEADER_HEIGHT, GAME_SCREEN_WIDTH,
                    GAME_SCREEN_HEIGHT - HEADER_HEIGHT, COLOR_BACKGROUND);
  canvas().drawRect(BOARD_X - 1, BOARD_Y - 1,
                    BOARD_WIDTH + 2, BOARD_HEIGHT + 2, COLOR_BORDER);
  drawCell(food, COLOR_FOOD);
  for (uint16_t index = snakeLength; index > 0; index--)
    drawCell(snake[index - 1], index == 1 ? COLOR_HEAD : COLOR_SNAKE);
}

bool pointOnSnake(const Point& point) {
  for (uint16_t index = 0; index < snakeLength; index++) {
    if (snake[index].x == point.x && snake[index].y == point.y)
      return true;
  }
  return false;
}

bool spawnFood() {
  if (snakeLength >= MAX_SNAKE_LENGTH)
    return false;

  // Try random positions first, then fall back to a deterministic scan as the
  // board becomes crowded so food placement can never loop forever.
  for (uint16_t attempt = 0; attempt < MAX_SNAKE_LENGTH * 2; attempt++) {
    Point candidate{static_cast<uint8_t>(random(GRID_WIDTH)),
                    static_cast<uint8_t>(random(GRID_HEIGHT))};
    if (!pointOnSnake(candidate)) {
      food = candidate;
      return true;
    }
  }

  for (uint8_t y = 0; y < GRID_HEIGHT; y++) {
    for (uint8_t x = 0; x < GRID_WIDTH; x++) {
      Point candidate{x, y};
      if (!pointOnSnake(candidate)) {
        food = candidate;
        return true;
      }
    }
  }
  return false;
}

void resetGame(Direction startingDirection = Direction::Right) {
  snakeLength = 4;
  const uint8_t centerX = GRID_WIDTH / 2;
  const uint8_t centerY = GRID_HEIGHT / 2;
  snake[0] = Point{centerX, centerY};
  for (uint8_t index = 1; index < snakeLength; index++) {
    switch (startingDirection) {
      case Direction::Up:
        snake[index] = Point{centerX, static_cast<uint8_t>(centerY + index)};
        break;
      case Direction::Down:
        snake[index] = Point{centerX, static_cast<uint8_t>(centerY - index)};
        break;
      case Direction::Left:
        snake[index] = Point{static_cast<uint8_t>(centerX + index), centerY};
        break;
      case Direction::Right:
        snake[index] = Point{static_cast<uint8_t>(centerX - index), centerY};
        break;
    }
  }
  direction = startingDirection;
  travelDirection = startingDirection;
  score = 0;
  moveInterval = STARTING_MOVE_MS;
  gameOver = false;
  boardFilled = false;
  spawnFood();
  canvas().fillScreen(COLOR_BACKGROUND);
  drawHeader();
  drawBoard();
  presentFrame();
}

void drawEndScreen() {
  canvas().fillRect(8, 38, GAME_SCREEN_WIDTH - 16, 58, TFT_BLACK);
  canvas().drawRect(8, 38, GAME_SCREEN_WIDTH - 16, 58,
                    boardFilled ? TFT_GREEN : TFT_RED);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(boardFilled ? TFT_GREEN : TFT_RED, TFT_BLACK);
  canvas().drawString(boardFilled ? "YOU WIN" : "GAME OVER",
                      GAME_SCREEN_WIDTH / 2, 43, 2);
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().drawString(String(F("Score ")) + score,
                      GAME_SCREEN_WIDTH / 2, 63, 1);
  canvas().drawString("Direction: retry", GAME_SCREEN_WIDTH / 2, 76, 1);
  canvas().drawString("Center: exit", GAME_SCREEN_WIDTH / 2, 86, 1);
  canvas().setTextDatum(TL_DATUM);
}

bool isOpposite(Direction first, Direction second) {
  return (first == Direction::Up && second == Direction::Down) ||
         (first == Direction::Down && second == Direction::Up) ||
         (first == Direction::Left && second == Direction::Right) ||
         (first == Direction::Right && second == Direction::Left);
}

void acceptDirection(Direction requested) {
  if (!isOpposite(travelDirection, requested))
    direction = requested;
}

Point nextHead() {
  Point next = snake[0];
  switch (direction) {
    case Direction::Up: next.y--; break;
    case Direction::Down: next.y++; break;
    case Direction::Left: next.x--; break;
    case Direction::Right: next.x++; break;
  }
  return next;
}

void endGame(bool won) {
  gameOver = true;
  boardFilled = won;
  drawEndScreen();
  presentFrame();
}

void moveSnake() {
  const Point next = nextHead();
  if (next.x >= GRID_WIDTH || next.y >= GRID_HEIGHT) {
    endGame(false);
    return;
  }

  const bool growing = next.x == food.x && next.y == food.y;
  const uint16_t collisionLength = growing ? snakeLength : snakeLength - 1;
  for (uint16_t index = 0; index < collisionLength; index++) {
    if (snake[index].x == next.x && snake[index].y == next.y) {
      endGame(false);
      return;
    }
  }

  const Point oldHead = snake[0];
  const Point oldTail = snake[snakeLength - 1];
  if (growing) {
    for (uint16_t index = snakeLength; index > 0; index--)
      snake[index] = snake[index - 1];
    snakeLength++;
  }
  else {
    for (uint16_t index = snakeLength - 1; index > 0; index--)
      snake[index] = snake[index - 1];
  }
  snake[0] = next;
  travelDirection = direction;

  if (!growing)
    drawCell(oldTail, COLOR_BACKGROUND, false);
  drawCell(oldHead, COLOR_SNAKE);
  drawCell(snake[0], COLOR_HEAD);

  if (!growing) {
    presentFrame();
    return;
  }

  score += 10;
  drawHeader();
  uint16_t speedReduction = (score / 50) * 10;
  if (speedReduction > STARTING_MOVE_MS - MINIMUM_MOVE_MS)
    speedReduction = STARTING_MOVE_MS - MINIMUM_MOVE_MS;
  moveInterval = STARTING_MOVE_MS - speedReduction;
  if (!spawnFood()) {
    endGame(true);
    return;
  }
  drawCell(food, COLOR_FOOD);
  presentFrame();
}

bool retryPressed(Direction& requested) {
  if (u_btn.justPressed()) { requested = Direction::Up; return true; }
  if (d_btn.justPressed()) { requested = Direction::Down; return true; }
  if (l_btn.justPressed()) { requested = Direction::Left; return true; }
  if (r_btn.justPressed()) { requested = Direction::Right; return true; }
  return false;
}

}  // namespace

void run() {
  // The display remains at the firmware's configured orientation throughout.
  releaseControls();
  randomSeed(micros());
  GameFrameBuffer bufferedFrame(display_obj.tft);
  if (!bufferedFrame.begin(GAME_SCREEN_WIDTH, GAME_SCREEN_HEIGHT)) {
    display_obj.tft.fillScreen(TFT_BLACK);
    display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
    display_obj.tft.drawString("Frame buffer error", 8, 56, 1);
    releaseControls();
    return;
  }
  frameBuffer = &bufferedFrame;
  resetGame();
  uint32_t nextMoveAt = millis() + moveInterval;

  while (true) {
    if (c_btn.justPressed())
      break;

    if (gameOver) {
      Direction requested = Direction::Right;
      if (retryPressed(requested)) {
        resetGame(requested);
        nextMoveAt = millis() + moveInterval;
      }
      delay(5);
      continue;
    }

    if (u_btn.justPressed()) acceptDirection(Direction::Up);
    if (d_btn.justPressed()) acceptDirection(Direction::Down);
    if (l_btn.justPressed()) acceptDirection(Direction::Left);
    if (r_btn.justPressed()) acceptDirection(Direction::Right);

    const uint32_t now = millis();
    if (static_cast<int32_t>(now - nextMoveAt) >= 0) {
      moveSnake();
      nextMoveAt = now + moveInterval;
    }
    delay(5);
  }

  frameBuffer = nullptr;
  releaseButton(c_btn);
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace SnakeGame

#else

namespace SnakeGame {
void run() {}
}  // namespace SnakeGame

#endif
