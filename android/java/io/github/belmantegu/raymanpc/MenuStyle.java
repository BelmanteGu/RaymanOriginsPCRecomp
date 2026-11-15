package io.github.belmantegu.raymanpc;

import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.LayerDrawable;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * The launcher's look, after Rayman Origins' own menus: white bold italic capitals with a dark shadow,
 * the selected line on an orange-to-red bar that fades out to the right, black panels with a silver edge, button
 * hints in grey pills at the bottom and a heading on a red brush-like stroke.
 */
final class MenuStyle {
    static final int ORANGE = Color.rgb(0xF0, 0x7A, 0x1E);
    static final int RED = Color.rgb(0xC8, 0x1E, 0x12);
    static final int DARK_RED = Color.rgb(0x5A, 0x08, 0x06);
    static final int SILVER = Color.rgb(0xC9, 0xCC, 0xD2);
    static final int PANEL = Color.argb(215, 0, 0, 0);
    static final int MUTED = Color.argb(200, 255, 255, 255);

    private final Context ctx;
    final Typeface heavy, plain;

    MenuStyle(Context ctx) {
        this.ctx = ctx;
        heavy = Typeface.create("sans-serif-condensed", Typeface.BOLD_ITALIC);
        plain = Typeface.create("sans-serif", Typeface.NORMAL);
    }

    int dp(float v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, ctx.getResources().getDisplayMetrics());
    }

    TextView text(String s, float sp, int color, Typeface tf) {
        TextView t = new TextView(ctx);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextColor(color);
        t.setTypeface(tf);
        t.setShadowLayer(dp(3), dp(1), dp(2), Color.argb(230, 0, 0, 0));
        t.setIncludeFontPadding(false);
        return t;
    }

    /** A heading: white italic capitals on a slanted red stroke (the "MAIN MENU" title). */
    TextView heading(String s) {
        TextView t = text(s, 30, Color.WHITE, heavy);
        t.setLetterSpacing(0.02f);
        GradientDrawable stroke = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                new int[] {Color.argb(0, 0xC8, 0x1E, 0x12), RED, Color.argb(230, 0xA0, 0x10, 0x0C), Color.argb(0, 0x80, 0, 0)});
        LayerDrawable bg = new LayerDrawable(new android.graphics.drawable.Drawable[] {stroke});
        bg.setLayerInset(0, 0, dp(16), 0, dp(2));
        t.setBackground(bg);
        t.setPadding(dp(14), dp(2), dp(40), dp(4));
        return t;
    }

    /** A menu line; select() puts it on the orange bar. */
    TextView item(String s) {
        TextView t = text(s.toUpperCase(), 20, Color.WHITE, heavy);
        t.setPadding(dp(18), dp(7), dp(90), dp(7));
        t.setGravity(Gravity.CENTER_VERTICAL);
        t.setClickable(true);
        select(t, false);
        return t;
    }

    void select(TextView t, boolean on) {
        if (on) {
            GradientDrawable bar = new GradientDrawable(GradientDrawable.Orientation.LEFT_RIGHT,
                    new int[] {ORANGE, Color.rgb(0xD8, 0x4A, 0x14), RED, Color.argb(160, 0x8A, 0x10, 0x0A), Color.argb(0, 0x50, 0, 0)});
            t.setBackground(bar);
            t.setTextColor(Color.WHITE);
        } else {
            t.setBackground(null);
            t.setTextColor(Color.argb(235, 255, 255, 255));
        }
    }

    /** A black panel with a silver edge (the game's dialogs). */
    LinearLayout panel() {
        LinearLayout p = new LinearLayout(ctx);
        p.setOrientation(LinearLayout.VERTICAL);
        GradientDrawable g = new GradientDrawable();
        g.setColor(PANEL);
        g.setCornerRadius(dp(4));
        g.setStroke(dp(2), SILVER);
        p.setBackground(g);
        p.setPadding(dp(16), dp(12), dp(16), dp(12));
        return p;
    }

    /** A button hint: a grey pill (the pad button) and its word, as in the game's bottom bar. */
    View hint(String label) {
        LinearLayout row = new LinearLayout(ctx);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        View pill = new View(ctx);
        GradientDrawable g = new GradientDrawable(GradientDrawable.Orientation.TOP_BOTTOM,
                new int[] {Color.rgb(0x9A, 0x9C, 0xA2), Color.rgb(0x4A, 0x4C, 0x52)});
        g.setCornerRadius(dp(4));
        g.setStroke(dp(1), Color.rgb(0x20, 0x20, 0x24));
        pill.setBackground(g);
        row.addView(pill, new LinearLayout.LayoutParams(dp(44), dp(16)));
        TextView t = text(label.toUpperCase(), 12, Color.WHITE, plain);
        t.setPadding(dp(8), 0, dp(18), 0);
        row.addView(t);
        return row;
    }

    /** The bottom bar holding the hints. */
    LinearLayout hintBar() {
        LinearLayout bar = new LinearLayout(ctx);
        bar.setOrientation(LinearLayout.HORIZONTAL);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        GradientDrawable g = new GradientDrawable();
        g.setColor(Color.argb(190, 10, 10, 12));
        g.setCornerRadius(dp(16));
        g.setStroke(dp(1), Color.argb(120, 0xC9, 0xCC, 0xD2));
        bar.setBackground(g);
        bar.setPadding(dp(14), dp(6), dp(4), dp(6));
        return bar;
    }

    /** A thin orange progress bar. */
    android.widget.ProgressBar progress() {
        android.widget.ProgressBar p = new android.widget.ProgressBar(ctx, null, android.R.attr.progressBarStyleHorizontal);
        p.setMax(1000);
        p.setProgressTintList(android.content.res.ColorStateList.valueOf(ORANGE));
        p.setProgressBackgroundTintList(android.content.res.ColorStateList.valueOf(Color.argb(120, 255, 255, 255)));
        return p;
    }
}
