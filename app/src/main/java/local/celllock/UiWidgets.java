package local.celllock;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.View;

/** Presentation-only widgets. Nothing here is evidence for a lock or recovery state. */
final class UiWidgets {
    private UiWidgets() { }

    static final int GOOD = 0xFF16734A, FAIR = 0xFFB7791F, WEAK = 0xFFB42318, EMPTY = 0xFFD5DBE3;

    /** Display bucket 0–4 for RSRP; -1 when the value is unavailable. */
    static int signalLevel(boolean nr, int rsrp) {
        if (rsrp == Integer.MAX_VALUE || rsrp > -30 || rsrp < -160) return -1;
        int[] limits = nr ? new int[]{-80, -90, -100, -110} : new int[]{-85, -95, -105, -115};
        for (int i = 0; i < limits.length; ++i) if (rsrp >= limits[i]) return 4 - i;
        return 0;
    }

    static int signalColor(int level) { return level >= 3 ? GOOD : level == 2 ? FAIR : WEAK; }

    static String signalWord(int level) {
        return level >= 4 ? "很好" : level == 3 ? "较好" : level == 2 ? "一般" : level == 1 ? "较弱" : "很弱";
    }

    /** Four rising bars; hidden while no reading is available. */
    static final class SignalBars extends View {
        private final Paint paint = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF bar = new RectF();
        private int level = -1;

        SignalBars(Context context) {
            super(context);
            setVisibility(GONE);
        }

        void setLevel(int value) {
            level = value;
            setVisibility(value < 0 ? GONE : VISIBLE);
            setContentDescription(value < 0 ? "" : "信号" + signalWord(value));
            invalidate();
        }

        @Override protected void onDraw(Canvas canvas) {
            if (level < 0) return;
            float width = getWidth(), height = getHeight();
            float gap = width * 0.08f, step = (width - gap * 3) / 4f, radius = step * 0.3f;
            int filled = Math.max(1, level);
            for (int i = 0; i < 4; ++i) {
                float top = height * (0.7f - 0.2f * i);
                float left = i * (step + gap);
                bar.set(left, top, left + step, height);
                paint.setColor(i < filled ? signalColor(level) : EMPTY);
                canvas.drawRoundRect(bar, radius, radius, paint);
            }
        }
    }
}
