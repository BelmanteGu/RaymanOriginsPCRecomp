/* The pad button pictures put into the game's button atlas (pad_icons.c), embedded in the executable:
 * private/include/pad_icons.rgba, made by tools/pad_icons_art.py from runtime/assets/pad_icons.png (the touch
 * controls' balls and ones drawn in their style): 512x256 RGBA and its mip levels. Found through the include path. */
__asm__(".section .rodata\n"
        ".global pad_icons_art\n"
        ".balign 16\n"
        "pad_icons_art:\n"
        ".incbin \"pad_icons.rgba\"\n"
        ".previous\n");
