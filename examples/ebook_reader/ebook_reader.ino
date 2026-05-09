#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_TCA8418.h>

// Pin definitions from utilities.h
#define BOARD_SPI_SCK  36
#define BOARD_SPI_MOSI 33
#define BOARD_SPI_MISO 47

#define BOARD_LORA_CS   3
#define BOARD_LORA_RST  4
#define BOARD_SD_CS   48
#define BOARD_EPD_CS   34
#define BOARD_EPD_DC   35
#define BOARD_EPD_BUSY 37
#define BOARD_EPD_RST  -1

// Keypad pins
#define KEYPAD_SDA          13
#define KEYPAD_SCL          14
#define KEYPAD_IRQ          15
#define BOARD_I2C_ADDR_KEYBOARD   0x34

// Key Mappings (Vim-style j/k for navigation, Enter to select)
#define KEY_UP_ROW 1
#define KEY_UP_COL 7 // 'k'
#define KEY_DOWN_ROW 1
#define KEY_DOWN_COL 6 // 'j'
#define KEY_SELECT_ROW 2
#define KEY_SELECT_COL 9 // 'E' (Enter)
#define KEY_BACK_ROW 3
#define KEY_BACK_COL 7 // ESC
#define KEY_ROT_ROW 0
#define KEY_ROT_COL 3 // 'r'


// App States
enum AppState {
    SELECTING_FILE,
    READING_BOOK
};
AppState currentState = SELECTING_FILE;
int currentRotation = 1; // 1 = Landscape, 0 = Portrait

// E-Paper Display object
GxEPD2_BW<GxEPD2_310_GDEQ031T10, GxEPD2_310_GDEQ031T10::HEIGHT> display(GxEPD2_310_GDEQ031T10(BOARD_EPD_CS, BOARD_EPD_DC, BOARD_EPD_RST, BOARD_EPD_BUSY));

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
const int lineHeight = 20; // Increased to fit FreeMonoBold9pt7b
const int margin = 10;
int linesPerPage;

void setup() {
    Serial.begin(115200);
    delay(1000);
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
    
    // Explicitly initialize SPI with correct pins
    SPI.begin(BOARD_SPI_SCK, BOARD_SPI_MISO, BOARD_SPI_MOSI);

    // --- Initialize Display ---
    display.init(115200, true, 2, false); // 2ms pulse is standard for this board
    display.setRotation(currentRotation);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    linesPerPage = (display.height() - 2 * margin) / 20; // Re-sync with lineHeight
    
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
    // Use multi-argument begin to ensure correct SPI bus usage
    if (!SD.begin(BOARD_SD_CS, SPI)) {
        Serial.println("SD Card mount failed!");
        displayMessage("Error: SD Card not found!");
        while (1);
    }
    Serial.println("SD Card mounted.");

    // --- Initial State Setup ---
    scanFiles();
    if (fileCount > 0) {
        selectedFileIndex = 0; // Ensure first file is selected
    }
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
            // The TCA8418 reports columns mirrored relative to the silkscreen
            // labels, so reverse to match keymap indexing (see factory's
            // peri_keypad.cpp).
            int col = 9 - (k % 10);

            Serial.printf("KEY EVENT: Row=%d, Col=%d\n", row, col);

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
    bool isUp = (row == KEY_UP_ROW) && (col == KEY_UP_COL);
    bool isDown = (row == KEY_DOWN_ROW) && (col == KEY_DOWN_COL);
    bool isSelect = (row == KEY_SELECT_ROW) && (col == KEY_SELECT_COL);
    bool isRot  = (row == KEY_ROT_ROW) && (col == KEY_ROT_COL);

    if (isUp) {
        if (selectedFileIndex > 0) {
            selectedFileIndex--;
            Serial.printf("NAV: UP (Index: %d)\n", selectedFileIndex);
            displayFileMenu();
        }
    } else if (isDown) {
        if (selectedFileIndex < fileCount - 1) {
            selectedFileIndex++;
            Serial.printf("NAV: DOWN (Index: %d)\n", selectedFileIndex);
            displayFileMenu();
        }
    } else if (isSelect) {
        Serial.printf("NAV: SELECT (%d)\n", selectedFileIndex);
        if (fileCount > 0) {
            String path = fileList[selectedFileIndex];
            if (!path.startsWith("/")) path = "/" + path;
            Serial.printf("Opening: '%s' exists=%d\n", path.c_str(), SD.exists(path.c_str()));
            currentBook = SD.open(path.c_str(), FILE_READ);
            if (currentBook && !currentBook.isDirectory()) {
                Serial.printf("Opened OK: size=%u\n", (unsigned)currentBook.size());
                for (int i = 0; i < maxPages; i++) pageStartPositions[i] = 0;
                currentPage = 0;
                bookFullyIndexed = false;

                currentState = READING_BOOK;
                renderPage();
            } else {
                Serial.printf("Failed to open: %s (currentBook=%d)\n", path.c_str(), (bool)currentBook);
                if (currentBook) currentBook.close();
                String msg = "Failed: " + path;
                displayMessage(msg.c_str());
                delay(3000);
                displayFileMenu();
            }
        }
    } else if (isRot) {
        currentRotation = (currentRotation == 1) ? 0 : 1;
        applyRotation();
        Serial.printf("Menu rotation toggled to %d\n", currentRotation);
        displayFileMenu();
    } else {
        Serial.println("NAV: Unknown Key");
    }
}

void handleBookInput(int row, int col) {
    bool isNext = (row == KEY_DOWN_ROW) && (col == KEY_DOWN_COL);
    bool isPrev = (row == KEY_UP_ROW) && (col == KEY_UP_COL);
    bool isBack = (row == KEY_BACK_ROW) && (col == KEY_BACK_COL);
    bool isRot  = (row == KEY_ROT_ROW) && (col == KEY_ROT_COL);

    if (isNext) { // Next Page
        if (!bookFullyIndexed || (currentPage + 1 < maxPages && pageStartPositions[currentPage + 1] != 0)) {
            currentPage++;
            renderPage();
        } else {
             displayMessage("End of book.");
             delay(1000);
             renderPage(); // Go back to the last page of content
        }
    } else if (isPrev) { // Previous Page
        if (currentPage > 0) {
            currentPage--;
            renderPage();
        }
    } else if (isBack) { // Back to Menu
        currentBook.close();
        currentState = SELECTING_FILE;
        displayFileMenu();
    } else if (isRot) { // Toggle Rotation
        currentRotation = (currentRotation == 1) ? 0 : 1;
        applyRotation();

        // Invalidate future page boundaries as they depend on linesPerPage
        for (int i = currentPage + 1; i < maxPages; i++) {
            pageStartPositions[i] = 0;
        }
        bookFullyIndexed = false;

        Serial.printf("Rotation toggled to %d, linesPerPage is now %d\n", currentRotation, linesPerPage);
        renderPage();
    }
}

// Toggling rotation mid-session on the UC8253 controller can leave its
// "previous" framebuffer (used by the fast-partial-update path) holding the
// pre-rotation pixels, so a subsequent refresh either stalls or visually
// does nothing. clearScreen() forces a clean full white refresh, which both
// resets the controller's refresh state and writes both internal buffers
// (0x10 previous, 0x13 current) to white before renderPage redraws.
void applyRotation() {
    display.setRotation(currentRotation);
    display.setFont(&FreeMonoBold9pt7b);
    display.setTextColor(GxEPD_BLACK);
    linesPerPage = (display.height() - 2 * margin) / 20;
    Serial.printf("applyRotation: rot=%d, w=%d h=%d, linesPerPage=%d\n",
                  currentRotation, display.width(), display.height(), linesPerPage);
    display.clearScreen();
}

void scanFiles() {
    fileCount = 0;
    // Check both root and /books directory
    const char* dirs[] = {"/books", "/"};
    for (int d = 0; d < 2; d++) {
        File root = SD.open(dirs[d]);
        if (!root || !root.isDirectory()) continue;
        
        while (fileCount < MAX_FILES) {
            File entry = root.openNextFile();
            if (!entry) break;
            
            if (!entry.isDirectory()) {
                String name = String(entry.name());
                if (name.endsWith(".txt")) {
                    // entry.name() may return the full path on ESP32 SD;
                    // reduce to basename so we can build a clean path.
                    int slash = name.lastIndexOf('/');
                    if (slash >= 0) name = name.substring(slash + 1);

                    if (d == 0) {
                        fileList[fileCount++] = "/books/" + name;
                    } else {
                        fileList[fileCount++] = "/" + name;
                    }
                    Serial.printf("Found book: %s\n", fileList[fileCount-1].c_str());
                }
            }
            entry.close();
        }
        root.close();
        if (fileCount > 0) break; // If we found books in /books, don't look in root
    }
    Serial.printf("Scan complete. Total books: %d\n", fileCount);
}

void displayFileMenu() {
    Serial.printf("Displaying Menu - Selected Index: %d, Count: %d\n", selectedFileIndex, fileCount);

    display.setRotation(currentRotation);
    const int W = display.width();
    const int H = display.height();

    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);

        // Header
        display.setTextColor(GxEPD_BLACK);
        display.setCursor(10, 25);
        display.print("SELECT BOOK:");
        display.drawFastHLine(0, 35, W, GxEPD_BLACK);

        // Footer with navigation hints
        display.drawFastHLine(0, H - 20, W, GxEPD_BLACK);
        display.setCursor(10, H - 5);
        display.print("k:up j:dn Enter:open r:rot");

        const int startY = 65;
        const int stepY = 25;
        const int barH = 22;
        const int barTopOffset = 18; // distance from text baseline to top of bar

        if (fileCount == 0) {
            display.setTextColor(GxEPD_BLACK);
            display.setCursor(10, startY);
            display.print("No .txt files found.");
        }

        for (int i = 0; i < fileCount; i++) {
            int y = startY + (i * stepY);

            // Show basename only for readability
            String label = fileList[i];
            int slash = label.lastIndexOf('/');
            if (slash >= 0) label = label.substring(slash + 1);

            if (i == selectedFileIndex) {
                display.fillRect(0, y - barTopOffset, W, barH, GxEPD_BLACK);
                display.setTextColor(GxEPD_WHITE, GxEPD_BLACK);
                display.setCursor(10, y);
                display.print("> ");
                display.print(label);
            } else {
                display.setTextColor(GxEPD_BLACK, GxEPD_WHITE);
                display.setCursor(10, y);
                display.print("  ");
                display.print(label);
            }
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
    
    display.setFullWindow();
    display.firstPage();
    do {
        // RESET FILE POSITION FOR EACH PAGE OF DISPLAY UPDATE
        currentBook.seek(position);
        
        display.fillScreen(GxEPD_WHITE);
        display.setCursor(margin, margin + lineHeight);
        
        for (int i = 0; i < linesPerPage; i++) {
            if (currentBook.available()) {
                String line = currentBook.readStringUntil('\n');
                display.println(line);
            } else {
                break;
            }
        }
    } while (display.nextPage());

    // Update pagination info after the display loop finishes
    if (!bookFullyIndexed && (currentPage + 1 < maxPages)) {
        // We need to advance the file pointer to the end of what was just rendered
        currentBook.seek(position);
        for (int i = 0; i < linesPerPage; i++) {
            if (currentBook.available()) {
                currentBook.readStringUntil('\n');
            } else {
                bookFullyIndexed = true;
                break;
            }
        }
        if(!bookFullyIndexed){
            if(currentBook.available()){
                pageStartPositions[currentPage + 1] = currentBook.position();
            } else {
                bookFullyIndexed = true;
            }
        }
    }
    Serial.printf("Page %d rendered. Start position: %lu\n", currentPage, position);
}

