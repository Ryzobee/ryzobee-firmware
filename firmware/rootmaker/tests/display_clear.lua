-- Public display interface agreed with the user: clear, rectangles, text.
-- read_pixel inspects our canvas, not the LCD; visible output needs eye checking.
local display = require("display")
display.clear(0x001F)
assert(display.read_pixel(0, 0) == 0x001F)
assert(display.read_pixel(239, 239) == 0x001F)
assert(display.show())
print("DISPLAY_CLEAR_PASS", "expected screen: solid blue")
