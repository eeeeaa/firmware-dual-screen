#include "scrollableTextArea.h"

#include <esp_heap_caps.h>

#define _scrollBuffer tft

// linesBuffer holds the whole file, so an unbounded load aborts on any file
// bigger than the free heap (a sniffer pcap easily is). Keep this much heap for
// the rest of the UI and stop loading instead.
static constexpr size_t HEAP_KEEP_FREE = 64 * 1024;
static constexpr size_t HEAP_ABORT_FLOOR = 32 * 1024;
static constexpr size_t MAX_STORED_LINES = 4096;
static constexpr size_t MAX_LINE_READ = 1024;

static size_t internalFreeHeap() {
    return heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// How many lines fit in the heap we can spare, at ~1 vector slot + 1 String
// buffer (rounded up to 16 bytes by Arduino String) + malloc header per line.
static size_t lineBudget(uint16_t charsPerLine) {
    size_t freeHeap = internalFreeHeap();
    if (freeHeap <= HEAP_KEEP_FREE) return 0;

    size_t perLine = sizeof(String) + ((charsPerLine + 16) & ~(size_t)0xf) + 8;
    size_t lines = (freeHeap - HEAP_KEEP_FREE) / perLine;
    return lines > MAX_STORED_LINES ? MAX_STORED_LINES : lines;
}

// A pcap or any other binary file is unreadable as text, so hex dump it instead.
static bool looksBinary(File &file) {
    uint8_t buf[256];
    int read = file.read(buf, sizeof(buf));
    file.seek(0);
    if (read <= 0) return false;

    size_t control = 0;
    for (int i = 0; i < read; i++) {
        uint8_t c = buf[i];
        if (c == 0) return true;
        if (c < 0x09 || (c > 0x0d && c < 0x20)) control++;
    }
    return control * 10 > (size_t)read;
}

// "OOOO  HH HH .. |ascii|" laid out to fit one text-area line.
static size_t hexBytesPerLine(uint16_t charsPerLine, uint8_t offsetDigits) {
    for (size_t bytes = 16; bytes > 4; bytes /= 2) {
        if (offsetDigits + 2 + 4 * bytes + 2 <= charsPerLine) return bytes;
    }
    return 4;
}

// readStringUntil() is unbounded: binary data without a 0x0a byte grows one
// String until the heap runs out.
static String readBoundedLine(File &file, size_t maxLength) {
    String line;
    line.reserve(maxLength + 1);

    while (file.available() && line.length() < maxLength) {
        int c = file.read();
        if (c < 0 || c == '\n') break;
        line += (char)c;
    }
    return line;
}

ScrollableTextArea::ScrollableTextArea(const String &title)
    : firstVisibleLine{0}, _redraw{true}, _title(title), _fontSize(FP), _startX(BORDER_PAD_X),
      _startY(BORDER_PAD_Y), _width(tftWidth - 2 * BORDER_PAD_X),
      _height(tftHeight - 4 - BORDER_PAD_X - BORDER_PAD_Y) {
    drawMainBorder();

    if (!_title.isEmpty()) {
        printTitle(_title);
        _startY = tft.getCursorY();
        _height -= (_startY - BORDER_PAD_Y);
    }

    setup();
}

ScrollableTextArea::ScrollableTextArea(
    uint8_t fontSize, int16_t startX, int16_t startY, int32_t width, int32_t height, bool drawBorders,
    bool indentWrappedLines
)
    : firstVisibleLine{0}, _redraw{true}, _title(""), _fontSize(fontSize), _startX(startX), _startY(startY),
      _width(width), _height(height), _indentWrappedLines(indentWrappedLines) {
    if (drawBorders) { drawMainBorder(); }
    setup();
}

ScrollableTextArea::~ScrollableTextArea() {
    // We don't use Sprites for big things, unfortunetly theres no much RAM in all devices
}

void ScrollableTextArea::setup() {
    _scrollBuffer.setTextColor(bruceConfig.priColor);
    _scrollBuffer.setTextSize(_fontSize);
    _scrollBuffer.fillRect(_startX, _startY, _width, _height, bruceConfig.bgColor);

    _maxCharactersPerLine = floor(_width / _scrollBuffer.textWidth("w", _fontSize));
    _pixelsPerLine = _scrollBuffer.fontHeight() + 2;
    _maxVisibleLines = floor(_height / _pixelsPerLine);
}

void ScrollableTextArea::scrollUp() {
    if (firstVisibleLine) {
        firstVisibleLine--;
        _redraw = true;
    }
}

void ScrollableTextArea::scrollDown() {
    if (firstVisibleLine + _maxVisibleLines <= linesBuffer.size()) {
        if (firstVisibleLine == 0) firstVisibleLine++;
        firstVisibleLine++;
        _redraw = true;
    }
}

void ScrollableTextArea::scrollToLine(size_t lineNumber) {
    if (linesBuffer.empty()) return; // Ensure there's content to scroll

    if (lineNumber > linesBuffer.size() - _maxVisibleLines) {
        firstVisibleLine =
            (linesBuffer.size() > _maxVisibleLines) ? linesBuffer.size() - _maxVisibleLines : 0;
    } else {
        firstVisibleLine = lineNumber;
    }
}

String ScrollableTextArea::getLine(size_t lineNumber) {
    return linesBuffer[(lineNumber >= linesBuffer.size()) ? linesBuffer.size() : lineNumber];
}

size_t ScrollableTextArea::getMaxLines() { return linesBuffer.size(); }

void ScrollableTextArea::show(bool force) {
    draw(force);

    while (check(SelPress)) {
        update(force);
        yield();
    }
    while (!check(SelPress)) {
        update(force);
        yield();
    }
}

uint32_t ScrollableTextArea::getMaxVisibleTextLength() { return _maxVisibleLines * _maxCharactersPerLine; }

void ScrollableTextArea::update(bool force) {
#ifdef HAS_ENCODER
    int32_t rotarySteps = drainRotarySteps();
    if (rotarySteps != 0) {
        check(PrevPress);
        check(NextPress);
        check(UpPress);
        check(DownPress);
        while (rotarySteps > 0) {
            scrollUp();
            rotarySteps--;
        }
        while (rotarySteps < 0) {
            scrollDown();
            rotarySteps++;
        }
        vTaskDelay(4 / portTICK_PERIOD_MS);
        PrevPress = false;
        NextPress = false;
        UpPress = false;
        DownPress = false;
    }
#endif

    if (check(PrevPress) || check(UpPress)) scrollUp();
    else if (check(NextPress) || check(DownPress)) scrollDown();

    draw(force);
}

void ScrollableTextArea::fromFile(File file) {
    linesBuffer.clear();

    const uint16_t perLine = _maxCharactersPerLine ? _maxCharactersPerLine : 1;
    const size_t maxLines = lineBudget(perLine);
    const size_t fileSize = file.size();
    size_t shownBytes = 0;
    bool truncated = false;

    if (maxLines == 0) {
        addLine("Not enough free memory to open this file.");
    } else {
        linesBuffer.reserve(maxLines + 1);

        if (looksBinary(file)) {
            const uint8_t offsetDigits = fileSize > 0xffff ? 6 : 4;
            const size_t bytesPerLine = hexBytesPerLine(perLine, offsetDigits);
            const bool withAscii = offsetDigits + 2 + 4 * bytesPerLine + 2 <= perLine;
            uint8_t buf[16];
            char line[80];

            while (file.available()) {
                if (linesBuffer.size() >= maxLines || internalFreeHeap() < HEAP_ABORT_FLOOR) {
                    truncated = true;
                    break;
                }

                int read = file.read(buf, bytesPerLine);
                if (read <= 0) break;

                char *out = line;
                out += snprintf(out, sizeof(line), "%0*X  ", offsetDigits, (unsigned)shownBytes);
                for (int i = 0; i < (int)bytesPerLine; i++) {
                    if (i < read) out += snprintf(out, 4, "%02X ", buf[i]);
                    else if (withAscii) out += snprintf(out, 4, "   ");
                }
                if (withAscii) {
                    *out++ = '|';
                    for (int i = 0; i < read; i++) {
                        uint8_t c = buf[i];
                        *out++ = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
                    }
                    *out++ = '|';
                }
                *out = '\0';

                linesBuffer.emplace_back(line);
                shownBytes += read;
            }
            _redraw = true;
        } else {
            while (file.available()) {
                if (linesBuffer.size() >= maxLines || internalFreeHeap() < HEAP_ABORT_FLOOR) {
                    truncated = true;
                    break;
                }

                // A single logical line may wrap over several stored lines
                size_t roomInChars = (maxLines - linesBuffer.size()) * (perLine > 1 ? perLine - 1 : 1);
                String text = readBoundedLine(file, roomInChars < MAX_LINE_READ ? roomInChars : MAX_LINE_READ);

                shownBytes += text.length() + 1;
                addLine(text);
            }
        }

        if (file.available()) truncated = true;
        if (truncated) {
            char note[64];
            snprintf(
                note,
                sizeof(note),
                "-- truncated: %u of %u bytes --",
                (unsigned)shownBytes,
                (unsigned)fileSize
            );
            linesBuffer.emplace(linesBuffer.begin(), note);
        }
    }

    draw(true);
    delay(100);
    draw(true);
}

void ScrollableTextArea::clear() {
    firstVisibleLine = 0;
    linesBuffer.clear();
}

void ScrollableTextArea::fromString(const String &text) {
    clear();
    int startIdx = 0;
    int endIdx = 0;

    while (endIdx < text.length()) {
        if (text[endIdx] == '\n') {
            addLine(text.substring(startIdx, endIdx));
            startIdx = endIdx + 1;
        }

        endIdx++;
    }

    // Add the last line if there's remaining text (text does not ends with \n)
    if (startIdx < text.length()) { addLine(text.substring(startIdx, endIdx)); }
}

// for devices it will act as a scrollable text area
void ScrollableTextArea::addLine(const String &text) {
    if (text.isEmpty()) {
        linesBuffer.emplace_back("");
        return;
    }

    String buff;
    size_t start = 0;
    bool firstLine = true;

    // Automatically split into multiple lines
    while (start < text.length()) {
        size_t len = _maxCharactersPerLine;

        if (!firstLine && _indentWrappedLines) {
            buff = " " + text.substring(start, start + len - 1); // Reduce length for space
            start += len - 1;
        } else {
            buff = text.substring(start, start + len);
            start += len;
        }
        if (buff.endsWith("\r")) buff.remove(buff.length() - 1);

        linesBuffer.emplace_back(buff);
        firstLine = false;
    }

    _redraw = true;
}

void ScrollableTextArea::draw(bool force) {
    if (!_redraw && !force) return;

    _scrollBuffer.fillRect(_startX, _startY, _width, _height, bruceConfig.bgColor);
    _scrollBuffer.setTextColor(bruceConfig.priColor);
    uint8_t _fSize = tft.getTextSize();
    tft.setTextSize(FP);

    uint16_t yOffset = 0;
    size_t lines = 0;

    // if there is text above
    if (firstVisibleLine) {
        _scrollBuffer.drawString("...", 0 + _startX, _startY + yOffset);
        yOffset += _pixelsPerLine;
        lines++;
    }

    int32_t tmpHeight = _height;
    // if there is text below
    if (linesBuffer.size() - firstVisibleLine >= _maxVisibleLines) {
        _scrollBuffer.drawString("...", 0 + _startX, _startY + _height - _pixelsPerLine);
        tmpHeight -= _pixelsPerLine;
        lines++;
    }

    size_t idx{firstVisibleLine};
    while (yOffset < tmpHeight && lines < _maxVisibleLines && idx < linesBuffer.size()) {
        _scrollBuffer.drawString(linesBuffer[idx], 0 + _startX, _startY + yOffset);
        yOffset += _pixelsPerLine;
        lines++;
        idx++;
    }

    lastVisibleLine = firstVisibleLine + lines;
    tft.setTextFont(_fSize);

    _redraw = false;
}
