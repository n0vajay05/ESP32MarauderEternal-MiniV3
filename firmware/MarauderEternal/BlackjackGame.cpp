#include "BlackjackGame.h"

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

namespace BlackjackGame {
namespace {

constexpr int16_t GAME_WIDTH = TFT_WIDTH;
constexpr int16_t HEADER_HEIGHT = 16;
constexpr int16_t CARD_WIDTH = 16;
constexpr int16_t CARD_HEIGHT = 28;
constexpr int16_t CARD_STEP = 18;
constexpr int16_t CARDS_X = 2;
constexpr int16_t DEALER_Y = 30;
constexpr int16_t PLAYER_Y = 77;
constexpr uint8_t MAX_HAND_CARDS = 7;

struct HandValue {
  uint8_t total;
  bool soft;
};

enum class RoundState : uint8_t {
  PlayerTurn,
  Result,
};

uint8_t deck[52]{};
uint8_t deckPosition = 0;
uint8_t playerCards[MAX_HAND_CARDS]{};
uint8_t dealerCards[MAX_HAND_CARDS]{};
uint8_t playerCount = 0;
uint8_t dealerCount = 0;
RoundState roundState = RoundState::PlayerTurn;
uint16_t wins = 0;
uint16_t losses = 0;
uint16_t ties = 0;
const char* resultMessage = "";
int8_t resultOutcome = 0;
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

HandValue handValue(const uint8_t cards[MAX_HAND_CARDS], uint8_t count) {
  uint8_t total = 0;
  uint8_t aces = 0;
  for (uint8_t index = 0; index < count; index++) {
    const uint8_t rank = cardRank(cards[index]);
    if (rank == 14) {
      total += 11;
      aces++;
    }
    else {
      total += rank > 10 ? 10 : rank;
    }
  }
  while (total > 21 && aces > 0) {
    total -= 10;
    aces--;
  }
  HandValue value{total, aces > 0};
  return value;
}

void drawHeader() {
  canvas().fillRect(0, 0, GAME_WIDTH, HEADER_HEIGHT, TFT_NAVY);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_YELLOW, TFT_NAVY);
  canvas().setCursor(2, 4);
  canvas().print(F("BLACKJACK"));
  canvas().setTextColor(TFT_WHITE, TFT_NAVY);
  canvas().setCursor(62, 4);
  canvas().print(F("W"));
  canvas().print(wins);
  canvas().setCursor(86, 4);
  canvas().print(F("L"));
  canvas().print(losses);
  canvas().setCursor(110, 4);
  canvas().print(F("T"));
  canvas().print(ties);
}

void drawCard(uint8_t card, int16_t x, int16_t y, bool faceDown) {
  if (faceDown) {
    canvas().fillRect(x, y, CARD_WIDTH, CARD_HEIGHT, TFT_BLUE);
    canvas().drawRect(x, y, CARD_WIDTH, CARD_HEIGHT, TFT_WHITE);
    canvas().drawLine(x + 2, y + 3, x + CARD_WIDTH - 3,
                      y + CARD_HEIGHT - 4, TFT_CYAN);
    canvas().drawLine(x + CARD_WIDTH - 3, y + 3, x + 2,
                      y + CARD_HEIGHT - 4, TFT_CYAN);
    return;
  }

  canvas().fillRect(x, y, CARD_WIDTH, CARD_HEIGHT, TFT_WHITE);
  canvas().drawRect(x, y, CARD_WIDTH, CARD_HEIGHT, TFT_LIGHTGREY);
  canvas().setTextDatum(TC_DATUM);
  canvas().setTextColor(suitColor(cardSuit(card)), TFT_WHITE);
  canvas().drawString(rankLabel(cardRank(card)), x + CARD_WIDTH / 2,
                      y + 3, 1);
  char suitText[2] = {suitLabel(cardSuit(card)), '\0'};
  canvas().drawString(suitText, x + CARD_WIDTH / 2, y + 15, 1);
  canvas().setTextDatum(TL_DATUM);
}

void drawGame() {
  canvas().fillScreen(TFT_BLACK);
  drawHeader();

  const bool hideHoleCard = roundState == RoundState::PlayerTurn;
  const HandValue dealerValue = handValue(dealerCards, dealerCount);
  const HandValue playerValue = handValue(playerCards, playerCount);
  canvas().setTextDatum(TL_DATUM);
  canvas().setTextSize(1);
  canvas().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  canvas().setCursor(2, 19);
  canvas().print(F("DEALER "));
  if (hideHoleCard)
    canvas().print(F("?"));
  else
    canvas().print(dealerValue.total);
  canvas().setCursor(2, 66);
  canvas().print(F("YOU "));
  canvas().print(playerValue.total);
  if (playerValue.soft)
    canvas().print(F(" soft"));

  for (uint8_t index = 0; index < dealerCount; index++)
    drawCard(dealerCards[index], CARDS_X + index * CARD_STEP, DEALER_Y,
             hideHoleCard && index == 1);
  for (uint8_t index = 0; index < playerCount; index++)
    drawCard(playerCards[index], CARDS_X + index * CARD_STEP, PLAYER_Y, false);

  canvas().fillRect(0, 107, GAME_WIDTH, 21, TFT_BLACK);
  canvas().setTextDatum(TC_DATUM);
  if (roundState == RoundState::PlayerTurn) {
    canvas().setTextColor(TFT_CYAN, TFT_BLACK);
    canvas().drawString("UP:HIT  DOWN:STAND", GAME_WIDTH / 2, 110, 1);
    canvas().setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    canvas().drawString("Hold center: exit", GAME_WIDTH / 2, 119, 1);
  }
  else {
    canvas().setTextColor(resultOutcome > 0 ? TFT_GREEN :
                          (resultOutcome < 0 ? TFT_RED : TFT_YELLOW),
                          TFT_BLACK);
    canvas().drawString(resultMessage, GAME_WIDTH / 2, 109, 1);
    canvas().setTextColor(TFT_WHITE, TFT_BLACK);
    canvas().drawString("Tap: deal  Hold: exit", GAME_WIDTH / 2, 118, 1);
  }
  canvas().setTextDatum(TL_DATUM);
  presentFrame();
}

void setResult(const char* message, int8_t outcome) {
  resultMessage = message;
  resultOutcome = outcome;
  if (outcome > 0)
    wins++;
  else if (outcome < 0)
    losses++;
  else
    ties++;
  roundState = RoundState::Result;
  drawGame();
}

void compareHands() {
  const HandValue player = handValue(playerCards, playerCount);
  const HandValue dealer = handValue(dealerCards, dealerCount);
  if (player.total > 21) {
    setResult("BUST - DEALER WINS", -1);
  }
  else if (dealer.total > 21) {
    setResult("WIN - DEALER BUST", 1);
  }
  else if (player.total > dealer.total) {
    setResult("WIN", 1);
  }
  else if (player.total < dealer.total) {
    setResult("LOSE", -1);
  }
  else {
    setResult("PUSH", 0);
  }
}

void dealerTurn() {
  HandValue dealer = handValue(dealerCards, dealerCount);
  while (dealer.total < 17 && dealerCount < MAX_HAND_CARDS) {
    dealerCards[dealerCount++] = dealCard();
    dealer = handValue(dealerCards, dealerCount);
  }
  compareHands();
}

void newHand() {
  shuffleDeck();
  playerCount = 0;
  dealerCount = 0;
  playerCards[playerCount++] = dealCard();
  dealerCards[dealerCount++] = dealCard();
  playerCards[playerCount++] = dealCard();
  dealerCards[dealerCount++] = dealCard();
  resultMessage = "";
  resultOutcome = 0;
  roundState = RoundState::PlayerTurn;

  const HandValue player = handValue(playerCards, playerCount);
  const HandValue dealer = handValue(dealerCards, dealerCount);
  if (player.total == 21 || dealer.total == 21) {
    if (player.total == 21 && dealer.total == 21)
      setResult("PUSH - BOTH BLACKJACK", 0);
    else if (player.total == 21)
      setResult("BLACKJACK - YOU WIN", 1);
    else
      setResult("DEALER BLACKJACK", -1);
    return;
  }
  drawGame();
}

void playerHit() {
  if (playerCount >= MAX_HAND_CARDS) {
    dealerTurn();
    return;
  }
  playerCards[playerCount++] = dealCard();
  const HandValue value = handValue(playerCards, playerCount);
  if (value.total > 21) {
    compareHands();
    return;
  }
  if (value.total == 21 || playerCount >= MAX_HAND_CARDS) {
    dealerTurn();
    return;
  }
  drawGame();
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
    if (centerEvent == GameInput::CenterEvent::Tap &&
        roundState == RoundState::Result)
      newHand();

    if (roundState == RoundState::PlayerTurn) {
      if (u_btn.justPressed() || l_btn.justPressed())
        playerHit();
      if (roundState == RoundState::PlayerTurn &&
          (d_btn.justPressed() || r_btn.justPressed())) {
        dealerTurn();
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

}  // namespace BlackjackGame

#else

namespace BlackjackGame {
void run() {}
}  // namespace BlackjackGame

#endif
