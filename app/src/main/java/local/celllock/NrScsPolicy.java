package local.celllock;

/** Bounded defaults for frequency-only NR locking; never a live SCS measurement. */
final class NrScsPolicy {
    static final class Selection {
        final int code;
        final String reason;
        final String bands;

        Selection(int code, String reason, String bands) {
            this.code = code;
            this.reason = reason;
            this.bands = bands;
        }
    }

    private NrScsPolicy() {}

    static Selection resolve(int arfcn) {
        // These are defaults observed in Cellular-Pro 1.9.10 (yn.n), with
        // frequency ranges from lv.B. Deliberately cover only a small FR1
        // allowlist. All retained candidate bands have the same default.
        // Do not infer a unique band, channel support, or the live SCS here.
        if (between(arfcn, 185000, 192000)) return automatic(0, "n8");
        if (between(arfcn, 151601, 158199)) return automatic(0, "n28");
        if (between(arfcn, 361000, 376000)) {
            return automatic(1, arfcn >= 370000 ? "n3/n2/n25" : "n3");
        }
        if (between(arfcn, 499200, 538000)) {
            String bands = "n41/n90";
            if (between(arfcn, 500000, 514000) || between(arfcn, 524000, 538000)) bands += "/n7";
            if (between(arfcn, 514000, 524000)) bands += "/n38";
            return automatic(1, bands);
        }
        if (between(arfcn, 620000, 680000)) {
            String bands = arfcn <= 653333 ? "n77/n78" : "n77";
            if (between(arfcn, 636667, 646666)) bands += "/n48";
            return automatic(1, bands);
        }
        if (between(arfcn, 693334, 733333)) return automatic(1, "n79");

        // Even familiar bands can overlap another band's different default.
        // No country/serving-band guess or unconditional 30 kHz fallback.
        if (between(arfcn, 422000, 434000)) return ambiguous("n1/n66");
        if (between(arfcn, 158200, 160600)) return ambiguous("n28/n20");
        return new Selection(-1, "无法自动确定 SCS，请在高级设置中选择", "");
    }

    private static Selection automatic(int code, String bands) {
        return new Selection(code, "按频段默认 " + (code == 0 ? "15" : "30") + " kHz", bands);
    }

    private static Selection ambiguous(String bands) {
        return new Selection(-1, "频段默认值不同，请在高级设置中选择 SCS", bands);
    }

    private static boolean between(int value, int first, int last) {
        return value >= first && value <= last;
    }
}
