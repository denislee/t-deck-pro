#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_TCA8418.h>

// Pin definitions from test_EPD.ino
#define BOARD_SPI_CS    34
#define BOARD_SPI_DC    35
#define BOARD_SPI_RST  -1
#define BOARD_SPI_BUSY 37
#define BOARD_SPI_SCK  36
#define BOARD_SPI_MOSI 33

#define BOARD_LORA_CS   3
#define BOARD_LORA_RST  4
#define BOARD_SD_CS   48
#define BOARD_EPD_CS   34

// Keypad pins
#define KEYPAD_SDA          13
#define KEYPAD_SCL          14
#define KEYPAD_IRQ          15
#define BOARD_I2C_ADDR_KEYBOARD   0x34

// Key Mappings (Assumed)
#define KEY_UP_ROW 0
#define KEY_UP_COL 8
#define KEY_DOWN_ROW 2
#define KEY_DOWN_COL 8
#define KEY_SELECT_ROW 3
#define KEY_SELECT_COL 8
#define KEY_BACK_ROW 3
#define KEY_BACK_COL 0


// App States
enum AppState {
    SELECTING_FILE,
    READING_BOOK
};
AppState currentState = SELECTING_FILE;

// E-Paper Display object
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(GxEPD2_310_GDEQ031T10(BOARD_SPI_CS, BOARD_SPI_DC, BOARD_SPI_RST, BOARD_SPI_BUSY));

// Keypad object
Adafruit_TCA8418 keypad;

// File management
File currentBook;
String fileList[20];
int fileCount = 0;
int selectedFileIndex = 0;
const int MAX_FILES = 20;

// Pagination
unsigned long pageStartPositions[500]; // Store the start position of each page
int currentPage = 0;
int maxPages = 500;
bool bookFullyIndexed = false;
const int lineHeight = 12; // Approximate height for FreeMonoBold9pt7b
const int margin = 10;
int linesPerPage;

void setup() {
    Serial.begin(115200);
    Serial.println("E-book Reader starting up...");

    // --- Initialize SPI for all devices ---
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    pinMode(BOARD_LORA_RST, OUTPUT);
    digitalWrite(BOARD_LORA_RST, HIGH);
    pinMode(BOARD_SD_CS, OUTPUT);
    digitalWrite(BOARD_SD_CS, HIGH);
    pinMode(BOARD_EPD_CS, OUTPUT);
    digitalWrite(BOARD_EPD_CS, HIGH);
    SPI.begin(BOARD_SPI_SCK, -1, BOARD_SPI_MOSI, BOARD_SPI_CS);

    // --- Initialize Display ---
    display.init(115200, true, 2, false);
    display.setRotation(1); // Landscape mode
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    linesPerPage = (display.height() - 2 * margin) / lineHeight;
    
    // --- Initialize I2C and Keypad ---
    Wire.begin(KEYPAD_SDA, KEYPAD_SCL);
    if (!keypad.begin(BOARD_I2C_ADDR_KEYBOARD, &Wire)) {
        Serial.println("Keypad not found!");
        displayMessage("Error: Keypad not found!");
        while (1);
    }
    keypad.matrix(4, 10);
    keypad.flush();

    // --- Initialize SD Card ---
    if (!SD.begin(BOARD_SD_CS)) {
        Serial.println("SD Card mount failed!");
        displayMessage("Error: SD Card not found!");
        while (1);
    }

    // --- Initial State Setup ---
    scanFiles();
    displayFileMenu();
}

void loop() {
    if (keypad.available() > 0) {
        int k = keypad.getEvent();
        bool pressed = k & 0x80;
        
        if (pressed) {
            k &= 0x7F;
            k--;
            int row = k / 10;
            int col = k % 10;
            Serial.printf("Key Press - Row: %d, Col: %d\n", row, col);

            switch (currentState) {
                case SELECTING_FILE:
                    handleMenuInput(row, col);
                    break;
                case READING_BOOK:
                    handleBookInput(row, col);
                    break;
            }
        }
    }
}

void handleMenuInput(int row, int col) {
    if (row == KEY_UP_ROW && col == KEY_UP_COL) {
        if (selectedFileIndex > 0) {
            selectedFileIndex--;
            displayFileMenu();
        }
    } else if (row == KEY_DOWN_ROW && col == KEY_DOWN_COL) {
        if (selectedFileIndex < fileCount - 1) {
            selectedFileIndex++;
            displayFileMenu();
        }
    } else if (row == KEY_SELECT_ROW && col == KEY_SELECT_COL) {
        if (fileCount > 0) {
            String path = "/" + fileList[selectedFileIndex];
            currentBook = SD.open(path.c_str());
            if (currentBook) {
                // Reset pagination for new book
                for (int i = 0; i < maxPages; i++) pageStartPositions[i] = 0;
                currentPage = 0;
                bookFullyIndexed = false;
                
                currentState = READING_BOOK;
                renderPage();
            } else {
                displayMessage("Error: Failed to open file.");
                delay(2000);
                displayFileMenu();
            }
        }
    }
}

void handleBookInput(int row, int col) {
    if (row == KEY_DOWN_ROW && col == KEY_DOWN_COL) { // Next Page
        if (!bookFullyIndexed || (currentPage + 1 < maxPages && pageStartPositions[currentPage + 1] != 0)) {
            currentPage++;
            renderPage();
        } else {
             displayMessage("End of book.");
             delay(1000);
             renderPage(); // Go back to the last page of content
        }
    } else if (row == KEY_UP_ROW && col == KEY_UP_COL) { // Previous Page
        if (currentPage > 0) {
            currentPage--;
            renderPage();
        }
    } else if (row == KEY_BACK_ROW && col == KEY_BACK_COL) { // Back to Menu
        currentBook.close();
        currentState = SELECTING_FILE;
        displayFileMenu();
    }
}

void scanFiles() {
    fileCount = 0;
    File root = SD.open("/");
    if (!root) {
        displayMessage("Error: Cannot open root dir");
        return;
    }
    while (fileCount < MAX_FILES) {
        File entry = root.openNextFile();
        if (!entry) {
            break; // No more files
        }
        if (!entry.isDirectory() && String(entry.name()).endsWith(".txt")) {
            fileList[fileCount++] = String(entry.name());
        }
        entry.close();
    }
    root.close();
}

void displayFileMenu() {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(margin, margin + lineHeight);
        display.println("Please select a book:");
        display.println("");

        for (int i = 0; i < fileCount; i++) {
            if (i == selectedFileIndex) {
                display.print("> ");
            } else {
                display.print("  ");
            }
            display.println(fileList[i]);
        }
    } while (display.nextPage());
}

void displayMessage(const char* message) {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        int16_t tbx, tby; uint16_t tbw, tbh;
        display.getTextBounds(message, 0, 0, &tbx, &tby, &tbw, &tbh);
        uint16_t x = ((display.width() - tbw) / 2) - tbx;
        uint16_t y = ((display.height() - tbh) / 2) - tby;
        display.setCursor(x, y);
        display.print(message);
    } while (display.nextPage());
}

void renderPage() {
    if (!currentBook) return;

    unsigned long position = pageStartPositions[currentPage];
    currentBook.seek(position);
    
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(margin, margin + lineHeight);
        
        for (int i = 0; i < linesPerPage; i++) {
            if (currentBook.available()) {
                String line = currentBook.readStringUntil('\n');
                display.println(line);
            } else {
                bookFullyIndexed = true;
                break;
            }
        }
    } while (display.nextPage());

    // If we haven't indexed the next page yet, record its starting position
    if (!bookFullyIndexed && (currentPage + 1 < maxPages)) {
        if(currentBook.available()){
            pageStartPositions[currentPage + 1] = currentBook.position();
        } else {
            bookFullyIndexed = true;
        }
    }
    Serial.printf("Page %d rendered. Start position: %lu\n", currentPage, position);
}
