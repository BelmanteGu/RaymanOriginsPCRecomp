/* The on-screen controls' drawings, embedded in the executable: private/include/touch_art.rgba, made by
 * tools/touch_art.py from runtime/assets/touch/ (the PNG art of the Xbox 360 port's touch controls): 7 pictures of
 * 256x256 RGBA, premultiplied alpha, in the order of touch.c's ART_* names. Found through the include path (-I). */
__asm__(".section .rodata\n"
        ".global touch_art\n"
        ".balign 16\n"
        "touch_art:\n"
        ".incbin \"touch_art.rgba\"\n"
        ".previous\n");
