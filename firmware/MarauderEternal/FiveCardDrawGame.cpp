#include "FiveCardDrawGame.h"

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

namespace FiveCardDrawGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t CARD_WIDTH = 22;
constexpr int16_t CARD_HEIGHT = 31;
constexpr int16_t CARD_GAP = 2;
constexpr int16_t CARDS_X = 4;
constexpr int16_t DEALER_Y = 29;
constexpr int16_t PLAYER_Y = 85;
constexpr uint8_t HAND_SIZE = 5;

enum class RoundState : uint8_t {
  Choosing,
  Result,
};

uint8_t deck[52]{};
uint8_t deckPosition = 0;
uint8_t playerHand[HAND_SIZE]{};
uint8_t dealerHand[HAND_SIZE]{};
bool held[HAND_SIZE]{};
uint8_t selectedCard = 0;
RoundState roundState = RoundState::Choosing;
uint16_t wins = 0;
uint16_t losses = 0;
uint16_t ties = 0;
const char* resultMessage = "";
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

uint8_t cardRank(uint8_t card) {
  return card % 13 + 2;
}

uint8_t cardSuit(uint8_t card) {
  return card / 13;
}

const char* rankLabel(uint8_t rank) {
  switch (rank) {
    case 14: return "A";
    case 13: return "K";
    case 12: return "Q";
    case 11: return "J";
    case 10: return "10";
    case 9: return "9";
    case 8: return "8";
    case 7: return "7";
    case 6: return "6";
    case 5: return "5";
    case 4: return "4";
    case 3: return "3";
    default: return "2";
  }
}

char suitLabel(uint8_t suit) {
  const char labels[4] = {'S', 'H', 'D', 'C'};
  return labels[suit & 3];
}

uint16_t suitColor(uint8_t suit) {
  return suit == 1 || suit == 2 ? TFT_RED : TFT_BLACK;
}

void shuffleDeck() {
  for (uint8_t card = 0; card < 52; card++)
    deck[card] = card;
  for (int16_t index = 51; index > 0; index--) {
    const int16_t other = random(index + 1);
    const uint8_t temporary = deck[index];
    deck[index] = deck[other];
    deck[other] = temporary;
  }
  deckPosition = 0;
}

uint8_t dealCard() {
  return deck[deckPosition++];
}

void sortRanksDescending(uint8_t ranks[HAND_SIZE]) {
  for (uint8_t index = 1; index < HAND_SIZE; index++) {
    const uint8_t value = ranks[index];
    int8_t position = index - 1;
    while (position >= 0 && ranks[position] < value) {
      ranks[position + 1] = ranks[position];
      position--;
    }
    ranks[position + 1] = value;
  }
}

uint32_t packScore(uint8_t category, uint8_t first = 0,
                   uint8_t second = 0, uint8_t third = 0,
                   uint8_t fourth = 0, uint8_t fifth = 0) {
  return (static_cast<uint32_t>(category) << 20) |
         (static_cast<uint32_t>(first) << 16) |
         (static_cast<uint32_t>(second) << 12) |
         (static_cast<uint32_t>(third) << 8) |
         (static_cast<uint32_t>(fourth) << 4) | fifth;
}

uint32_t evaluateHand(const uint8_t hand[HAND_SIZE]) {
  uint8_t counts[15]{};
  uint8_t ranks[HAND_SIZE]{};
  uint8_t suitCounts[4]{};
  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    ranks[index] = cardRank(hand[index]);
    counts[ranks[index]]++;
    suitCounts[cardSuit(hand[index])]++;
  }
  sortRanksDescending(ranks);

  const bool flush = suitCounts[cardSuit(hand[0])] == HAND_SIZE;
  bool straight = true;
  for (uint8_t index = 1; index < HAND_SIZE; index++) {
    if (ranks[index - 1] != ranks[index] + 1) {
      straight = false;
      break;
    }
  }
  uint8_t straightHigh = ranks[0];
  if (!straight && ranks[0] == 14 && ranks[1] == 5 && ranks[2] == 4 &&
      ranks[3] == 3 && ranks[4] == 2) {
    straight = true;
    straightHigh = 5;
  }

  uint8_t four = 0;
  uint8_t three = 0;
  uint8_t highPair = 0;
  uint8_t lowPair = 0;
  for (int8_t rank = 14; rank >= 2; rank--) {
    if (counts[rank] == 4)
      four = rank;
    else if (counts[rank] == 3)
      three = rank;
    else if (counts[rank] == 2) {
      if (highPair == 0)
        highPair = rank;
      else
        lowPair = rank;
    }
  }

  if (straight && flush)
    return packScore(8, straightHigh);
  if (four != 0) {
    uint8_t kicker = 0;
    for (int8_t rank = 14; rank >= 2; rank--)
      if (counts[rank] == 1) kicker = rank;
    return packScore(7, four, kicker);
  }
  if (three != 0 && highPair != 0)
    return packScore(6, three, highPair);
  if (flush)
    return packScore(5, ranks[0], ranks[1], ranks[2], ranks[3], ranks[4]);
  if (straight)
    return packScore(4, straightHigh);
  if (three != 0) {
    uint8_t kickers[2]{};
    uint8_t count = 0;
    for (int8_t rank = 14; rank >= 2; rank--)
      if (counts[rank] == 1) kickers[count++] = rank;
    return packScore(3, three, kickers[0], kickers[1]);
  }
  if (highPair != 0 && lowPair != 0) {
    uint8_t kicker = 0;
    for (int8_t rank = 14; rank >= 2; rank--)
      if (counts[rank] == 1) kicker = rank;
    return packScore(2, highPair, lowPair, kicker);
  }
  if (highPair != 0) {
    uint8_t kickers[3]{};
    uint8_t count = 0;
    for (int8_t rank = 14; rank >= 2; rank--)
      if (counts[rank] == 1) kickers[count++] = rank;
    return packScore(1, highPair, kickers[0], kickers[1], kickers[2]);
  }
  return packScore(0, ranks[0], ranks[1], ranks[2], ranks[3], ranks[4]);
}

const char* categoryLabel(uint32_t score) {
  switch ((score >> 20) & 0x0f) {
    case 8: return "STRAIGHT FLUSH";
    case 7: return "FOUR KIND";
    case 6: return "FULL HOUSE";
    case 5: return "FLUSH";
    case 4: return "STRAIGHT";
    case 3: return "THREE KIND";
    case 2: return "TWO PAIR";
    case 1: return "PAIR";
    default: return "HIGH CARD";
  }
}

void drawHeader() {
  canvas().fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_YELLOW, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("5 DRAW"));
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(48, 4);
  canvas().print(F("W"));
  canvas().print(wins);
  canvas().setCursor(75, 4);
  canvas().print(F("L"));
  canvas().print(losses);
  canvas().setCursor(102, 4);
  canvas().print(F("T"));
  canvas().print(ties);
}

void drawCard(uint8_t card, int16_t x, int16_t y, bool faceDown,
              bool selected, bool isHeld) {
  if (faceDown) {
    canvas().fillRect(x, y, CARD_WIDTH, CARD_HEIGHT, TFT_BLUE);
    canvas().drawRect(x, y, CARD_WIDTH, CARD_HEIGHT, TFT_WHITE);
    canvas().drawLine(x + 3, y + 3, x + CARD_WIDTH - 4,
                      y + CARD_HEIGHT - 4, TFT_CYAN);
    canvas().drawLine(x + CARD_WIDTH - 4, y + 3, x + 3,
                      y + CARD_HEIGHT - 4, TFT_CYAN);
    return;
  }

  canvas().fillRect(x, y, CARD_WIDTH, CARD_HEIGHT, TFT_WHITE);
  canvas().drawRect(x, y, CARD_WIDTH, CARD_HEIGHT,
                    selected ? TFT_YELLOW : TFT_LIGHTGREY);
  const uint16_t color = suitColor(cardSuit(card));
  canvas().setTextColor(color, TFT_WHITE);
  canvas().setTextDatum(TC_DATUM);
  canvas().drawString(rankLabel(cardRank(card)), x + CARD_WIDTH / 2,
                      y + 4, 1);
  char suitText[2] = {suitLabel(cardSuit(card)), '\0'};
  canvas().drawString(suitText, x + CARD_WIDTH / 2, y + 16, 1);
  if (isHeld) {
    canvas().fillRect(x + 1, y + CARD_HEIGHT - 7,
                      CARD_WIDTH - 2, 6, TFT_GREEN);
    canvas().setTextColor(TFT_BLACK, TFT_GREEN);
    canvas().drawString("H", x + CARD_WIDTH / 2,
                        y + CARD_HEIGHT - 8, 1);
  }
  canvas().setTextDatum(TL_DATUM);
}

void drawGame() {
  canvas().fillScreen(TFT_BLACK);
  drawHeader();
  canvas().setTextSize(1);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas().setCursor(3, 19);
  canvas().print(F("DEALER"));
  canvas().setCursor(3, 75);
  canvas().print(F("YOUR HAND"));

  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    const int16_t x = CARDS_X + index * (CARD_WIDTH + CARD_GAP);
    drawCard(dealerHand[index], x, DEALER_Y,
             roundState == RoundState::Choosing, false, false);
    drawCard(playerHand[index], x, PLAYER_Y, false,
             roundState == RoundState::Choosing && index == selectedCard,
             roundState == RoundState::Choosing && held[index]);
  }

  canvas().fillRect(0, 62, GAME_WIDTH, 12, TFT_BLACK);
  canvas().setTextDatum(TC_DATUM);
  if (roundState == RoundState::Choosing) {
    canvas().setTextColor(TFT_CYAN, TFT_BLACK);
    canvas().drawString("UP:HOLD  TAP:DRAW", GAME_WIDTH / 2, 64, 1);
  }
  else {
    canvas().setTextColor(
        resultMessage[0] == 'W' ? TFT_GREEN :
        (resultMessage[0] == 'L' ? TFT_RED : TFT_YELLOW), TFT_BLACK);
    canvas().drawString(resultMessage, GAME_WIDTH / 2, 64, 1);
  }
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void newHand() {
  shuffleDeck();
  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    playerHand[index] = dealCard();
    dealerHand[index] = dealCard();
    held[index] = false;
  }
  selectedCard = 0;
  roundState = RoundState::Choosing;
  resultMessage = "";
  drawGame();
}

void chooseDealerHolds(bool dealerHeld[HAND_SIZE]) {
  uint8_t rankCounts[15]{};
  uint8_t suitCounts[4]{};
  bool hasGroup = false;
  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    rankCounts[cardRank(dealerHand[index])]++;
    suitCounts[cardSuit(dealerHand[index])]++;
    dealerHeld[index] = false;
  }
  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    if (rankCounts[cardRank(dealerHand[index])] >= 2) {
      dealerHeld[index] = true;
      hasGroup = true;
    }
  }
  if (hasGroup)
    return;

  int8_t fourFlushSuit = -1;
  for (uint8_t suit = 0; suit < 4; suit++)
    if (suitCounts[suit] == 4) fourFlushSuit = suit;
  if (fourFlushSuit >= 0) {
    for (uint8_t index = 0; index < HAND_SIZE; index++)
      dealerHeld[index] = cardSuit(dealerHand[index]) == fourFlushSuit;
    return;
  }

  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    const uint8_t rank = cardRank(dealerHand[index]);
    dealerHeld[index] = rank >= 11 || rank == 14;
  }
}

void finishDraw() {
  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    if (!held[index])
      playerHand[index] = dealCard();
  }

  bool dealerHeld[HAND_SIZE]{};
  chooseDealerHolds(dealerHeld);
  for (uint8_t index = 0; index < HAND_SIZE; index++) {
    if (!dealerHeld[index])
      dealerHand[index] = dealCard();
  }

  const uint32_t playerScore = evaluateHand(playerHand);
  const uint32_t dealerScore = evaluateHand(dealerHand);
  if (playerScore > dealerScore) {
    wins++;
    resultMessage = "WIN - TAP FOR DEAL";
  }
  else if (playerScore < dealerScore) {
    losses++;
    resultMessage = "LOSE - TAP FOR DEAL";
  }
  else {
    ties++;
    resultMessage = "TIE - TAP FOR DEAL";
  }
  roundState = RoundState::Result;
  drawGame();

  canvas().setTextDatum(TR_DATUM);
  canvas().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas().drawString(categoryLabel(dealerScore), GAME_WIDTH - 2, 19, 1);
  canvas().drawString(categoryLabel(playerScore), GAME_WIDTH - 2, 75, 1);
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

}  // namespace

void run() {
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
  wins = 0;
  losses = 0;
  ties = 0;
  GameInput::TapHoldButton centerButton(c_btn);
  centerButton.reset();
  newHand();

  while (true) {
    const GameInput::CenterEvent centerEvent = centerButton.poll(millis());
    if (centerEvent == GameInput::CenterEvent::Hold)
      break;
    if (centerEvent == GameInput::CenterEvent::Tap) {
      if (roundState == RoundState::Choosing)
        finishDraw();
      else
        newHand();
    }

    if (roundState == RoundState::Choosing) {
      if (l_btn.justPressed()) {
        selectedCard = selectedCard == 0 ? HAND_SIZE - 1 : selectedCard - 1;
        drawGame();
      }
      if (r_btn.justPressed()) {
        selectedCard = (selectedCard + 1) % HAND_SIZE;
        drawGame();
      }
      if (u_btn.justPressed() || d_btn.justPressed()) {
        held[selectedCard] = !held[selectedCard];
        drawGame();
      }
    }
    delay(4);
  }

  frameBuffer = nullptr;
  releaseControls();
  display_obj.tft.setTextDatum(TL_DATUM);
  display_obj.tft.setTextWrap(false);
  display_obj.tft.setTextSize(1);
}

}  // namespace FiveCardDrawGame

#else

namespace FiveCardDrawGame {
void run() {}
}  // namespace FiveCardDrawGame

#endif
