#include <Arduino.h>
#include <Wire.h>
#include <avr/pgmspace.h>

#define LED1_PIN PIN_PA6
#define LED2_PIN PIN_PA3
#define BUTTON_PIN PIN_PA7

#define I2C_ADDRESS 0x42

#define CHALLENGE1_START 0x00
#define CHALLENGE2_TRIGGER_REGISTER 0x2B
#define CHALLENGE2_START 0x2C
#define CHALLENGE2_TRIGGER_VALUE 0xA5

#define I2C_RESPONSE_SIZE 32

#define BLINK_INTERVAL_MS 500
#define FADE_STEP_MS 4
#define DEBOUNCE_TIME_MS 40
#define CHALLENGE2_VISIBLE_MS 1000

const char challenge1Flag[] PROGMEM =
    "FLAG{I2C1_4b881479437795f9d3ee4982afce64be}";

const char challenge2Flag[] PROGMEM =
    "FLAG{I2C2_52c263a749aaa7b76c1a2eb11bb8d45a}";

constexpr uint8_t CHALLENGE1_LENGTH =
    sizeof(challenge1Flag) - 1;

constexpr uint8_t CHALLENGE2_LENGTH =
    sizeof(challenge2Flag) - 1;

enum LedMode
{
    LED_MODE_BLINK,
    LED_MODE_ALTERNATE_BLINK,
    LED_MODE_SMOOTH_FADE,
    LED_MODE_SOLID,
    LED_MODE_OFF,
    LED_MODE_COUNT
};

LedMode ledMode = LED_MODE_BLINK;

uint8_t currentRegister = 0;

volatile bool challenge2TriggerPending = false;
volatile bool challenge2Active = false;

uint32_t challenge2ExpireTime = 0;
uint32_t lastBlinkTime = 0;
uint32_t lastFadeTime = 0;
uint32_t lastDebounceTime = 0;

bool blinkState = true;
uint8_t fadeLevel = 0;
uint8_t fadeDuty = 0;
int8_t fadeDirection = 1;
bool lastButtonReading = HIGH;
bool stableButtonState = HIGH;

uint8_t readRegister(uint8_t address)
{
    if (address >= CHALLENGE1_START &&
        address < CHALLENGE1_START + CHALLENGE1_LENGTH)
    {
        uint8_t index = address - CHALLENGE1_START;
        return pgm_read_byte(&challenge1Flag[index]);
    }

    if (address == CHALLENGE2_TRIGGER_REGISTER)
    {
        return 0xFF;
    }

    if (challenge2Active &&
        address >= CHALLENGE2_START &&
        address < CHALLENGE2_START + CHALLENGE2_LENGTH)
    {
        uint8_t index = address - CHALLENGE2_START;
        return pgm_read_byte(&challenge2Flag[index]);
    }

    return 0xFF;
}

void receiveEvent(int byteCount)
{
    Wire.getBytesRead();

    if (byteCount <= 0)
    {
        return;
    }

    currentRegister = Wire.read();
    byteCount--;

    while (byteCount > 0 && Wire.available())
    {
        uint8_t value = Wire.read();

        if (currentRegister == CHALLENGE2_TRIGGER_REGISTER &&
            value == CHALLENGE2_TRIGGER_VALUE)
        {
            challenge2TriggerPending = true;
        }

        currentRegister++;
        byteCount--;
    }
}

void requestEvent()
{
    currentRegister += Wire.getBytesRead();

    for (uint8_t i = 0; i < I2C_RESPONSE_SIZE; i++)
    {
        Wire.write(readRegister(currentRegister + i));
    }
}

void setLedPower(bool enabled)
{
    digitalWrite(LED1_PIN, enabled ? HIGH : LOW);
    digitalWrite(LED2_PIN, enabled ? HIGH : LOW);
}

void applyLedMode()
{
    if (ledMode == LED_MODE_BLINK ||
        ledMode == LED_MODE_ALTERNATE_BLINK)
    {
        blinkState = true;
        digitalWrite(LED1_PIN, HIGH);
        digitalWrite(LED2_PIN,
                     ledMode == LED_MODE_BLINK ? HIGH : LOW);
        lastBlinkTime = millis();
    }
    else if (ledMode == LED_MODE_SMOOTH_FADE)
    {
        fadeLevel = 0;
        fadeDuty = 0;
        fadeDirection = 1;
        digitalWrite(LED1_PIN, LOW);
        digitalWrite(LED2_PIN, HIGH);
        lastFadeTime = millis();
    }
    else if (ledMode == LED_MODE_SOLID)
    {
        setLedPower(true);
    }
    else
    {
        setLedPower(false);
    }
}

void advanceLedMode()
{
    uint8_t nextMode = static_cast<uint8_t>(ledMode) + 1;

    if (nextMode >= LED_MODE_COUNT)
    {
        nextMode = 0;
    }

    ledMode = static_cast<LedMode>(nextMode);
    applyLedMode();
}

void updateButton()
{
    bool reading = digitalRead(BUTTON_PIN);
    uint32_t now = millis();

    if (reading != lastButtonReading)
    {
        lastDebounceTime = now;
        lastButtonReading = reading;
    }

    if ((uint32_t)(now - lastDebounceTime) >= DEBOUNCE_TIME_MS)
    {
        if (reading != stableButtonState)
        {
            stableButtonState = reading;

            if (stableButtonState == LOW)
            {
                advanceLedMode();
            }
        }
    }
}

void updateLeds()
{
    uint32_t now = millis();

    if (ledMode == LED_MODE_SMOOTH_FADE)
    {
        if ((uint32_t)(now - lastFadeTime) >= FADE_STEP_MS)
        {
            lastFadeTime = now;

            if ((fadeLevel == 255 && fadeDirection > 0) ||
                (fadeLevel == 0 && fadeDirection < 0))
            {
                fadeDirection = -fadeDirection;
            }

            fadeLevel += fadeDirection;

            uint32_t level = fadeLevel;
            fadeDuty =
                level * level * (765 - 2 * level) / 65025;
        }

        uint8_t pwmPhase = micros() >> 3;
        digitalWrite(LED1_PIN, pwmPhase < fadeDuty ? HIGH : LOW);
        digitalWrite(LED2_PIN, pwmPhase >= fadeDuty ? HIGH : LOW);
        return;
    }

    if (ledMode != LED_MODE_BLINK &&
        ledMode != LED_MODE_ALTERNATE_BLINK)
    {
        return;
    }

    if ((uint32_t)(now - lastBlinkTime) >= BLINK_INTERVAL_MS)
    {
        lastBlinkTime = now;
        blinkState = !blinkState;

        if (ledMode == LED_MODE_ALTERNATE_BLINK)
        {
            digitalWrite(LED1_PIN, blinkState ? HIGH : LOW);
            digitalWrite(LED2_PIN, blinkState ? LOW : HIGH);
        }
        else
        {
            setLedPower(blinkState);
        }
    }
}

void updateChallenge2()
{
    uint32_t now = millis();

    if (challenge2TriggerPending)
    {
        challenge2TriggerPending = false;
        challenge2Active = true;
        challenge2ExpireTime = now + CHALLENGE2_VISIBLE_MS;
    }

    if (challenge2Active &&
        (int32_t)(now - challenge2ExpireTime) >= 0)
    {
        challenge2Active = false;
    }
}

void setup()
{
    pinMode(LED1_PIN, OUTPUT);
    pinMode(LED2_PIN, OUTPUT);
    pinMode(BUTTON_PIN, INPUT_PULLUP);

    applyLedMode();

    Wire.begin(I2C_ADDRESS);
    Wire.onReceive(receiveEvent);
    Wire.onRequest(requestEvent);
}

void loop()
{
    updateButton();
    updateLeds();
    updateChallenge2();
}
