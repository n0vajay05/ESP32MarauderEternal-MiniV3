#include "TetrisGame.h"

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

namespace TetrisGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t GAME_HEIGHT = TFT_HEIGHT;
constexpr uint8_t BOARD_COLUMNS = 10;
constexpr uint8_t BOARD_ROWS = 18;
constexpr int16_t CELL_SIZE = 6;
constexpr int16_t BOARD_X = 3;
constexpr int16_t BOARD_Y = 18;
constexpr int16_t BOARD_WIDTH = BOARD_COLUMNS * CELL_SIZE;
constexpr int16_t BOARD_HEIGHT = BOARD_ROWS * CELL_SIZE;
constexpr uint16_t FRAME_INTERVAL_MS = 40;
constexpr uint16_t STARTING_DROP_MS = 650;
constexpr uint16_t MINIMUM_DROP_MS = 140;
constexpr uint16_t HORIZONTAL_REPEAT_DELAY_MS = 180;
constexpr uint16_t HORIZONTAL_REPEAT_MS = 75;
constexpr uint16_t SOFT_DROP_MS = 45;

// Four row-major 4x4 rotations for I, O, T, S, Z, J, and L pieces.
const uint16_t PIECE_MASKS[7][4] = {
    {0x0F00, 0x2222, 0x00F0, 0x4444},
    {0x6600, 0x6600, 0x6600, 0x6600},
    {0x4E00, 0x4640, 0x0E40, 0x4C40},
    {0x6C00, 0x4620, 0x06C0, 0x8C40},
    {0xC600, 0x2640, 0x0C60, 0x4C80},
    {0x8E00, 0x6440, 0x0E20, 0x44C0},
    {0x2E00, 0x4460, 0x0E80, 0xC440},
};

const uint16_t PIECE_COLORS[7] = {
    TFT_CYAN, TFT_YELLOW, TFT_MAGENTA, TFT_GREEN,
    TFT_RED, TFT_BLUE, TFT_ORANGE};

enum class GameState : uint8_t {
  Playing,
  GameOver,
};

uint8_t board[BOARD_ROWS][BOARD_COLUMNS]{};
uint8_t currentPiece = 0;
uint8_t currentRotation = 0;
uint8_t nextPiece = 0;
int8_t pieceX = 0;
int8_t pieceY = 0;
uint32_t score = 0;
uint16_t clearedLines = 0;
uint16_t dropInterval = STARTING_DROP_MS;
int8_t horizontalDirection = 0;
uint32_t nextHorizontalMoveAt = 0;
uint32_t nextSoftDropAt = 0;
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

bool pieceCell(uint8_t type, uint8_t rotation, uint8_t row, uint8_t column) {
  const uint8_t bit = row * 4 + column;
  return (PIECE_MASKS[type][rotation] & (0x8000U >> bit)) != 0;
}

bool pieceFits(uint8_t type, uint8_t rotation, int8_t x, int8_t y) {
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t column = 0; column < 4; column++) {
      if (!pieceCell(type, rotation, row, column))
        continue;
      const int8_t boardX = x + column;
      const int8_t boardY = y + row;
      if (boardX < 0 || boardX >= BOARD_COLUMNS || boardY >= BOARD_ROWS)
        return false;
      if (boardY >= 0 && board[boardY][boardX] != 0)
        return false;
    }
  }
  return true;
}

void drawCell(int8_t x, int8_t y, uint16_t color, int16_t size = CELL_SIZE) {
  if (x < 0 || y < 0)
    return;
  canvas().fillRect(BOARD_X + x * size + 1,
                    BOARD_Y + y * size + 1,
                    size - 1, size - 1, color);
}

void drawPiece(uint8_t type, uint8_t rotation, int8_t x, int8_t y) {
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t column = 0; column < 4; column++) {
      if (pieceCell(type, rotation, row, column))
        drawCell(x + column, y + row, PIECE_COLORS[type]);
    }
  }
}

void drawPreview() {
  constexpr int16_t PREVIEW_X = 83;
  constexpr int16_t PREVIEW_Y = 82;
  constexpr int16_t PREVIEW_CELL = 5;
  canvas().fillRect(68, 68, GAME_WIDTH - 68, 40, TFT_BLACK);
  canvas().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas().setCursor(69, 69);
  canvas().print(F("NEXT"));
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t column = 0; column < 4; column++) {
      if (pieceCell(nextPiece, 0, row, column)) {
        canvas().fillRect(PREVIEW_X + column * PREVIEW_CELL,
                          PREVIEW_Y + row * PREVIEW_CELL,
                          PREVIEW_CELL - 1, PREVIEW_CELL - 1,
                          PIECE_COLORS[nextPiece]);
      }
    }
  }
}

void drawSidebar() {
  canvas().fillRect(65, 0, GAME_WIDTH - 65, GAME_HEIGHT, TFT_BLACK);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_CYAN, TFT_BLACK);
  canvas().setCursor(69, 5);
  canvas().print(F("TETRIS"));
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().setCursor(69, 23);
  canvas().print(F("SCORE"));
  canvas().setCursor(69, 34);
  canvas().print(score);
  canvas().setCursor(69, 49);
  canvas().print(F("LINES"));
  canvas().setCursor(69, 60);
  canvas().print(clearedLines);
  drawPreview();
  canvas().setTextColor(TFT_DARKGREY, TFT_BLACK);
  canvas().setCursor(69, 115);
  canvas().print(F("C:EXIT"));
}

void drawBoard() {
  canvas().fillRect(0, 0, 65, GAME_HEIGHT, TFT_BLACK);
  canvas().drawRect(BOARD_X - 1, BOARD_Y - 1,
                    BOARD_WIDTH + 2, BOARD_HEIGHT + 2, TFT_DARKGREY);
  for (uint8_t row = 0; row < BOARD_ROWS; row++) {
    for (uint8_t column = 0; column < BOARD_COLUMNS; column++) {
      if (board[row][column] != 0)
        drawCell(column, row, PIECE_COLORS[board[row][column] - 1]);
    }
  }
  if (gameState == GameState::Playing)
    drawPiece(currentPiece, currentRotation, pieceX, pieceY);
}

void drawGame() {
  drawBoard();
  drawSidebar();
  presentFrame();
}

void drawGameOver() {
  canvas().fillRect(8, 40, GAME_WIDTH - 16, 57, TFT_BLACK);
  canvas().drawRect(8, 40, GAME_WIDTH - 16, 57, TFT_RED);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(TFT_RED, TFT_BLACK);
  canvas().drawString("GAME OVER", GAME_WIDTH / 2, 45, 2);
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().drawString(String(F("Score ")) + score,
                      GAME_WIDTH / 2, 66, 1);
  canvas().drawString("Direction: retry", GAME_WIDTH / 2, 78, 1);
  canvas().drawString("Center: exit", GAME_WIDTH / 2, 88, 1);
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void spawnPiece() {
  currentPiece = nextPiece;
  nextPiece = static_cast<uint8_t>(random(0, 7));
  currentRotation = 0;
  pieceX = 3;
  pieceY = 0;
  if (!pieceFits(currentPiece, currentRotation, pieceX, pieceY)) {
    gameState = GameState::GameOver;
    drawGame();
    drawGameOver();
  }
}

uint8_t clearFullLines() {
  uint8_t count = 0;
  for (int8_t row = BOARD_ROWS - 1; row >= 0; row--) {
    bool full = true;
    for (uint8_t column = 0; column < BOARD_COLUMNS; column++) {
      if (board[row][column] == 0) {
        full = false;
        break;
      }
    }
    if (!full)
      continue;

    count++;
    for (int8_t copyRow = row; copyRow > 0; copyRow--) {
      for (uint8_t column = 0; column < BOARD_COLUMNS; column++)
        board[copyRow][column] = board[copyRow - 1][column];
    }
    for (uint8_t column = 0; column < BOARD_COLUMNS; column++)
      board[0][column] = 0;
    row++;
  }
  return count;
}

void updateScoreForLines(uint8_t count) {
  static const uint16_t LINE_SCORES[5] = {0, 100, 300, 500, 800};
  score += LINE_SCORES[count];
  clearedLines += count;
  uint16_t reduction = clearedLines * 12;
  if (reduction > STARTING_DROP_MS - MINIMUM_DROP_MS)
    reduction = STARTING_DROP_MS - MINIMUM_DROP_MS;
  dropInterval = STARTING_DROP_MS - reduction;
}

void lockPiece() {
  for (uint8_t row = 0; row < 4; row++) {
    for (uint8_t column = 0; column < 4; column++) {
      if (!pieceCell(currentPiece, currentRotation, row, column))
        continue;
      const int8_t boardY = pieceY + row;
      const int8_t boardX = pieceX + column;
      if (boardY < 0) {
        gameState = GameState::GameOver;
        drawGameOver();
        return;
      }
      board[boardY][boardX] = currentPiece + 1;
    }
  }
  updateScoreForLines(clearFullLines());
  spawnPiece();
}

bool movePiece(int8_t deltaX, int8_t deltaY) {
  if (!pieceFits(currentPiece, currentRotation,
                 pieceX + deltaX, pieceY + deltaY)) {
    return false;
  }
  pieceX += deltaX;
  pieceY += deltaY;
  return true;
}

void rotatePiece() {
  const uint8_t newRotation = (currentRotation + 1) & 0x03;
  static const int8_t KICKS[] = {0, -1, 1, -2, 2};
  for (int8_t kick : KICKS) {
    if (pieceFits(currentPiece, newRotation, pieceX + kick, pieceY)) {
      currentRotation = newRotation;
      pieceX += kick;
      return;
    }
  }
}

void handleHorizontalInput(uint32_t now) {
  int8_t requested = 0;
  const bool left = buttonDown(l_btn);
  const bool right = buttonDown(r_btn);
  if (left != right)
    requested = left ? -1 : 1;

  if (requested == 0) {
    horizontalDirection = 0;
    return;
  }
  if (requested != horizontalDirection) {
    horizontalDirection = requested;
    movePiece(requested, 0);
    nextHorizontalMoveAt = now + HORIZONTAL_REPEAT_DELAY_MS;
    return;
  }
  if (static_cast<int32_t>(now - nextHorizontalMoveAt) >= 0) {
    movePiece(requested, 0);
    nextHorizontalMoveAt = now + HORIZONTAL_REPEAT_MS;
  }
}

void resetGame() {
  memset(board, 0, sizeof(board));
  score = 0;
  clearedLines = 0;
  dropInterval = STARTING_DROP_MS;
  horizontalDirection = 0;
  gameState = GameState::Playing;
  nextPiece = static_cast<uint8_t>(random(0, 7));
  spawnPiece();
  canvas().fillScreen(TFT_BLACK);
  drawGame();
}

bool retryPressed() {
  return u_btn.justPressed() || d_btn.justPressed() ||
         l_btn.justPressed() || r_btn.justPressed();
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
  uint32_t now = millis();
  uint32_t nextFrameAt = now;
  uint32_t nextDropAt = now + dropInterval;
  nextSoftDropAt = now;

  while (true) {
    if (c_btn.justPressed())
      break;

    if (gameState == GameState::GameOver) {
      if (retryPressed()) {
        resetGame();
        now = millis();
        nextFrameAt = now;
        nextDropAt = now + dropInterval;
      }
      delay(5);
      continue;
    }

    now = millis();
    handleHorizontalInput(now);
    if (u_btn.justPressed())
      rotatePiece();

    if (buttonDown(d_btn) &&
        static_cast<int32_t>(now - nextSoftDropAt) >= 0) {
      if (movePiece(0, 1))
        score++;
      else
        lockPiece();
      nextSoftDropAt = now + SOFT_DROP_MS;
      nextDropAt = now + dropInterval;
    }

    if (gameState == GameState::Playing &&
        static_cast<int32_t>(now - nextDropAt) >= 0) {
      if (!movePiece(0, 1))
        lockPiece();
      nextDropAt = now + dropInterval;
    }

    if (gameState == GameState::Playing &&
        static_cast<int32_t>(now - nextFrameAt) >= 0) {
      drawGame();
      nextFrameAt = now + FRAME_INTERVAL_MS;
    }
    delay(4);
  }

  frameBuffer = nullptr;
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace TetrisGame

#else

namespace TetrisGame {
void run() {}
}  // namespace TetrisGame

#endif
