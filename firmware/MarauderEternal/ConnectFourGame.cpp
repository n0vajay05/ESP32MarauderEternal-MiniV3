#include "ConnectFourGame.h"

#include "configs.h"

#if defined(HAS_SCREEN) && defined(HAS_BUTTONS) && \
    (D_BTN >= 0) && (L_BTN >= 0) && (R_BTN >= 0) && (C_BTN >= 0)

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

namespace ConnectFourGame {
namespace {

constexpr uint8_t ROWS = 6;
constexpr uint8_t COLUMNS = 7;
constexpr uint8_t EMPTY = 0;
constexpr uint8_t HUMAN = 1;
constexpr uint8_t COMPUTER = 2;
constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t SELECTOR_TOP = 16;
constexpr int16_t SELECTOR_HEIGHT = 17;
constexpr int16_t BOARD_X = 8;
constexpr int16_t BOARD_Y = 34;
constexpr int16_t CELL_WIDTH = 16;
constexpr int16_t CELL_HEIGHT = 15;
constexpr uint8_t SEARCH_DEPTH = 5;
constexpr int32_t WIN_SCORE = 30000;
constexpr int32_t INFINITY_SCORE = 32000;

const uint8_t COLUMN_ORDER[COLUMNS] = {3, 2, 4, 1, 5, 0, 6};

uint8_t board[ROWS][COLUMNS]{};
uint8_t selectedColumn = 3;
uint32_t searchNodes = 0;
bool gameFinished = false;
bool humanWon = false;
bool drawGame = false;
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

void drawHeader() {
  canvas().fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_YELLOW, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("4 IN ROW"));
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(84, 4);
  canvas().print(F("D:exit"));
}

uint16_t pieceColor(uint8_t piece) {
  if (piece == HUMAN) return TFT_YELLOW;
  if (piece == COMPUTER) return TFT_RED;
  return TFT_BLACK;
}

void drawBoard(bool present = true) {
  canvas().fillRect(BOARD_X, BOARD_Y,
                    COLUMNS * CELL_WIDTH, ROWS * CELL_HEIGHT, TFT_BLUE);
  for (uint8_t row = 0; row < ROWS; row++) {
    for (uint8_t column = 0; column < COLUMNS; column++) {
      const int16_t centerX = BOARD_X + column * CELL_WIDTH + CELL_WIDTH / 2;
      const int16_t centerY = BOARD_Y + row * CELL_HEIGHT + CELL_HEIGHT / 2;
      canvas().fillCircle(centerX, centerY, 5,
                          pieceColor(board[row][column]));
    }
  }
  canvas().drawRect(BOARD_X, BOARD_Y,
                    COLUMNS * CELL_WIDTH, ROWS * CELL_HEIGHT, TFT_CYAN);
  if (present)
    presentFrame();
}

void drawSelector(const char* message = nullptr, bool present = true) {
  canvas().fillRect(0, SELECTOR_TOP, GAME_WIDTH,
                    SELECTOR_HEIGHT, TFT_BLACK);
  if (message != nullptr) {
    canvas().setTextDatum(TC_DATUM);
    canvas().setTextColor(TFT_WHITE, TFT_BLACK);
    canvas().drawString(message, GAME_WIDTH / 2, SELECTOR_TOP + 4, 1);
    canvas().setTextDatum(TL_DATUM);
    if (present)
      presentFrame();
    return;
  }

  const int16_t centerX = BOARD_X + selectedColumn * CELL_WIDTH + CELL_WIDTH / 2;
  canvas().fillTriangle(centerX, SELECTOR_TOP + 3,
                        centerX - 5, SELECTOR_TOP + 13,
                        centerX + 5, SELECTOR_TOP + 13, TFT_YELLOW);
  if (present)
    presentFrame();
}

bool columnAvailable(uint8_t column) {
  return column < COLUMNS && board[0][column] == EMPTY;
}

int8_t dropPiece(uint8_t column, uint8_t piece) {
  for (int8_t row = ROWS - 1; row >= 0; row--) {
    if (board[row][column] == EMPTY) {
      board[row][column] = piece;
      return row;
    }
  }
  return -1;
}

uint8_t countDirection(int8_t row, int8_t column,
                       int8_t rowStep, int8_t columnStep,
                       uint8_t piece) {
  uint8_t count = 0;
  row += rowStep;
  column += columnStep;
  while (row >= 0 && row < ROWS && column >= 0 && column < COLUMNS &&
         board[row][column] == piece) {
    count++;
    row += rowStep;
    column += columnStep;
  }
  return count;
}

bool winningMove(uint8_t row, uint8_t column, uint8_t piece) {
  const int8_t directions[4][2] = {{0, 1}, {1, 0}, {1, 1}, {1, -1}};
  for (uint8_t index = 0; index < 4; index++) {
    const int8_t rowStep = directions[index][0];
    const int8_t columnStep = directions[index][1];
    const uint8_t count = 1 +
        countDirection(row, column, rowStep, columnStep, piece) +
        countDirection(row, column, -rowStep, -columnStep, piece);
    if (count >= 4)
      return true;
  }
  return false;
}

bool boardFull() {
  for (uint8_t column = 0; column < COLUMNS; column++) {
    if (columnAvailable(column))
      return false;
  }
  return true;
}

int16_t scoreWindow(uint8_t startRow, uint8_t startColumn,
                    int8_t rowStep, int8_t columnStep) {
  uint8_t computerCount = 0;
  uint8_t humanCount = 0;
  uint8_t emptyCount = 0;
  for (uint8_t index = 0; index < 4; index++) {
    const uint8_t piece = board[startRow + index * rowStep]
                               [startColumn + index * columnStep];
    if (piece == COMPUTER) computerCount++;
    else if (piece == HUMAN) humanCount++;
    else emptyCount++;
  }

  if (computerCount > 0 && humanCount > 0)
    return 0;
  if (computerCount == 4) return 10000;
  if (humanCount == 4) return -10000;
  if (computerCount == 3 && emptyCount == 1) return 120;
  if (humanCount == 3 && emptyCount == 1) return -150;
  if (computerCount == 2 && emptyCount == 2) return 14;
  if (humanCount == 2 && emptyCount == 2) return -18;
  return 0;
}

int32_t evaluateBoard() {
  int32_t score = 0;
  for (uint8_t row = 0; row < ROWS; row++) {
    if (board[row][3] == COMPUTER) score += 4;
    else if (board[row][3] == HUMAN) score -= 4;
  }

  for (uint8_t row = 0; row < ROWS; row++)
    for (uint8_t column = 0; column <= COLUMNS - 4; column++)
      score += scoreWindow(row, column, 0, 1);
  for (uint8_t column = 0; column < COLUMNS; column++)
    for (uint8_t row = 0; row <= ROWS - 4; row++)
      score += scoreWindow(row, column, 1, 0);
  for (uint8_t row = 0; row <= ROWS - 4; row++)
    for (uint8_t column = 0; column <= COLUMNS - 4; column++)
      score += scoreWindow(row, column, 1, 1);
  for (uint8_t row = 0; row <= ROWS - 4; row++)
    for (uint8_t column = 3; column < COLUMNS; column++)
      score += scoreWindow(row, column, 1, -1);
  return score;
}

int32_t minimax(uint8_t depth, int32_t alpha, int32_t beta,
                bool computerTurn) {
  searchNodes++;
  if ((searchNodes & 0x1ff) == 0)
    yield();
  if (depth == 0 || boardFull())
    return evaluateBoard();

  if (computerTurn) {
    int32_t best = -INFINITY_SCORE;
    for (uint8_t index = 0; index < COLUMNS; index++) {
      const uint8_t column = COLUMN_ORDER[index];
      if (!columnAvailable(column)) continue;
      const int8_t row = dropPiece(column, COMPUTER);
      const int32_t value = winningMove(row, column, COMPUTER)
          ? WIN_SCORE + depth
          : minimax(depth - 1, alpha, beta, false);
      board[row][column] = EMPTY;
      if (value > best) best = value;
      if (best > alpha) alpha = best;
      if (beta <= alpha) break;
    }
    return best;
  }

  int32_t best = INFINITY_SCORE;
  for (uint8_t index = 0; index < COLUMNS; index++) {
    const uint8_t column = COLUMN_ORDER[index];
    if (!columnAvailable(column)) continue;
    const int8_t row = dropPiece(column, HUMAN);
    const int32_t value = winningMove(row, column, HUMAN)
        ? -WIN_SCORE - depth
        : minimax(depth - 1, alpha, beta, true);
    board[row][column] = EMPTY;
    if (value < best) best = value;
    if (best < beta) beta = best;
    if (beta <= alpha) break;
  }
  return best;
}

uint8_t chooseComputerColumn() {
  searchNodes = 0;
  int32_t bestScore = -INFINITY_SCORE;
  uint8_t bestColumns[COLUMNS]{};
  uint8_t bestCount = 0;

  for (uint8_t index = 0; index < COLUMNS; index++) {
    const uint8_t column = COLUMN_ORDER[index];
    if (!columnAvailable(column)) continue;
    const int8_t row = dropPiece(column, COMPUTER);
    const int32_t score = winningMove(row, column, COMPUTER)
        ? WIN_SCORE + SEARCH_DEPTH
        : minimax(SEARCH_DEPTH - 1, -INFINITY_SCORE,
                  INFINITY_SCORE, false);
    board[row][column] = EMPTY;

    if (score > bestScore) {
      bestScore = score;
      bestColumns[0] = column;
      bestCount = 1;
    }
    else if (score == bestScore) {
      bestColumns[bestCount++] = column;
    }
  }
  return bestCount == 0 ? 0 : bestColumns[random(bestCount)];
}

void showEndScreen() {
  const uint16_t color = drawGame ? TFT_WHITE : (humanWon ? TFT_GREEN : TFT_RED);
  const char* title = drawGame ? "DRAW" : (humanWon ? "YOU WIN" : "AI WINS");
  canvas().fillRect(13, 52, GAME_WIDTH - 26, 47, TFT_BLACK);
  canvas().drawRect(13, 52, GAME_WIDTH - 26, 47, color);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(color, TFT_BLACK);
  canvas().drawString(title, GAME_WIDTH / 2, 57, 2);
  canvas().setTextColor(TFT_WHITE, TFT_BLACK);
  canvas().drawString("Center: again", GAME_WIDTH / 2, 78, 1);
  canvas().drawString("Down: exit", GAME_WIDTH / 2, 89, 1);
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void finishGame(bool won, bool tied) {
  gameFinished = true;
  humanWon = won;
  drawGame = tied;
  showEndScreen();
}

void resetGame() {
  for (uint8_t row = 0; row < ROWS; row++)
    for (uint8_t column = 0; column < COLUMNS; column++)
      board[row][column] = EMPTY;
  selectedColumn = 3;
  gameFinished = false;
  humanWon = false;
  drawGame = false;
  canvas().fillScreen(TFT_BLACK);
  drawHeader();
  drawSelector(nullptr, false);
  drawBoard();
}

void moveSelection(int8_t direction) {
  int8_t column = selectedColumn;
  for (uint8_t attempt = 0; attempt < COLUMNS; attempt++) {
    column += direction;
    if (column < 0) column = COLUMNS - 1;
    if (column >= COLUMNS) column = 0;
    if (columnAvailable(column)) {
      selectedColumn = column;
      drawSelector();
      return;
    }
  }
}

void playHumanTurn() {
  if (!columnAvailable(selectedColumn)) {
    moveSelection(1);
    return;
  }

  const int8_t humanRow = dropPiece(selectedColumn, HUMAN);
  drawBoard();
  if (winningMove(humanRow, selectedColumn, HUMAN)) {
    finishGame(true, false);
    return;
  }
  if (boardFull()) {
    finishGame(false, true);
    return;
  }

  drawSelector("AI thinking...");
  delay(20);
  const uint8_t computerColumn = chooseComputerColumn();
  const int8_t computerRow = dropPiece(computerColumn, COMPUTER);
  drawBoard();
  if (winningMove(computerRow, computerColumn, COMPUTER)) {
    finishGame(false, false);
    return;
  }
  if (boardFull()) {
    finishGame(false, true);
    return;
  }

  if (!columnAvailable(selectedColumn))
    moveSelection(1);
  else
    drawSelector();
}

}  // namespace

void run() {
  // The Mini V3 remains in its configured fixed orientation.
  releaseControls();
  randomSeed(micros());
  GameFrameBuffer bufferedFrame(display_obj.tft);
  if (!bufferedFrame.begin(GAME_WIDTH, TFT_HEIGHT)) {
    display_obj.tft.fillScreen(TFT_BLACK);
    display_obj.tft.setTextColor(TFT_RED, TFT_BLACK);
    display_obj.tft.drawString("Frame buffer error", 8, 56, 1);
    releaseControls();
    return;
  }
  frameBuffer = &bufferedFrame;
  resetGame();

  while (true) {
    if (d_btn.justPressed())
      break;

    if (gameFinished) {
      if (c_btn.justPressed())
        resetGame();
      delay(5);
      continue;
    }

    if (l_btn.justPressed())
      moveSelection(-1);
    if (r_btn.justPressed())
      moveSelection(1);
    if (c_btn.justPressed())
      playHumanTurn();
    delay(5);
  }

  frameBuffer = nullptr;
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace ConnectFourGame

#else

namespace ConnectFourGame {
void run() {}
}

#endif
