package local.celllock;

import android.Manifest;
import android.animation.LayoutTransition;
import android.app.Activity;
import android.app.AlertDialog;
import android.app.Dialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.ColorStateList;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.RippleDrawable;
import android.location.LocationManager;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.SystemClock;
import android.telephony.CellIdentityLte;
import android.telephony.CellIdentityNr;
import android.telephony.CellInfo;
import android.telephony.CellInfoLte;
import android.telephony.CellInfoNr;
import android.telephony.CellSignalStrengthNr;
import android.telephony.SubscriptionManager;
import android.telephony.TelephonyManager;
import android.text.InputType;
import android.text.Editable;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowManager;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputMethodManager;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.RadioButton;
import android.widget.RadioGroup;
import android.widget.ScrollView;
import android.widget.Spinner;
import android.widget.TextView;
import android.widget.Toast;
import java.io.OutputStream;
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.ArrayList;
import java.util.Date;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

/** Independent UI. A returned backend message is never interpreted as proof of cell locking. */
public final class MainActivity extends Activity {
    private static final int PERMISSIONS = 41;
    private static final int EXPORT_LOG = 42;
    private static final int INK = 0xFF14263D;
    private static final int MUTED = 0xFF475569;
    private static final int ACTION_BLUE = 0xFF174EA6;
    private static final int LINE = 0xFFE2E8F0, DANGER = 0xFFB42318, WARN_INK = 0xFF85540A;
    private static final int PRIMARY_LOCK = 0, PRIMARY_PROBE = 1, PRIMARY_RESTORE = 2;
    private static final int STYLE_FILLED = 0, STYLE_OUTLINE = 1, STYLE_QUIET = 2, STYLE_SEGMENT = 3;
    // Read-only cell polling after a radio-changing operation; never repeats a backend action.
    private static final long POLL_FIRST_MS = 3000, POLL_INTERVAL_MS = 5000, POLL_WINDOW_MS = 120000;
    static final int MODE_CELL = 0, MODE_EARFCN = 1;
    static final String MODE_PREFERENCE = "lock_mode";
    static final String NR_MODE_PREFERENCE = "nr_lock_mode";
    private static final String RADIO_PREFERENCE = "nr_sa_mode";
    private static final String SCS_TARGET_STATE = "scs_target", SCS_MANUAL_STATE = "scs_manual";
    private static final String NR_BAND_STATE = "nr_target_band";
    private final Handler main = new Handler(Looper.getMainLooper());
    private final List<RadioProbe.SlotEntry> subscriptions = new ArrayList<>();
    private Worker worker;
    private LinearLayout page, simGroup, bandChips, scsRow;
    private final List<StateButton> simButtons = new ArrayList<>();
    private int simPosition = -1;
    private UiWidgets.SignalBars signalBars;
    private ProgressBar progress;
    private TextView inputError, footerHint;
    private boolean pendingFill;
    private final Runnable pollTick = this::pollCell;
    private final Runnable busyTick = this::tickBusy;
    private LinearLayout nrBandGroup;
    private final NrBandChoice bandChoice = new NrBandChoice();
    private NrBandChoice.Selection bandSelection = new NrBandChoice.Selection(-1, NrBandChoice.UNKNOWN, -1);
    private int selectedNrBand = -1;
    private final NrScsChoice scsChoice = new NrScsChoice();
    private int restoredScsSub = -1;
    private SharedPreferences preferences;
    private int lockMode = MODE_CELL;
    private int nrLockMode = MODE_EARFCN;
    private boolean nrSa;
    private EditText earfcnInput, pciInput;
    private TextView cellText, cellMeta, permissionText, readinessLabel, readinessText, resultText, operationText;
    private TextView appliedTargetText, nextStepText, frequencyLabel, scsStatus, nrBandStatus;
    private LinearLayout readinessPanel, permissionPanel, pciGroup, resultPanel, lteModes;
    private Button readButton, fillButton, probeButton, lockButton, restoreButton;
    private RadioButton cellModeButton, frequencyModeButton;
    private Button moreButton, grantButton, detailsButton, lteButton, nrButton;
    private Button advancedButton;
    private int selectedSub = SubscriptionManager.INVALID_SUBSCRIPTION_ID;
    private int selectedSlot = -1;
    private int defaultDataSub = SubscriptionManager.INVALID_SUBSCRIPTION_ID;
    private String subscriptionExplanation = "尚未查询订阅接口。";
    private int preferredSub = SubscriptionManager.INVALID_SUBSCRIPTION_ID;
    private int currentEarfcn = -1, currentPci = -1, currentCellSub = -1;
    private int[] currentNrBands = new int[0];
    private boolean currentNrSa;
    private long currentCellTimeMs;
    private String cellReadAt = "";
    private int readSequence;
    private boolean reading, destroyed;

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        Object retained = getLastNonConfigurationInstance();
        worker = retained instanceof Worker ? (Worker) retained : new Worker(getApplicationContext());
        preferences = getSharedPreferences("lock_target", MODE_PRIVATE);
        lockMode = saved == null ? loadLockMode(preferences) : normalizeLockMode(saved.getInt(MODE_PREFERENCE, loadLockMode(preferences)));
        nrLockMode = saved == null ? loadNrLockMode(preferences) : normalizeNrLockMode(saved.getInt(NR_MODE_PREFERENCE, loadNrLockMode(preferences)));
        nrSa = saved == null ? loadNrSa(preferences) : saved.getBoolean(RADIO_PREFERENCE, loadNrSa(preferences));
        if (saved != null) preferredSub = saved.getInt("subscription", -1);
        createUi();
        if (saved != null) {
            earfcnInput.setText(saved.getString("earfcn", ""));
            pciInput.setText(saved.getString("pci", ""));
            if (nrSa) {
                setNrBand(saved.getInt(NR_BAND_STATE, -1));
                scsChoice.restore(saved.getInt(SCS_TARGET_STATE, -1), saved.getInt(SCS_MANUAL_STATE, -1));
                restoredScsSub = preferredSub;
            }
        }
        worker.attach(this);
        worker.readSystemInfo();
        loadSubscriptions();
    }

    private void createUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xFFF4F6FA);
        ScrollView scroll = new ScrollView(this);
        scroll.setFillViewport(true);
        page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(16), dp(8), dp(16), dp(16));
        // Keeps the soft keyboard closed at launch and gives hideKeyboard() a neutral focus target.
        page.setFocusableInTouchMode(true);
        animateChanges(page);
        scroll.addView(page);
        root.addView(scroll, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);

        LinearLayout header = row();
        TextView title = text("网络锁定", 24, INK);
        title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        header.addView(title, weighted());
        moreButton = button("更多", STYLE_QUIET);
        moreButton.setOnClickListener(v -> showMore());
        header.addView(moreButton, new LinearLayout.LayoutParams(dp(72), -2));
        page.addView(header, width());

        LinearLayout radios = segmentGroup();
        lteButton = button("LTE", STYLE_SEGMENT);
        nrButton = button("5G SA", STYLE_SEGMENT);
        lteButton.setOnClickListener(v -> selectRadio(false));
        nrButton.setOnClickListener(v -> selectRadio(true));
        radios.addView(lteButton, weighted());
        radios.addView(nrButton, weighted());
        LinearLayout.LayoutParams radiosParams = width(); radiosParams.topMargin = dp(8);
        page.addView(radios, radiosParams);

        permissionPanel = card(page, "");
        permissionPanel.setBackground(outlined(0xFFFFF6E8, 0xFFEFD5A9, 16));
        permissionText = text("", 13, WARN_INK);
        permissionPanel.addView(permissionText);
        grantButton = button("授予权限", STYLE_OUTLINE);
        grantButton.setOnClickListener(v -> {
            if (!hasRadioPermissions()) requestRadioPermissions();
            else if (!locationEnabled()) {
                try { startActivity(new Intent(android.provider.Settings.ACTION_LOCATION_SOURCE_SETTINGS)); }
                catch (Exception error) { showError("请在系统设置中开启定位", error); }
            }
            else loadSubscriptions();
        });
        permissionPanel.addView(grantButton, buttonWidth());

        // Device readiness and the serving cell share one card: both decide what the user can do next.
        LinearLayout radio = card(page, "");
        readinessPanel = row();
        readinessPanel.setPadding(dp(14), dp(10), dp(4), dp(10));
        LinearLayout statusCopy = new LinearLayout(this);
        statusCopy.setOrientation(LinearLayout.VERTICAL);
        readinessLabel = text("待检测", 16, INK);
        readinessLabel.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        statusCopy.addView(readinessLabel);
        readinessText = text("请先检测设备", 13, MUTED);
        statusCopy.addView(readinessText);
        readinessPanel.addView(statusCopy, weighted());
        probeButton = button("重新检测", STYLE_QUIET);
        probeButton.setOnClickListener(v -> runProbe());
        readinessPanel.addView(probeButton, new LinearLayout.LayoutParams(-2, -2));
        radio.addView(readinessPanel, width());

        LinearLayout cellHeading = row();
        cellHeading.setPadding(0, dp(14), 0, 0);
        TextView cellTitle = text("当前驻留", 16, INK);
        cellTitle.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        cellHeading.addView(cellTitle, weighted());
        readButton = button("刷新", STYLE_QUIET);
        readButton.setOnClickListener(v -> readCurrentCell());
        cellHeading.addView(readButton, new LinearLayout.LayoutParams(-2, -2));
        radio.addView(cellHeading, width());
        simGroup = segmentGroup();
        LinearLayout.LayoutParams simParams = width(); simParams.topMargin = dp(6);
        radio.addView(simGroup, simParams);
        LinearLayout cellRow = row();
        cellRow.setPadding(0, dp(12), 0, dp(2));
        cellText = text("点击刷新，查看当前小区", 20, INK);
        cellText.setTextIsSelectable(true);
        cellRow.addView(cellText, weighted());
        signalBars = new UiWidgets.SignalBars(this);
        LinearLayout.LayoutParams barParams = new LinearLayout.LayoutParams(dp(26), dp(20));
        barParams.leftMargin = dp(10);
        cellRow.addView(signalBars, barParams);
        radio.addView(cellRow, width());
        cellMeta = text("", 12, MUTED);
        radio.addView(cellMeta, width());
        fillButton = button("填入当前小区", STYLE_OUTLINE);
        fillButton.setOnClickListener(v -> fillCurrentTarget());
        radio.addView(fillButton, buttonWidth());

        LinearLayout target = card(page, "锁定目标");
        TextView modeLabel = text("锁定方式", 12, MUTED);
        target.addView(modeLabel);
        RadioGroup modes = new RadioGroup(this);
        modes.setOrientation(RadioGroup.HORIZONTAL);
        lteModes = modes;
        frequencyModeButton = modeOption("仅频点");
        cellModeButton = modeOption("频点＋PCI");
        frequencyModeButton.setOnClickListener(v -> selectMode(MODE_EARFCN));
        cellModeButton.setOnClickListener(v -> selectMode(MODE_CELL));
        modes.addView(frequencyModeButton, weighted());
        modes.addView(cellModeButton, weighted());
        target.addView(modes, width());
        addSpace(target, 8);
        LinearLayout fields = row();
        fields.setGravity(Gravity.TOP);
        LinearLayout earfcnGroup = new LinearLayout(this);
        earfcnGroup.setOrientation(LinearLayout.VERTICAL);
        frequencyLabel = text("EARFCN", 12, MUTED);
        earfcnGroup.addView(frequencyLabel);
        earfcnInput = input("输入频点");
        earfcnInput.setContentDescription("LTE EARFCN");
        earfcnGroup.addView(earfcnInput, fieldParams());
        fields.addView(earfcnGroup, weighted());
        pciGroup = new LinearLayout(this);
        pciGroup.setOrientation(LinearLayout.VERTICAL);
        pciGroup.addView(text("PCI", 12, MUTED));
        pciInput = input("输入 PCI");
        pciInput.setContentDescription("LTE PCI");
        pciInput.setImeOptions(EditorInfo.IME_ACTION_DONE);
        pciGroup.addView(pciInput, fieldParams());
        LinearLayout.LayoutParams pciParams = weighted(); pciParams.leftMargin = dp(12);
        fields.addView(pciGroup, pciParams);
        target.addView(fields, width());
        earfcnInput.setOnEditorActionListener((view, action, event) -> {
            if (action == EditorInfo.IME_ACTION_NEXT && pciGroup.getVisibility() == View.VISIBLE) {
                pciInput.requestFocus();
                return true;
            }
            if (action == EditorInfo.IME_ACTION_NEXT || action == EditorInfo.IME_ACTION_DONE) { hideKeyboard(); return true; }
            return false;
        });
        pciInput.setOnEditorActionListener((view, action, event) -> {
            if (action == EditorInfo.IME_ACTION_DONE) { hideKeyboard(); return true; }
            return false;
        });
        inputError = text("", 12, DANGER);
        inputError.setPadding(dp(2), dp(6), 0, 0);
        inputError.setVisibility(View.GONE);
        target.addView(inputError, width());
        nrBandGroup = new LinearLayout(this);
        nrBandGroup.setOrientation(LinearLayout.VERTICAL);
        nrBandGroup.setPadding(0, dp(12), 0, 0);
        LinearLayout bandRow = row();
        bandRow.setMinimumHeight(dp(44));
        bandRow.addView(text("频段", 13, MUTED), new LinearLayout.LayoutParams(dp(44), -2));
        bandChips = segmentGroup();
        LinearLayout.LayoutParams chipParams = new LinearLayout.LayoutParams(-2, -2);
        chipParams.rightMargin = dp(12);
        bandRow.addView(bandChips, chipParams);
        nrBandStatus = text("填写频点后自动匹配", 12, MUTED);
        bandRow.addView(nrBandStatus, weighted());
        nrBandGroup.addView(bandRow, width());
        scsRow = row();
        scsRow.setPadding(0, dp(6), 0, 0);
        scsStatus = text("", 13, MUTED);
        scsRow.addView(scsStatus, weighted());
        advancedButton = button("高级设置", STYLE_QUIET);
        advancedButton.setOnClickListener(v -> showNrAdvanced());
        scsRow.addView(advancedButton, new LinearLayout.LayoutParams(-2, -2));
        nrBandGroup.addView(scsRow, width());
        target.addView(nrBandGroup, width());
        earfcnInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
                scsChoice.targetChanged(currentNrTarget());
                updateNrBandChoices();
                renderBackendState();
            }
            @Override public void afterTextChanged(Editable value) { }
        });
        pciInput.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int start, int count, int after) { }
            @Override public void onTextChanged(CharSequence s, int start, int before, int count) {
                if (nrSa && nrLockMode == MODE_CELL) {
                    scsChoice.clearOverride();
                    updateNrBandChoices();
                }
                renderBackendState();
            }
            @Override public void afterTextChanged(Editable value) { }
        });
        appliedTargetText = text("", 13, MUTED);
        appliedTargetText.setPadding(0, dp(10), 0, 0);
        target.addView(appliedTargetText, width());

        resultPanel = card(page, "");
        LinearLayout resultHeading = row();
        TextView resultTitle = text("最近操作", 14, INK);
        resultTitle.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        resultHeading.addView(resultTitle);
        operationText = text("", 12, MUTED);
        operationText.setPadding(dp(8), 0, 0, 0);
        resultHeading.addView(operationText, weighted());
        detailsButton = button("详情", STYLE_QUIET);
        detailsButton.setOnClickListener(v -> showOperationDetails());
        resultHeading.addView(detailsButton, new LinearLayout.LayoutParams(-2, -2));
        resultPanel.addView(resultHeading, width());
        resultText = text("", 15, INK);
        resultText.setTextIsSelectable(true);
        resultPanel.addView(resultText, width());
        nextStepText = text("", 13, MUTED);
        nextStepText.setPadding(0, dp(4), 0, 0);
        resultPanel.addView(nextStepText, width());

        LinearLayout footer = new LinearLayout(this);
        footer.setOrientation(LinearLayout.VERTICAL);
        footer.setPadding(dp(16), dp(6), dp(16), dp(12));
        footer.setBackgroundColor(Color.WHITE);
        footer.setElevation(dp(8));
        animateChanges(footer);
        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setIndeterminate(true);
        progress.setIndeterminateTintList(ColorStateList.valueOf(ACTION_BLUE));
        progress.setVisibility(View.GONE);
        footer.addView(progress, width());
        footerHint = text("", 13, INK);
        footerHint.setPadding(0, dp(4), 0, dp(8));
        footer.addView(footerHint, width());
        LinearLayout actions = row();
        lockButton = button("开始锁定", STYLE_FILLED);
        lockButton.setMinimumHeight(dp(52));
        lockButton.setOnClickListener(v -> runPrimaryAction());
        actions.addView(lockButton, weighted());
        restoreButton = button("恢复原配置", STYLE_OUTLINE);
        restoreButton.setMinimumHeight(dp(52));
        restoreButton.setOnClickListener(v -> requestRestore());
        LinearLayout.LayoutParams restoreParams = weighted(); restoreParams.leftMargin = dp(10);
        actions.addView(restoreButton, restoreParams);
        footer.addView(actions, width());
        root.addView(footer, width());
    }

    private void selectMode(int mode) {
        if (!canChangeMode()) return;
        if (nrSa) {
            int selectedMode = normalizeNrLockMode(mode);
            if (nrLockMode != selectedMode) { scsChoice.clearOverride(); setNrBand(-1); }
            nrLockMode = selectedMode;
            saveNrLockMode(preferences, nrLockMode);
        } else {
            lockMode = normalizeLockMode(mode);
            saveLockMode(preferences, lockMode);
        }
        renderBackendState();
    }

    private int activeLockMode() { return nrSa ? nrLockMode : lockMode; }

    private boolean canChangeMode() {
        return modeChangeAllowed(worker.busy, worker.backendAttempted, worker.recoveryUnknown, worker.restorable);
    }

    private void selectRadio(boolean selectedNrSa) {
        if (!canChangeMode() || nrSa == selectedNrSa) return;
        nrSa = selectedNrSa;
        scsChoice.clearOverride();
        setNrBand(-1);
        preferences.edit().putBoolean(RADIO_PREFERENCE, nrSa).apply();
        worker.ready = false;
        worker.probed = false;
        worker.restoreNotice.begin(true);
        if (worker.backend != null) worker.backend.invalidateReadiness();
        earfcnInput.setText("");
        pciInput.setText("");
        stopPolling();
        invalidateCellEvidence(idleCellText());
        renderBackendState();
        autoReadCell();
    }

    private boolean canFillCurrentTarget() {
        return currentNrSa == nrSa && (nrSa
            ? canFillNrTarget(nrLockMode, currentCellSub, selectedSub, currentEarfcn, currentPci,
                currentCellTimeMs, SystemClock.elapsedRealtime())
            : canFillTarget(lockMode, currentCellSub, selectedSub, currentEarfcn, currentPci));
    }

    private int currentNrTarget() {
        if (!nrSa) return -1;
        try { return parseNrArfcn(earfcnInput.getText().toString()); }
        catch (IllegalArgumentException invalid) { return -1; }
    }

    private void updateNrBandChoices() {
        int target = currentNrTarget();
        bandChoice.targetChanged(target, currentNrPci(), nrLockMode == MODE_CELL);
        boolean usable = nrSa && currentNrSa && currentCellSub == selectedSub && selectedSub >= 0
            && nrObservationCurrent(currentCellTimeMs, SystemClock.elapsedRealtime());
        bandSelection = bandChoice.resolve(currentEarfcn, currentPci, currentNrBands, usable);
        if (selectedNrBand != bandSelection.band) scsChoice.clearOverride();
        selectedNrBand = bandSelection.band;
    }

    private NrBandChoice.Selection automaticNrBand() {
        boolean usable = nrSa && currentNrSa && currentCellSub == selectedSub && selectedSub >= 0
            && nrObservationCurrent(currentCellTimeMs, SystemClock.elapsedRealtime());
        return bandChoice.automatic(currentEarfcn, currentPci, currentNrBands, usable);
    }

    private void setNrBand(int band) {
        updateNrBandChoices();
        bandChoice.choose(NrBandPolicy.find(currentNrTarget(), band) == null ? -1 : band);
        renderNrScs();
    }

    private int currentNrPci() {
        if (pciInput == null) return -1;
        String value = pciInput.getText().toString().trim();
        if (!value.matches("[0-9]+")) return -1;
        try { int pci = Integer.parseInt(value); return validNrPci(pci) ? pci : -1; }
        catch (NumberFormatException invalid) { return -1; }
    }

    private void renderNrScs() {
        if (advancedButton == null || scsStatus == null) return;
        updateNrBandChoices();
        int target = currentNrTarget();
        int manual = scsChoice.manualCode();
        renderBandChips(target);
        String bandStatus = target < 0 ? "填写频点后自动匹配" : selectedNrBand > 0
            ? (bandSelection.source == NrBandChoice.OBSERVED ? "来自本次读取"
                : bandSelection.source == NrBandChoice.MANUAL ? "手动选择 · 再点一次恢复自动" : "按频点匹配")
            : NrBandPolicy.candidates(target).isEmpty() ? "暂无匹配规则"
                : "频点位于重叠范围，请点选目标频段";
        if (bandSelection.conflicts()) bandStatus = "与当前上报 n" + bandSelection.observedBand + " 不同，请核对目标";
        nrBandStatus.setText(bandStatus);
        nrBandStatus.setTextColor(bandSelection.conflicts() ? DANGER : MUTED);
        final String status;
        boolean pending = false;
        if (target < 0) status = "SCS · 随频段默认";
        else if (manual >= 0) status = "SCS " + scsKhz(manual) + " kHz · 手动";
        else {
            int code = nrScsSelection(nrLockMode, target, selectedNrBand).code;
            pending = code < 0 && !(nrLockMode == MODE_CELL && selectedNrBand < 0);
            status = code >= 0 ? "SCS " + scsKhz(code) + " kHz · 频段默认"
                : pending ? "SCS 待确认 · 请在高级设置中选择" : "SCS · 确认频段后决定";
        }
        scsStatus.setText(status);
        scsStatus.setTextColor(pending ? WARN_INK : MUTED);
        advancedButton.setEnabled(canChangeMode() && target >= 0);
    }

    private String bandChipKey = "";

    /** One chip per supported candidate; overlapping ranges stay visible instead of being guessed. */
    private void renderBandChips(int target) {
        boolean enabled = canChangeMode();
        String key = target + ":" + selectedNrBand + ":" + enabled;
        if (key.equals(bandChipKey)) return;
        bandChipKey = key;
        bandChips.removeAllViews();
        List<NrBandPolicy.Band> candidates = NrBandPolicy.candidates(target);
        bandChips.setVisibility(candidates.isEmpty() ? View.GONE : View.VISIBLE);
        if (candidates.isEmpty()) return;
        for (NrBandPolicy.Band band : candidates) {
            StateButton chip = new StateButton(band.label, STYLE_SEGMENT);
            chip.setPrimary(band.number == selectedNrBand);
            chip.setEnabled(enabled);
            chip.setContentDescription("NR 频段 " + band.label + (band.number == selectedNrBand ? "，已选择" : ""));
            chip.setOnClickListener(v -> chooseBandChip(band.number));
            bandChips.addView(chip, new LinearLayout.LayoutParams(dp(76), -2));
        }
    }

    /** Tapping the active chip, or the automatic band, returns to automatic matching. */
    private void chooseBandChip(int band) {
        if (!nrSa || !canChangeMode()) return;
        updateNrBandChoices();
        int automatic = automaticNrBand().band;
        int manual = band == selectedNrBand || band == automatic ? -1 : band;
        try { bandChoice.choose(manual); }
        catch (IllegalArgumentException error) { Toast.makeText(this, error.getMessage(), Toast.LENGTH_SHORT).show(); return; }
        renderBackendState();
    }

    /** SCS only; the band is chosen with the chips on the main page. */
    private void showNrAdvanced() {
        if (!nrSa || !canChangeMode()) return;
        updateNrBandChoices();
        final int target = currentNrTarget();
        if (target < 0) { Toast.makeText(this, "请先填写 NR-ARFCN", Toast.LENGTH_SHORT).show(); return; }
        final int subscription = selectedSub, mode = nrLockMode, band = selectedNrBand;
        final String pci = pciInput.getText().toString();
        final NrScsPolicy.Selection automatic = nrScsSelection(mode, target, band);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), dp(8), dp(24), dp(8));
        content.addView(text("NR-ARFCN " + target + (band > 0 ? " · n" + band : " · 频段待确认"), 13, MUTED));
        content.addView(text("SCS", 13, INK), buttonWidth());
        Spinner scsPicker = new Spinner(this);
        scsPicker.setMinimumHeight(dp(48));
        scsPicker.setContentDescription("SCS，自动或手动");
        ArrayAdapter<String> scsAdapter = new ArrayAdapter<>(this, android.R.layout.simple_spinner_item,
            new String[]{automatic.code < 0 ? "自动 · 待确认" : "自动 · " + scsKhz(automatic.code) + " kHz",
                "15 kHz", "30 kHz", "60 kHz", "120 kHz", "240 kHz"});
        scsAdapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        scsPicker.setAdapter(scsAdapter);
        scsPicker.setSelection(scsChoice.manualCode() + 1);
        content.addView(scsPicker, width());
        content.addView(text(automatic.reason + "\n默认值并非实测 SCS。频段请在主页面点选。", 13, MUTED), buttonWidth());
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("高级设置 · SCS").setView(content)
            .setPositiveButton("确定", null).setNegativeButton("取消", null).create();
        dialog.setOnShowListener(ignored -> dialog.getButton(AlertDialog.BUTTON_POSITIVE).setOnClickListener(v -> {
            updateNrBandChoices();
            // The target or its band changed while the dialog was open; a stale SCS must not apply.
            if (!nrSa || !canChangeMode() || selectedSub != subscription || currentNrTarget() != target
                    || nrLockMode != mode || selectedNrBand != band
                    || (mode == MODE_CELL && !pciInput.getText().toString().equals(pci))) {
                dialog.dismiss();
                return;
            }
            int code = scsPicker.getSelectedItemPosition() - 1;
            if (mode == MODE_CELL && band < 0) {
                Toast.makeText(this, "请先在主页面点选目标 NR 频段", Toast.LENGTH_LONG).show();
                return;
            }
            if (code == -1 && automatic.code < 0) {
                Toast.makeText(this, "请确认 NR 频段或 SCS", Toast.LENGTH_LONG).show();
                return;
            }
            try { scsChoice.choose(code); }
            catch (IllegalArgumentException error) { Toast.makeText(this, error.getMessage(), Toast.LENGTH_LONG).show(); return; }
            renderBackendState();
            dialog.dismiss();
        }));
        dialog.show();
    }

    private void showMore() {
        new AlertDialog.Builder(this).setTitle("更多")
            .setItems(new String[]{"设备详情", "操作详情与日志", "使用说明", "导出诊断日志", "关于"}, (dialog, which) -> {
                if (which == 0) showDeviceDetails();
                else if (which == 1) showOperationDetails();
                else if (which == 2) showGuide();
                else if (which == 3) {
                    if (worker.busy) Toast.makeText(this, "请等待当前操作完成", Toast.LENGTH_SHORT).show();
                    else chooseLogDestination();
                } else showAbout();
            }).setNegativeButton("关闭", null).show();
    }

    private static final String[][] GUIDE = {
        {"1. 检测设备", "确认 Root 授权和固件兼容。检测、锁定及恢复会停止已确认的 Cellular-Pro 采集，不会自动重启采集。"},
        {"2. 选择目标", "先选“仅频点”或“频点＋PCI”，填写目标或点“填入当前”，再点底部“开始锁定”并在确认面板核对。仅支持第一卡槽为默认数据卡。LTE 的 EARFCN 为 0–65535、PCI 为 0–503；SA 的 PCI 为 0–1007。不支持 NSA。切换制式后重新检测。"},
        {"频段与 SCS", "支持 n1/n3/n8/n28/n41/n66/n77/n78/n79 的已核对范围，n1 包含 428910。优先跟随同一 SIM、同一目标的新鲜驻留上报；只有一个候选时按频点匹配。重叠频段无法确认时，在主页面点选频段，再点一次恢复自动；修改目标会重新匹配。SCS 默认为频段默认值，并非实测值，必要时可在高级设置更改。n1 尚待实机锁定验证。"},
        {"3. 锁定后核对", "操作会重启基带并短暂中断双卡网络。完成后约 2 分钟内每 5 秒自动读取一次小区，读到新鲜驻留即停止。配置已应用不等于已经驻留目标，当前匹配也不保证长期锁定。"},
        {"4. 恢复原配置", "恢复本次操作前保存的配置，可能包含其他 APP 原有的锁定。恢复依据保存的记录，不受当前制式或 SIM 选择影响。不要在锁定期间用其他工具改配置或升级固件。"},
        {"遇到异常", "保留恢复记录和诊断日志；卸载或清除数据不等于解除锁定。结果未知时不会自动重复重启。副卡搜网曾由用户手动切换飞行模式解决，不代表所有手机均适用。"}};

    /** Bold section titles make the long guide scannable. */
    private void showGuide() {
        android.text.SpannableStringBuilder body = new android.text.SpannableStringBuilder();
        for (String[] section : GUIDE) {
            if (body.length() > 0) body.append("\n\n");
            int start = body.length();
            body.append(section[0]);
            body.setSpan(new android.text.style.StyleSpan(Typeface.BOLD), start, body.length(), 0);
            body.append('\n').append(section[1]);
        }
        showInformation("使用说明", body, false);
    }

    private String versionName() {
        try { return getPackageManager().getPackageInfo(getPackageName(), 0).versionName; }
        catch (Exception ignored) { return "未知"; }
    }

    static final String DEVELOPER = "Liangvv";
    static final String PROJECT_URL = "https://github.com/sujuxiaoren/network-lock";

    /** The project link opens in the browser; this app itself has no network permission. */
    private void showAbout() {
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(24), dp(22), dp(24), dp(4));
        LinearLayout heading = row();
        android.widget.ImageView icon = new android.widget.ImageView(this);
        try { icon.setImageDrawable(getPackageManager().getApplicationIcon(getApplicationInfo())); }
        catch (Exception ignored) { icon.setVisibility(View.GONE); }
        icon.setImportantForAccessibility(View.IMPORTANT_FOR_ACCESSIBILITY_NO);
        heading.addView(icon, new LinearLayout.LayoutParams(dp(48), dp(48)));
        LinearLayout names = new LinearLayout(this);
        names.setOrientation(LinearLayout.VERTICAL);
        names.setPadding(dp(14), 0, 0, 0);
        TextView name = text("网络锁定", 18, INK);
        name.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        names.addView(name);
        names.addView(text("版本 " + versionName(), 13, MUTED));
        heading.addView(names, weighted());
        content.addView(heading, width());
        LinearLayout.LayoutParams first = width(); first.topMargin = dp(16);
        content.addView(aboutRow("开发者", DEVELOPER, false), first);
        content.addView(aboutRow("项目主页", PROJECT_URL.substring("https://".length()), true), width());
        TextView scope = text("支持 LTE 与 5G SA 锁频点、锁小区，需要 Root。\n适配 Xiaomi 13 Pro / Android 13 及已核对的基带和诊断库。\n每台手机独立保存恢复配置，请勿跨手机复制恢复文件。", 13, MUTED);
        scope.setPadding(0, dp(12), 0, 0);
        content.addView(scope, width());
        ScrollView scroll = new ScrollView(this);
        scroll.addView(content);
        new AlertDialog.Builder(this).setView(scroll).setPositiveButton("关闭", null).show();
    }

    private LinearLayout aboutRow(String label, String value, boolean link) {
        LinearLayout line = row();
        line.setMinimumHeight(dp(48));
        line.addView(text(label, 14, MUTED), new LinearLayout.LayoutParams(dp(72), -2));
        TextView field = text(value, 15, link ? ACTION_BLUE : INK);
        field.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        line.addView(field, weighted());
        if (!link) return line;
        line.addView(text("打开", 13, ACTION_BLUE));
        line.setBackground(new RippleDrawable(ColorStateList.valueOf(0x22174EA6), null, background(Color.WHITE, 8)));
        line.setContentDescription(label + " " + value + "，点按在浏览器中打开，长按复制网址");
        line.setOnClickListener(v -> openProjectPage());
        line.setOnLongClickListener(v -> { copyProjectUrl(); return true; });
        return line;
    }

    private void openProjectPage() {
        try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(PROJECT_URL))); }
        catch (Exception noBrowser) { copyProjectUrl(); }
    }

    private void copyProjectUrl() {
        android.content.ClipboardManager clipboard = getSystemService(android.content.ClipboardManager.class);
        if (clipboard == null) return;
        clipboard.setPrimaryClip(android.content.ClipData.newPlainText("网络锁定项目主页", PROJECT_URL));
        // Android 13+ shows its own copy confirmation.
        if (Build.VERSION.SDK_INT < 33) Toast.makeText(this, "已复制项目主页网址", Toast.LENGTH_SHORT).show();
    }

    private void showDeviceDetails() {
        showInformation("设备详情", Build.MANUFACTURER + " " + Build.MODEL + "\nAndroid "
            + Build.VERSION.RELEASE + " · API " + Build.VERSION.SDK_INT
            + (worker.miuiVersion.isEmpty() ? "" : " · MIUI " + worker.miuiVersion)
            + "\n系统版本 " + Build.DISPLAY + "\n\n"
            + (hasRadioPermissions() ? "电话与精确位置权限：已授予" : "电话或精确位置权限：未授予")
            + "\n系统定位：" + (locationEnabled() ? "已开启" : "已关闭")
            + "\n\n" + subscriptionExplanation, false);
    }

    private void showOperationDetails() {
        String backendLog = worker.backend == null ? "" : worker.backend.getLog();
        showInformation("操作详情与日志", worker.result + "\n\n界面记录\n" + worker.log
            + "\n\n后端记录\n" + backendLog, true);
    }

    private void showInformation(String title, CharSequence body, boolean monospace) {
        ScrollView scroll = new ScrollView(this);
        TextView content = text("", 14, INK);
        content.setText(body);
        if (monospace) content.setTypeface(Typeface.MONOSPACE);
        content.setTextIsSelectable(true);
        content.setPadding(dp(20), dp(12), dp(20), dp(20));
        scroll.addView(content);
        AlertDialog.Builder dialog = new AlertDialog.Builder(this).setTitle(title).setView(scroll)
            .setPositiveButton("关闭", null);
        if (monospace && !worker.busy) dialog.setNeutralButton("导出日志", (d, w) -> chooseLogDestination());
        dialog.show();
    }

    private void loadSubscriptions() {
        subscriptions.clear();
        List<String> labels = new ArrayList<>();
        try {
            RadioProbe.Snapshot snapshot = RadioProbe.inspect(this);
            subscriptions.addAll(snapshot.entries);
            defaultDataSub = snapshot.defaultDataSub;
            subscriptionExplanation = snapshot.explanation;
            worker.addLog("订阅接口：" + snapshot.detail);
            for (RadioProbe.SlotEntry info : subscriptions) {
                labels.add("SIM " + (info.slot + 1)
                        + (info.subId == defaultDataSub ? " · 数据卡" : ""));
            }
        } catch (Exception error) {
            defaultDataSub = SubscriptionManager.INVALID_SUBSCRIPTION_ID;
            subscriptionExplanation = "订阅查询失败：" + describe(error) + "；不会猜测订阅ID。";
            showError("读取 SIM 路由失败", error);
        }
        if (labels.isEmpty()) labels.add("暂无可用 SIM");
        int position = 0;
        if (preferredSub == SubscriptionManager.INVALID_SUBSCRIPTION_ID) {
            for (int i = 0; i < subscriptions.size(); i++) {
                if (subscriptions.get(i).subId == defaultDataSub) position = i;
            }
        }
        for (int i = 0; i < subscriptions.size(); i++) {
            if (subscriptions.get(i).subId == preferredSub) position = i;
        }
        buildSimButtons(labels);
        selectSubscription(subscriptions.isEmpty() ? -1 : position);
    }

    private void buildSimButtons(List<String> labels) {
        simGroup.removeAllViews();
        simButtons.clear();
        for (int i = 0; i < labels.size(); i++) {
            final int position = i;
            StateButton option = new StateButton(labels.get(i), STYLE_SEGMENT);
            option.setOnClickListener(v -> { if (!worker.busy && position != simPosition) selectSubscription(position); });
            simButtons.add(option);
            simGroup.addView(option, weighted());
        }
    }

    private void selectSubscription(int position) {
        int previous = selectedSub;
        selectedSub = -1;
        selectedSlot = -1;
        simPosition = -1;
        if (position >= 0 && position < subscriptions.size()) {
            RadioProbe.SlotEntry info = subscriptions.get(position);
            selectedSub = info.subId;
            selectedSlot = info.slot;
            preferredSub = selectedSub;
            simPosition = position;
        }
        boolean changed = selectedSub != previous;
        if (changed) {
            if (previous >= 0 || selectedSub != restoredScsSub) scsChoice.clearOverride();
            if (previous >= 0 || selectedSub != restoredScsSub) {
                bandChoice.clearOverride();
                setNrBand(-1);
            }
            // A real SIM switch ends post-operation polling; activity re-creation (previous < 0) keeps it.
            if (previous >= 0) { stopPolling(); pendingFill = false; }
            ++readSequence;
            reading = false;
            currentEarfcn = currentPci = currentCellSub = -1;
            currentNrBands = new int[0];
            cellText.setText(idleCellText());
            cellMeta.setText("");
            signalBars.setLevel(-1);
        }
        restoredScsSub = -1;
        renderBackendState();
        if (changed) autoReadCell();
    }

    private boolean hasRadioPermissions() {
        return checkSelfPermission(Manifest.permission.READ_PHONE_STATE) == PackageManager.PERMISSION_GRANTED
                && checkSelfPermission(Manifest.permission.ACCESS_FINE_LOCATION) == PackageManager.PERMISSION_GRANTED;
    }

    private void requestRadioPermissions() {
        requestPermissions(new String[]{Manifest.permission.READ_PHONE_STATE,
                Manifest.permission.ACCESS_COARSE_LOCATION, Manifest.permission.ACCESS_FINE_LOCATION}, PERMISSIONS);
    }

    private boolean locationEnabled() {
        try {
            LocationManager manager = getSystemService(LocationManager.class);
            return manager != null && manager.isLocationEnabled();
        } catch (Exception ignored) { return false; }
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(code, permissions, grants);
        if (code == PERMISSIONS) {
            loadSubscriptions();
            if (hasRadioPermissions() && selectedSub >= 0 && !reading) readCurrentCell();
            else if (!hasRadioPermissions()) showError("未获得所需权限；Root 检测仍可使用，小区读取和未知卡槽写入已禁用。", null);
        }
    }

    private void readCurrentCell() { readCurrentCell(false); }

    /** quiet: automatic read-only refresh; never prompts for permissions or opens dialogs. */
    private void readCurrentCell(boolean quiet) {
        if (worker.busy || (quiet && reading)) return;
        if (!hasRadioPermissions()) { if (!quiet) requestRadioPermissions(); else readFinished(); return; }
        if (selectedSub < 0) { if (!quiet) showError("未选择可读取的活动 SIM。", null); readFinished(); return; }
        final int sequence = ++readSequence;
        final int subscription = selectedSub;
        reading = true;
        currentEarfcn = currentPci = currentCellSub = -1;
        currentNrBands = new int[0];
        if (polling()) showPollingText("");
        else { cellText.setText("正在读取…"); cellMeta.setText(""); }
        renderBackendState();
        main.postDelayed(() -> {
            if (!destroyed && sequence == readSequence && reading) {
                reading = false;
                ++readSequence;
                if (polling()) showPollingText("本次读取超时");
                else {
                    cellText.setText("读取超时，请重试");
                    cellMeta.setText("请检查定位权限、定位开关和 SIM 状态");
                }
                worker.addLog("小区读取超时");
                readFinished();
                renderBackendState();
            }
        }, 12000);
        try {
            TelephonyManager manager = getSystemService(TelephonyManager.class);
            if (manager == null) throw new IllegalStateException("系统电话服务不可用");
            manager.createForSubscriptionId(subscription).requestCellInfoUpdate(worker.executor,
                    new TelephonyManager.CellInfoCallback() {
                        @Override public void onCellInfo(List<CellInfo> cells) {
                            main.post(() -> displayCell(sequence, subscription, cells));
                        }
                        @Override public void onError(int code, Throwable detail) {
                            main.post(() -> {
                                if (destroyed || sequence != readSequence) return;
                                reading = false;
                                if (polling()) showPollingText("系统错误 " + code);
                                else cellText.setText("小区读取失败（系统错误 " + code + "）。");
                                if (quiet) worker.addLog("小区读取失败（" + code + "）" + (detail == null ? "" : "：" + describe(detail)));
                                else showError("小区读取失败（" + code + "）", detail);
                                readFinished();
                                renderBackendState();
                            });
                        }
                    });
        } catch (Exception error) {
            reading = false;
            ++readSequence;
            if (polling()) showPollingText("无法请求小区更新");
            else cellText.setText("无法请求小区更新。");
            if (quiet) worker.addLog("无法请求小区更新：" + describe(error));
            else showError("无法请求小区更新", error);
            readFinished();
            renderBackendState();
        }
    }

    /** Settles a pending one-tap fill when a read ends without usable evidence. */
    private void readFinished() {
        if (!pendingFill) return;
        pendingFill = false;
        Toast.makeText(this, "暂未读到可用的当前小区", Toast.LENGTH_SHORT).show();
    }

    private void fillCurrentTarget() {
        if (!canChangeMode()) return;
        if (applyCurrentTarget()) return;
        if (!hasRadioPermissions()) { requestRadioPermissions(); return; }
        if (selectedSub < 0) { showError("未选择可读取的活动 SIM。", null); return; }
        // Evidence is missing or older than 30 s: refresh once, then fill from the new reading.
        pendingFill = true;
        if (!reading) readCurrentCell(true);
    }

    /** Copies the fresh serving cell into the form; never starts an operation. */
    private boolean applyCurrentTarget() {
        if (!canFillCurrentTarget() || (nrSa && !RadioProbe.reportsNrData(this, selectedSub))) return false;
        pendingFill = false;
        earfcnInput.setText(String.valueOf(currentEarfcn));
        if (activeLockMode() == MODE_CELL) pciInput.setText(String.valueOf(currentPci));
        if (nrSa) setNrBand(-1);
        hideKeyboard();
        renderBackendState();
        return true;
    }

    private void displayCell(int sequence, int subscription, List<CellInfo> cells) {
        if (destroyed || sequence != readSequence || subscription != selectedSub) return;
        reading = false;
        CellInfo serving = null;
        if (cells != null) for (CellInfo cell : cells) {
            if ((nrSa ? cell instanceof CellInfoNr : cell instanceof CellInfoLte) && cell.isRegistered()
                    && (serving == null || cell.getTimeStamp() > serving.getTimeStamp()))
                serving = cell;
        }
        if (serving == null) {
            signalBars.setLevel(-1);
            if (polling()) showPollingText(nrSa ? "暂无已注册的 NR 小区" : "暂无已注册的 LTE 小区");
            else {
                cellText.setText(nrSa ? "暂无已注册的 NR 小区" : "暂无已注册的 LTE 小区");
                cellMeta.setText("更新于 " + timeNow());
            }
            worker.addLog(nrSa ? "此订阅未返回已注册 NR" : "此订阅未返回已注册 LTE");
            readFinished();
        } else {
            final int observedEarfcn, observedPci, rsrp;
            int[] observedNrBands = new int[0];
            String bands = "";
            if (nrSa) {
                CellInfoNr nr = (CellInfoNr) serving;
                CellIdentityNr identity = (CellIdentityNr) nr.getCellIdentity();
                observedEarfcn = identity.getNrarfcn();
                observedPci = identity.getPci();
                rsrp = ((CellSignalStrengthNr) nr.getCellSignalStrength()).getSsRsrp();
                if (Build.VERSION.SDK_INT >= 30) {
                    observedNrBands = identity.getBands();
                    for (int band : observedNrBands) {
                        if (band > 0) bands += (bands.isEmpty() ? " · " : "/") + "n" + band;
                    }
                }
            } else {
                CellInfoLte lte = (CellInfoLte) serving;
                CellIdentityLte identity = lte.getCellIdentity();
                observedEarfcn = identity.getEarfcn();
                observedPci = identity.getPci();
                rsrp = lte.getCellSignalStrength().getRsrp();
            }
            long nowMs = SystemClock.elapsedRealtime();
            long cellTimeMs = serving.getTimeStamp() / 1_000_000L;
            boolean timestampKnown = cellTimeMs > 0 && cellTimeMs <= nowMs + 1000L;
            long ageMs = timestampKnown ? Math.max(0, nowMs - cellTimeMs) : -1;
            boolean postOperationFresh = worker.cellEvidenceAfterMs == 0
                    || (timestampKnown && cellTimeMs >= worker.cellEvidenceAfterMs);
            boolean verifiedSa = !nrSa || RadioProbe.reportsNrData(this, subscription);
            boolean usable = cellEvidenceUsable(nrSa, verifiedSa, timestampKnown, ageMs,
                    cellTimeMs, worker.cellEvidenceAfterMs);
            cellReadAt = timeNow();
            String summary = (nrSa ? "NR-ARFCN " : "EARFCN ") + available(observedEarfcn) + "  ·  PCI " + available(observedPci);
            int level = UiWidgets.signalLevel(nrSa, rsrp);
            signalBars.setLevel(level);
            String metadata = (nrSa ? "SS-RSRP " : "RSRP ") + (rsrp == CellInfo.UNAVAILABLE ? "未知" : rsrp + " dBm")
                    + (level < 0 ? "" : " · " + UiWidgets.signalWord(level)) + bands
                    + "\n" + timeNow() + (timestampKnown ? " · 数据 " + (ageMs / 1000) + " 秒前" : " · 数据时间未知");
            if (usable) {
                stopPolling();
                currentEarfcn = observedEarfcn;
                currentPci = observedPci;
                currentNrBands = observedNrBands.clone();
                currentCellSub = subscription;
                currentNrSa = nrSa;
                currentCellTimeMs = timestampKnown ? cellTimeMs : 0;
                if (nrSa) {
                    main.postDelayed(() -> {
                        if (!destroyed && sequence == readSequence && subscription == selectedSub)
                            renderBackendState();
                    }, Math.max(1L, 30001L - ageMs));
                }
            } else if (!verifiedSa) {
                metadata += "\n未确认 SA 数据注册，不能核对本次目标。";
            } else {
                metadata += "\n缓存数据，暂不能核对本次操作。请重新刷新。";
            }
            if (!usable && polling()) {
                // While the network recovers, an old or unverified reading is not news: keep the wait visible.
                signalBars.setLevel(-1);
                showPollingText(verifiedSa ? "暂未读到操作后的新鲜数据" : "SA 数据注册尚未恢复");
            } else {
                cellText.setText(summary);
                cellMeta.setText(metadata);
            }
            if (pendingFill && (!usable || !applyCurrentTarget())) readFinished();
            worker.addLog("小区读取：slot=" + selectedSlot + (nrSa ? " NR_ARFCN=" : " EARFCN=") + available(observedEarfcn)
                    + " PCI=" + available(observedPci) + " RSRP=" + (rsrp == CellInfo.UNAVAILABLE ? "未知" : rsrp)
                    + " age_ms=" + ageMs + " post_operation_fresh=" + postOperationFresh
                    + " usable=" + usable + (nrSa ? " sa_data_registered=" + verifiedSa : ""));
        }
        renderBackendState();
    }

    private void requestLock() {
        final LockTarget target = currentLockTarget();
        if (target == null) return;
        final int operationSub = selectedSub;
        hideKeyboard();
        confirmSheet("确认锁定", lockSummary(target),
            "· 将重启基带，双卡网络会短暂中断\n· 会停止已确认的 Cellular-Pro 采集，不会自动重启\n· 配置已应用不等于已驻留目标；完成后自动刷新核对",
            "开始锁定", () -> {
                LockTarget confirmed = currentLockTarget();
                if (confirmed == null) return;
                if (selectedSub != operationSub || !sameTarget(target, confirmed)) {
                    Toast.makeText(this, "目标已变化，请重新核对", Toast.LENGTH_LONG).show();
                    return;
                }
                worker.run(target.operationLabel(),
                    backend -> {
                        if (!RadioProbe.isDefaultDataOnSlotZero(operationSub)) throw new IllegalStateException("默认移动数据 SIM 已改变，未执行锁定。");
                        String output = target.execute(backend);
                        worker.appliedTarget = target;
                        worker.appliedSub = operationSub;
                        return output;
                    }, true);
            });
    }

    /** All pre-lock gates from 0.2.6, unchanged; returns null after telling the user why. */
    private LockTarget currentLockTarget() {
        if (!writeAllowed()) { showError("锁定需后端就绪、无待恢复记录，且所选第一卡槽是默认移动数据 SIM。", null); return null; }
        try {
            if (!RadioProbe.isDefaultDataOnSlotZero(selectedSub)) {
                loadSubscriptions();
                showError("默认移动数据 SIM 或卡槽映射已改变，未执行锁定。", null);
                return null;
            }
        } catch (Exception error) { showError("无法复核默认移动数据 SIM，未执行锁定", error); return null; }
        try {
            updateNrBandChoices();
            if (nrSa && bandSelection.conflicts())
                throw new IllegalArgumentException("手动频段与当前小区上报不一致，请点选恢复自动或核对目标。");
            if (nrSa && nrLockMode == MODE_CELL && NrBandPolicy.find(currentNrTarget(), selectedNrBand) == null) {
                Toast.makeText(this, "请先点选目标 NR 频段", Toast.LENGTH_LONG).show();
                return null;
            }
            if (nrSa && nrLockMode == MODE_EARFCN && scsChoice.manualCode() == -1
                    && nrScsSelection(nrLockMode, parseNrArfcn(earfcnInput.getText().toString()), selectedNrBand).code < 0) {
                showNrAdvanced();
                return null;
            }
            return nrSa ? resolveNrTarget(nrLockMode, earfcnInput.getText().toString(),
                    nrLockMode == MODE_CELL ? pciInput.getText().toString() : null, scsChoice.manualCode(), selectedNrBand)
                : parseLockTarget(lockMode, earfcnInput.getText().toString(),
                    lockMode == MODE_CELL ? pciInput.getText().toString() : null);
        } catch (Exception error) { showError("请检查锁定目标", error); return null; }
    }

    private static boolean sameTarget(LockTarget a, LockTarget b) {
        return a.nrSa == b.nrSa && a.earfcn == b.earfcn && a.scsCode == b.scsCode && a.nrBand == b.nrBand
            && (a.pci == null ? b.pci == null : a.pci.equals(b.pci));
    }

    private String lockSummary(LockTarget target) {
        String mode = target.pci == null ? "仅频点" : "频点＋PCI";
        String value = (target.nrSa ? "5G SA · " + mode + "\nNR-ARFCN " : "LTE · " + mode + "\nEARFCN ") + target.earfcn
            + (target.pci == null ? " · 不限 PCI" : " / PCI " + target.pci);
        if (target.nrSa) {
            if (target.pci != null) value += "\n频段 n" + target.nrBand;
            else if (selectedNrBand > 0) value += "\n频段 n" + selectedNrBand + "（仅用于确定 SCS）";
            value += "\nSCS " + scsKhz(target.scsCode) + " kHz" + (scsChoice.manualCode() >= 0 ? "（手动）" : "（默认值，非实测）");
        }
        return value + "\nSIM 1 · 默认数据卡";
    }

    private void requestRestore() {
        if (!restoreAllowed()) { showError("当前无法恢复，请先查看设备状态", null); return; }
        hideKeyboard();
        confirmSheet("恢复原配置", "回到本次锁定前保存的配置",
            "· 将重启基带，双卡网络会短暂中断\n· 会停止已确认的 Cellular-Pro 采集，不会自动重启\n· 原配置可能包含其他 APP 已有的限制",
            "恢复原配置", () -> {
                if (!restoreAllowed()) return;
                worker.run("恢复修改前配置并重启基带", RootBackend::restore, true, true);
            });
    }

    private void runProbe() {
        if (worker.busy) return;
        hideKeyboard();
        final boolean requestedNrSa = nrSa;
        worker.probedNrSa = requestedNrSa;
        worker.run("检测 Root / 设备能力", backend -> backend.probe(requestedNrSa));
    }

    private void runPrimaryAction() {
        int action = primaryAction();
        if (action == PRIMARY_RESTORE) requestRestore();
        else if (action == PRIMARY_PROBE) runProbe();
        else requestLock();
    }

    /** Footer button: what the user most likely needs next. Gates stay in each action. */
    private int primaryAction() {
        if (worker.backendAttempted && worker.restorable && !worker.recoveryUnknown) return PRIMARY_RESTORE;
        if (!worker.probed || worker.probedNrSa != nrSa || !worker.ready || worker.recoveryUnknown
                || showRestoreNotice()) return PRIMARY_PROBE;
        return PRIMARY_LOCK;
    }

    private void invalidateCellEvidence(String message) {
        ++readSequence;
        reading = false;
        pendingFill = false;
        currentEarfcn = currentPci = currentCellSub = -1;
        currentNrBands = new int[0];
        cellText.setText(message);
        cellMeta.setText("");
        if (signalBars != null) signalBars.setLevel(-1);
    }

    private String idleCellText() {
        if (!hasRadioPermissions()) return "授予权限后可读取当前小区";
        if (!locationEnabled()) return "开启系统定位后可读取当前小区";
        return "点击刷新，查看当前小区";
    }

    /** Quiet read-only refresh used at launch, SIM/制式 switch and after a probe. */
    private void autoReadCell() {
        if (destroyed || worker.busy || reading) return;
        if (polling()) { main.removeCallbacks(pollTick); main.postDelayed(pollTick, 500); return; }
        if (hasRadioPermissions() && locationEnabled() && selectedSub >= 0) readCurrentCell(true);
    }

    private boolean polling() { return worker.pollUntilMs > SystemClock.elapsedRealtime(); }

    private void stopPolling() {
        worker.pollUntilMs = 0;
        main.removeCallbacks(pollTick);
    }

    private void showPollingText(String detail) {
        long waited = Math.max(0, SystemClock.elapsedRealtime() - worker.pollStartMs) / 1000;
        cellText.setText("等待网络恢复…");
        cellMeta.setText("已等待 " + waited + " 秒 · 每 5 秒自动刷新" + (detail.isEmpty() ? "" : "\n" + detail));
    }

    /** One quiet read per tick until a fresh post-operation cell arrives or the window ends. */
    private void pollCell() {
        if (destroyed || worker.busy) return;
        if (!polling()) {
            if (worker.pollUntilMs != 0 && currentCellSub != selectedSub) {
                cellText.setText("网络尚未恢复");
                cellMeta.setText("已自动刷新约 2 分钟，可稍后点刷新再核对");
            }
            worker.pollUntilMs = 0;
            renderBackendState();
            return;
        }
        if (!reading) readCurrentCell(true);
        main.postDelayed(pollTick, POLL_INTERVAL_MS);
    }

    private boolean cellCurrent() {
        return currentCellSub == selectedSub && currentNrSa == nrSa && currentCellSub >= 0
            && nrObservationCurrent(currentCellTimeMs, SystemClock.elapsedRealtime());
    }

    private boolean writeAllowed() { return worker.probed && worker.probedNrSa == nrSa && worker.ready && !worker.recoveryUnknown && !worker.restorable && !worker.busy
            && selectedSub >= 0 && selectedSlot == 0 && selectedSub == defaultDataSub; }
    private boolean restoreAllowed() { return worker.restorable && !worker.busy; }
    private boolean showRestoreNotice() {
        return worker.restoreNotice.visible(worker.busy, worker.recoveryUnknown, worker.restorable);
    }

    private void renderBackendState() {
        if (destroyed || resultText == null) return;
        String reason;
        String recoveryReason = recoveryReadiness(worker.backendAttempted, worker.recoveryUnknown, worker.restorable, worker.probed);
        if (worker.busy) reason = "请等待操作完成";
        else if (showRestoreNotice()) reason = "再次锁定前，请检测设备";
        else if (recoveryReason != null) reason = recoveryReason;
        else if (!worker.ready) reason = "请查看最近操作的结果";
        else if (selectedSub < 0 || selectedSlot < 0) reason = "请授予权限并更新 SIM";
        else if (selectedSlot != 0) reason = "请选择 SIM 1";
        else if (selectedSub != defaultDataSub) reason = "请将 SIM 1 设为默认数据卡";
        else reason = nrSa ? "SIM 1 · 5G SA · 可设置目标" : "SIM 1 · LTE · 可设置目标";
        readinessText.setText(reason);
        renderReadinessAppearance();
        // Progress lives in the footer while busy; the card shows only the finished result.
        resultPanel.setVisibility(worker.hasResult && !worker.busy ? View.VISIBLE : View.GONE);
        operationText.setText(worker.completedAt);
        resultText.setText(UiPresentation.summary(worker.operation, worker.result, worker.failed));
        resultText.setTextColor(worker.failed ? DANGER : INK);
        String nextStep = worker.busy ? "" : showRestoreNotice() ? "再次锁定前，请检测设备"
            : worker.ready && !writeAllowed()
            ? reason : UiPresentation.nextStep(worker.failed, worker.recoveryUnknown, worker.restorable, worker.ready);
        nextStepText.setText(nextStep);
        nextStepText.setVisibility(nextStep.isEmpty() || (!worker.failed && writeAllowed()) ? View.GONE : View.VISIBLE);
        detailsButton.setEnabled(!worker.busy);
        moreButton.setEnabled(!worker.busy);
        grantButton.setEnabled(!worker.busy);
        for (int i = 0; i < simButtons.size(); i++) {
            simButtons.get(i).setPrimary(i == simPosition);
            simButtons.get(i).setEnabled(!worker.busy && !subscriptions.isEmpty());
        }
        readButton.setEnabled(!reading && !worker.busy);
        ((StateButton) readButton).setLabel(reading ? "读取中…" : "刷新");
        boolean frequencyOnly = activeLockMode() == MODE_EARFCN;
        lteButton.setEnabled(canChangeMode());
        nrButton.setEnabled(canChangeMode());
        ((StateButton) lteButton).setPrimary(!nrSa);
        ((StateButton) nrButton).setPrimary(nrSa);
        lteModes.setVisibility(View.VISIBLE);
        nrBandGroup.setVisibility(nrSa ? View.VISIBLE : View.GONE);
        updateNrBandChoices();
        renderNrScs();
        frequencyLabel.setText(nrSa ? "NR-ARFCN" : "EARFCN");
        earfcnInput.setContentDescription(nrSa ? "5G SA NR-ARFCN" : "LTE EARFCN");
        pciInput.setContentDescription(nrSa ? "5G SA PCI，0 至 1007" : "LTE PCI，0 至 503");
        frequencyModeButton.setEnabled(canChangeMode());
        cellModeButton.setEnabled(canChangeMode());
        frequencyModeButton.setChecked(frequencyOnly);
        cellModeButton.setChecked(!frequencyOnly);
        ((StateButton) fillButton).setLabel(frequencyOnly ? "填入当前频点" : "填入当前小区");
        fillButton.setEnabled(canChangeMode() && selectedSub >= 0 && !reading);
        earfcnInput.setEnabled(canChangeMode());
        int ime = frequencyOnly ? EditorInfo.IME_ACTION_DONE : EditorInfo.IME_ACTION_NEXT;
        if (earfcnInput.getImeOptions() != ime) earfcnInput.setImeOptions(ime);
        pciGroup.setVisibility(frequencyOnly ? View.GONE : View.VISIBLE);
        pciInput.setEnabled(canChangeMode() && !frequencyOnly);
        renderInputErrors(frequencyOnly);
        boolean permissionMissing = !hasRadioPermissions();
        boolean locationMissing = !locationEnabled();
        permissionPanel.setVisibility(permissionMissing || locationMissing || selectedSub < 0 ? View.VISIBLE : View.GONE);
        permissionText.setText(permissionMissing ? "读取小区需要电话与精确位置权限"
            : locationMissing ? "请开启系统定位以读取小区" : "未找到可验证的 SIM，请更新列表");
        ((StateButton) grantButton).setLabel(permissionMissing ? "授予权限" : locationMissing ? "开启定位" : "更新 SIM");
        LockTarget applied = worker.appliedTarget;
        appliedTargetText.setVisibility(applied == null ? View.GONE : View.VISIBLE);
        if (applied != null) {
            boolean sameSub = currentCellSub == worker.appliedSub && currentNrSa == applied.nrSa;
            boolean observedAfterOperation = sameSub && (applied.nrSa ? validNrArfcn(currentEarfcn) : validEarfcn(currentEarfcn));
            boolean matches = observedAfterOperation && currentEarfcn == applied.earfcn
                && (applied.pci == null || currentPci == applied.pci.intValue());
            boolean unknown = worker.recoveryUnknown || worker.failed;
            appliedTargetText.setText("本次应用目标：" + (applied.nrSa ? "NR-ARFCN " : "EARFCN ") + applied.earfcn
                + (applied.pci == null ? " · 不限 PCI" : " / PCI " + applied.pci)
                + (applied.nrSa && applied.pci != null ? " · n" + applied.nrBand : "")
                + (applied.nrSa ? " · SCS " + scsKhz(applied.scsCode) + " kHz" : "")
                + "\n" + (unknown ? "当前配置待确认，请查看详情"
                    : matches ? "上次读取匹配 · " + cellReadAt
                    : observedAfterOperation ? "上次读取未匹配 · " + cellReadAt
                    : polling() ? "正在自动核对驻留…" : "待刷新核对驻留"));
            appliedTargetText.setTextColor(unknown ? WARN_INK : matches ? UiWidgets.GOOD
                : observedAfterOperation ? WARN_INK : MUTED);
        }
        renderFooter();
    }

    private void renderFooter() {
        StateButton primary = (StateButton) lockButton;
        int action = primaryAction();
        String issue = worker.busy || action != PRIMARY_LOCK ? null : writeAllowed() ? targetIssue() : lockBlockReason();
        if (worker.busy) primary.setLabel("正在处理…");
        else if (action == PRIMARY_RESTORE) primary.setLabel("恢复原配置");
        else if (action == PRIMARY_PROBE) primary.setLabel(worker.probed || worker.backendAttempted ? "重新检测" : "检测设备");
        else primary.setLabel("开始锁定");
        primary.setEnabled(!worker.busy && (action == PRIMARY_RESTORE ? restoreAllowed()
            : action == PRIMARY_PROBE || (writeAllowed() && issue == null)));
        boolean secondaryRestore = !worker.busy && action != PRIMARY_RESTORE && restoreAllowed();
        restoreButton.setVisibility(secondaryRestore ? View.VISIBLE : View.GONE);
        probeButton.setVisibility(!worker.busy && action != PRIMARY_PROBE ? View.VISIBLE : View.GONE);
        progress.setVisibility(worker.busy ? View.VISIBLE : View.GONE);
        final String hint;
        final int color;
        if (worker.busy) { hint = busyHint(); color = ACTION_BLUE; }
        else if (action == PRIMARY_RESTORE) { hint = "有待恢复记录 · 恢复后才可再次锁定"; color = WARN_INK; }
        // Visible while the keyboard covers the inline message.
        else if (inputError.getVisibility() == View.VISIBLE) { hint = inputError.getText().toString(); color = DANGER; }
        else if (action == PRIMARY_PROBE) { hint = probeHint(); color = MUTED; }
        else if (issue != null) { hint = issue; color = WARN_INK; }
        else { hint = actionPreview(); color = INK; }
        footerHint.setText(hint);
        footerHint.setTextColor(color);
        main.removeCallbacks(busyTick);
        if (worker.busy) main.postDelayed(busyTick, 1000);
    }

    private void tickBusy() {
        if (destroyed || !worker.busy) return;
        footerHint.setText(busyHint());
        main.postDelayed(busyTick, 1000);
    }

    private String busyHint() {
        long seconds = Math.max(0, SystemClock.elapsedRealtime() - worker.busySinceMs) / 1000;
        String task = worker.operation.startsWith("检测") ? "正在检测设备"
            : worker.operation.startsWith("恢复") ? "正在恢复原配置并重启基带"
            : worker.operation.startsWith("导出") ? "正在导出诊断日志" : "正在写入配置并重启基带";
        return task + "… " + seconds + " 秒";
    }

    private String probeHint() {
        if (showRestoreNotice()) return "已恢复原配置 · 再次锁定前请重新检测";
        if (worker.recoveryUnknown && worker.backendAttempted) return "恢复记录状态未确认，请重新检测";
        if (worker.probed && worker.probedNrSa != nrSa) return "制式已切换，请重新检测";
        if (worker.probed && !worker.ready) return "检测未通过 · 处理后重新检测";
        return "先检测 Root 与固件兼容 · 会停止 Cellular-Pro 采集";
    }

    /** Same order as the readiness text; only the SIM-related reasons can remain after a passing probe. */
    private String lockBlockReason() {
        if (selectedSub < 0 || selectedSlot < 0) return "请授予权限并更新 SIM";
        if (selectedSlot != 0) return "请选择 SIM 1";
        if (selectedSub != defaultDataSub) return "请将 SIM 1 设为默认数据卡";
        return "暂不可锁定，请查看最近操作";
    }

    /** Display-only reason the current form cannot be locked yet; requestLock() still re-checks everything. */
    private String targetIssue() {
        String frequency = earfcnInput.getText().toString().trim();
        boolean cellMode = activeLockMode() == MODE_CELL;
        String pci = pciInput.getText().toString().trim();
        if (frequency.isEmpty()) return "填写" + (nrSa ? " NR-ARFCN" : " EARFCN") + (cellMode ? " 和 PCI" : "")
            + "，或点“填入当前" + (cellMode ? "小区" : "频点") + "”";
        if (cellMode && pci.isEmpty()) return "填写 PCI";
        if (inputError.getVisibility() == View.VISIBLE) return "请修正上方输入";
        if (!nrSa) return null;
        int target = currentNrTarget();
        if (bandSelection.conflicts()) return "所选频段与当前上报不同，请核对或点选恢复自动";
        if (cellMode && selectedNrBand < 0) return NrBandPolicy.candidates(target).isEmpty()
            ? "此频点暂无已核对的 NR 频段" : "请点选目标 NR 频段";
        if (scsChoice.manualCode() < 0 && nrScsSelection(nrLockMode, target, selectedNrBand).code < 0)
            return "SCS 待确认 · 请点选频段或在高级设置中选择";
        return null;
    }

    private void renderInputErrors(boolean frequencyOnly) {
        String frequency = earfcnInput.getText().toString().trim();
        String pci = pciInput.getText().toString().trim();
        String earfcnError = frequency.isEmpty() ? null : nrSa
            ? (numberIn(frequency, 0, 3279165) ? null : "NR-ARFCN 须为 0–3279165")
            : (numberIn(frequency, 0, 65535) ? null : "锁定仅支持 EARFCN 0–65535");
        int pciMax = nrSa ? 1007 : 503;
        String pciError = frequencyOnly || pci.isEmpty() || numberIn(pci, 0, pciMax) ? null : "PCI 须为 0–" + pciMax;
        setFieldError(earfcnInput, earfcnError != null);
        setFieldError(pciInput, pciError != null);
        String message = earfcnError != null ? earfcnError : pciError;
        inputError.setText(message == null ? "" : message);
        inputError.setVisibility(message == null ? View.GONE : View.VISIBLE);
    }

    private static boolean numberIn(String value, int min, int max) {
        if (!value.matches("[0-9]{1,7}")) return false;
        int number = Integer.parseInt(value);
        return number >= min && number <= max;
    }

    private void setFieldError(EditText field, boolean error) {
        if (Boolean.valueOf(error).equals(field.getTag())) return;
        field.setTag(error);
        field.setBackground(fieldBackground(error));
        field.setPadding(dp(14), dp(10), dp(14), dp(10));
    }

    private void renderReadinessAppearance() {
        final String label;
        final int fill, ink, border;
        if (worker.busy) {
            label = "处理中"; fill = 0xFFE8F0FE; ink = ACTION_BLUE; border = 0xFFB6CCF2;
        } else if (showRestoreNotice()) {
            label = "已恢复原配置"; fill = 0xFFE8F5EE; ink = 0xFF166534; border = 0xFFB7DDC6;
        } else if (!worker.backendAttempted || (!worker.probed && !worker.recoveryUnknown && !worker.restorable)) {
            label = "待检测"; fill = 0xFFEDF2F7; ink = 0xFF334155; border = 0xFFCBD5E1;
        } else if (writeAllowed()) {
            label = "就绪"; fill = 0xFFE8F5EE; ink = 0xFF166534; border = 0xFFB7DDC6;
        } else if (worker.backendAttempted && worker.restorable) {
            label = worker.recoveryUnknown ? "状态待确认" : "待恢复";
            fill = 0xFFFFF6E8; ink = 0xFF85540A; border = 0xFFEFD5A9;
        } else {
            label = "暂不可锁定"; fill = 0xFFFDEBEC; ink = 0xFF9B1C1C; border = 0xFFF2B8B5;
        }
        readinessLabel.setText(label);
        readinessLabel.setTextColor(ink);
        readinessText.setTextColor(ink);
        GradientDrawable shape = background(fill, 12);
        shape.setStroke(dp(1), border);
        readinessPanel.setBackground(shape);
    }

    static String recoveryReadiness(boolean attempted, boolean unknown, boolean restorable, boolean probed) {
        if (!attempted) return "请先检测设备";
        if (unknown) return "恢复状态未确认，请检测或查看详情";
        if (restorable) return "有待恢复记录，请先恢复原配置";
        if (!probed) return "请先检测设备";
        return null;
    }

    private void hideKeyboard() {
        View focused = getCurrentFocus();
        InputMethodManager keyboard = getSystemService(InputMethodManager.class);
        if (focused != null && keyboard != null) keyboard.hideSoftInputFromWindow(focused.getWindowToken(), 0);
        if (page != null) page.requestFocus();
    }

    /** Bottom confirmation for radio-changing actions; onConfirm must re-check its own gates. */
    private void confirmSheet(String title, String summary, String notes, String confirmLabel, Runnable onConfirm) {
        Dialog sheet = new Dialog(this);
        LinearLayout content = new LinearLayout(this);
        content.setOrientation(LinearLayout.VERTICAL);
        content.setPadding(dp(20), dp(20), dp(20), dp(16));
        GradientDrawable shape = new GradientDrawable();
        shape.setColor(Color.WHITE);
        float radius = dp(20);
        shape.setCornerRadii(new float[]{radius, radius, radius, radius, 0, 0, 0, 0});
        content.setBackground(shape);
        TextView heading = text(title, 18, INK);
        heading.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        content.addView(heading, width());
        TextView body = text(summary, 16, INK);
        body.setPadding(dp(14), dp(12), dp(14), dp(12));
        body.setBackground(background(0xFFF1F5F9, 12));
        LinearLayout.LayoutParams bodyParams = width(); bodyParams.topMargin = dp(12);
        content.addView(body, bodyParams);
        TextView detail = text(notes, 13, MUTED);
        detail.setPadding(0, dp(12), 0, 0);
        content.addView(detail, width());
        LinearLayout actions = row();
        StateButton cancel = new StateButton("取消", STYLE_OUTLINE);
        cancel.setOnClickListener(v -> sheet.dismiss());
        actions.addView(cancel, weighted());
        StateButton confirm = new StateButton(confirmLabel, STYLE_FILLED);
        confirm.setMinimumHeight(dp(52));
        confirm.setOnClickListener(v -> {
            sheet.dismiss();
            if (!destroyed && !worker.busy) onConfirm.run();
        });
        LinearLayout.LayoutParams confirmParams = weighted(); confirmParams.leftMargin = dp(10);
        actions.addView(confirm, confirmParams);
        content.addView(actions, buttonWidth());
        sheet.setContentView(content);
        sheet.setCanceledOnTouchOutside(true);
        android.view.Window window = sheet.getWindow();
        if (window != null) {
            window.setBackgroundDrawable(new ColorDrawable(Color.TRANSPARENT));
            window.setGravity(Gravity.BOTTOM);
            window.setWindowAnimations(android.R.style.Animation_InputMethod);
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND);
            window.setDimAmount(0.35f);
        }
        sheet.show();
        if (window != null) window.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
    }

    /** Called by the retained worker after each backend operation finishes. */
    private void operationFinished(String label, boolean changesRadio) {
        if (changesRadio && polling()) {
            main.removeCallbacks(pollTick);
            showPollingText("");
            main.postDelayed(pollTick, POLL_FIRST_MS);
        } else if (label.startsWith("检测")) autoReadCell();
    }

    private void chooseLogDestination() {
        try {
            Intent intent = new Intent(Intent.ACTION_CREATE_DOCUMENT);
            intent.addCategory(Intent.CATEGORY_OPENABLE);
            intent.setType("text/plain");
            intent.putExtra(Intent.EXTRA_TITLE, "lte-cell-lock-" + new SimpleDateFormat("yyyyMMdd-HHmmss", Locale.US).format(new Date()) + ".txt");
            startActivityForResult(intent, EXPORT_LOG);
        } catch (Exception error) { showError("无法打开日志保存位置", error); }
    }

    @Override protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (request != EXPORT_LOG || result != RESULT_OK || data == null || data.getData() == null) return;
        final Uri destination = data.getData();
        final String uiLog = worker.log.toString();
        worker.run("导出诊断日志", backend -> {
            String contents = "网络锁定 " + versionName() + "\n设备：" + Build.MANUFACTURER + " " + Build.MODEL
                    + "\nAndroid " + Build.VERSION.RELEASE + " / " + Build.DISPLAY
                    + "\n\n界面记录\n" + uiLog + "\n\n后端记录\n" + backend.getLog();
            try (OutputStream out = getApplicationContext().getContentResolver().openOutputStream(destination, "w")) {
                if (out == null) throw new IllegalStateException("无法打开目标文件");
                out.write(contents.getBytes(StandardCharsets.UTF_8));
            }
            return "诊断日志已写入你选择的文件。";
        });
    }

    private void showError(String message, Throwable detail) {
        String full = message + (detail == null ? "" : "：" + describe(detail));
        worker.addLog(full);
        showInformation("操作提示", full, false);
    }

    @Override protected void onSaveInstanceState(Bundle state) {
        state.putString("earfcn", earfcnInput.getText().toString());
        state.putString("pci", pciInput.getText().toString());
        state.putInt("subscription", selectedSub);
        state.putInt(MODE_PREFERENCE, lockMode);
        state.putInt(NR_MODE_PREFERENCE, nrLockMode);
        state.putBoolean(RADIO_PREFERENCE, nrSa);
        state.putInt(SCS_TARGET_STATE, scsChoice.target());
        state.putInt(SCS_MANUAL_STATE, scsChoice.manualCode());
        state.putInt(NR_BAND_STATE, bandChoice.manualBand());
        super.onSaveInstanceState(state);
    }

    @Override protected void onResume() {
        super.onResume();
        if (worker == null || simGroup == null || worker.busy) return;
        loadSubscriptions();
        if (polling()) { main.removeCallbacks(pollTick); main.postDelayed(pollTick, 500); }
        else {
            // A window that ended while away is not resumed; a normal quiet read replaces it.
            worker.pollUntilMs = 0;
            if (!cellCurrent()) autoReadCell();
        }
    }

    @Override protected void onPause() {
        main.removeCallbacks(pollTick);
        super.onPause();
    }

    @Override public Object onRetainNonConfigurationInstance() { return worker; }

    @Override protected void onDestroy() {
        destroyed = true;
        ++readSequence;
        main.removeCallbacksAndMessages(null);
        worker.detach(this);
        if (isFinishing()) worker.executor.shutdown();
        super.onDestroy();
    }

    private LinearLayout card(LinearLayout parent, String heading) {
        LinearLayout card = new LinearLayout(this);
        card.setOrientation(LinearLayout.VERTICAL);
        card.setPadding(dp(16), dp(14), dp(16), dp(14));
        card.setBackground(outlined(Color.WHITE, LINE, 16));
        card.setLayoutTransition(quickTransition(false));
        LinearLayout.LayoutParams params = width(); params.topMargin = dp(12);
        parent.addView(card, params);
        if (!heading.isEmpty()) {
            TextView title = text(heading, 16, INK);
            title.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            title.setPadding(0, 0, 0, dp(10)); card.addView(title);
        }
        return card;
    }
    private TextView text(String value, int size, int color) {
        TextView view = new TextView(this); view.setText(value); view.setTextSize(size); view.setTextColor(color);
        view.setLineSpacing(dp(3), 1); return view;
    }
    private EditText input(String hint) {
        EditText field = new EditText(this); field.setHint(hint); field.setSingleLine(true);
        field.setTextSize(20); field.setTextColor(INK); field.setHintTextColor(0xFF94A3B8);
        field.setMinimumHeight(dp(52)); field.setImeOptions(EditorInfo.IME_ACTION_NEXT);
        field.setInputType(InputType.TYPE_CLASS_NUMBER); field.setSelectAllOnFocus(true);
        setFieldError(field, false);
        return field;
    }
    /** Rounded outline; blue when focused, red when the value is out of range. */
    private android.graphics.drawable.StateListDrawable fieldBackground(boolean error) {
        android.graphics.drawable.StateListDrawable states = new android.graphics.drawable.StateListDrawable();
        states.addState(new int[]{-android.R.attr.state_enabled}, outlined(0xFFF1F5F9, LINE, 12));
        GradientDrawable focused = outlined(Color.WHITE, error ? DANGER : ACTION_BLUE, 12);
        focused.setStroke(dp(2), error ? DANGER : ACTION_BLUE);
        states.addState(new int[]{android.R.attr.state_focused}, focused);
        states.addState(new int[]{}, outlined(Color.WHITE, error ? DANGER : 0xFFCBD5E1, 12));
        return states;
    }
    private LinearLayout.LayoutParams fieldParams() { LinearLayout.LayoutParams params = width(); params.topMargin = dp(4); return params; }
    private GradientDrawable outlined(int fill, int stroke, int radius) {
        GradientDrawable shape = background(fill, radius); shape.setStroke(dp(1), stroke); return shape;
    }
    /** Grey track holding equal-width segment buttons (制式, SIM, NR 频段). */
    private LinearLayout segmentGroup() {
        LinearLayout group = row();
        group.setPadding(dp(3), dp(3), dp(3), dp(3));
        group.setBackground(background(0xFFE8EDF3, 12));
        return group;
    }
    /** Short resize animation so cards grow and shrink instead of jumping. */
    private void animateChanges(ViewGroup group) { group.setLayoutTransition(quickTransition(true)); }

    /** Framework defaults wait 300 ms before fading views in; keep feedback immediate. */
    private LayoutTransition quickTransition(boolean changing) {
        LayoutTransition transition = new LayoutTransition();
        if (changing) transition.enableTransitionType(LayoutTransition.CHANGING);
        transition.setDuration(160);
        transition.setStartDelay(LayoutTransition.APPEARING, 0);
        transition.setStartDelay(LayoutTransition.CHANGE_DISAPPEARING, 0);
        transition.setStartDelay(LayoutTransition.CHANGING, 0);
        return transition;
    }
    private Button button(String label, int style) {
        return new StateButton(label, style);
    }
    private RadioButton modeOption(String label) {
        RadioButton option = new RadioButton(this);
        option.setId(View.generateViewId());
        option.setText(label);
        option.setTextSize(15);
        option.setTextColor(INK);
        option.setMinimumHeight(dp(48));
        option.setPadding(dp(4), dp(8), dp(8), dp(8));
        option.setButtonTintList(new ColorStateList(
            new int[][]{new int[]{-android.R.attr.state_enabled}, new int[]{android.R.attr.state_checked}, new int[]{}},
            new int[]{0xFF9CA3AF, 0xFF16734A, 0xFF64748B}));
        option.setContentDescription("锁定方式：" + label);
        return option;
    }

    private String actionPreview() {
        String frequency = earfcnInput.getText().toString().trim();
        if (frequency.isEmpty()) return "填写目标后开始锁定";
        String target = (nrSa ? "SA · " : "LTE · ") + frequency;
        if (activeLockMode() == MODE_CELL) {
            String pci = pciInput.getText().toString().trim();
            target += " / PCI " + (pci.isEmpty() ? "待填写" : pci);
        } else target += " · 不限 PCI";
        if (nrSa && selectedNrBand > 0) target += " · n" + selectedNrBand;
        return target;
    }
    private final class StateButton extends Button {
        private String label;
        private int style;
        private boolean selectedOption;
        StateButton(String label, int style) {
            super(MainActivity.this);
            this.label = label; this.style = style;
            boolean segment = style == STYLE_SEGMENT;
            setAllCaps(false); setTextSize(segment ? 14 : 15);
            setTypeface(Typeface.DEFAULT, Typeface.BOLD);
            int height = segment ? 44 : 48;
            setMinimumHeight(dp(height)); setMinHeight(dp(height));
            setMinimumWidth(0); setMinWidth(0);
            setPadding(dp(style == STYLE_QUIET ? 12 : 10), dp(8), dp(style == STYLE_QUIET ? 12 : 10), dp(8));
            setGravity(Gravity.CENTER); setSingleLine(false);
            setBackgroundTintList(null); setStateListAnimator(null);
            refreshAppearance();
        }
        @Override public void setEnabled(boolean enabled) {
            boolean changed = enabled != isEnabled();
            super.setEnabled(enabled);
            if (label != null && changed) refreshAppearance();
        }
        void setLabel(String value) {
            if (!value.equals(label)) { label = value; refreshAppearance(); }
        }
        /** Segment: selected option. Action buttons: filled (true) or outlined (false). */
        void setPrimary(boolean value) {
            if (style == STYLE_SEGMENT) {
                if (selectedOption != value) { selectedOption = value; refreshAppearance(); }
                return;
            }
            if (style == STYLE_QUIET) return;
            int next = value ? STYLE_FILLED : STYLE_OUTLINE;
            if (style != next) { style = next; refreshAppearance(); }
        }
        private void refreshAppearance() {
            boolean enabled = isEnabled();
            setText(label);
            setSelected(selectedOption);
            setContentDescription(label + (selectedOption ? "，已选择" : "") + (enabled ? "，可操作" : "，不可用"));
            int fill, ink, stroke, ripple;
            if (style == STYLE_FILLED) {
                fill = enabled ? ACTION_BLUE : 0xFFE2E8F0; ink = enabled ? Color.WHITE : 0xFF64748B;
                stroke = fill; ripple = 0x40FFFFFF;
            } else if (style == STYLE_SEGMENT) {
                fill = selectedOption ? Color.WHITE : Color.TRANSPARENT;
                ink = !enabled ? 0xFF94A3B8 : selectedOption ? ACTION_BLUE : MUTED;
                stroke = selectedOption ? 0xFFCBD5E1 : Color.TRANSPARENT; ripple = 0x22174EA6;
            } else if (style == STYLE_QUIET) {
                fill = Color.TRANSPARENT; ink = enabled ? ACTION_BLUE : 0xFF94A3B8;
                stroke = Color.TRANSPARENT; ripple = 0x22174EA6;
            } else {
                fill = enabled ? Color.WHITE : 0xFFF1F5F9; ink = enabled ? ACTION_BLUE : 0xFF94A3B8;
                stroke = enabled ? 0xFFC5D3E8 : LINE; ripple = 0x22174EA6;
            }
            int radius = style == STYLE_SEGMENT ? 10 : 12;
            GradientDrawable shape = outlined(fill, stroke, radius);
            // Explicit mask keeps the ripple visible on transparent quiet/segment buttons.
            setBackground(new RippleDrawable(ColorStateList.valueOf(ripple), shape, background(Color.WHITE, radius)));
            setTextColor(ink);
        }
    }
    private GradientDrawable background(int color, int radius) {
        GradientDrawable shape = new GradientDrawable(); shape.setColor(color); shape.setCornerRadius(dp(radius)); return shape;
    }
    private LinearLayout.LayoutParams width() { return new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT); }
    private LinearLayout.LayoutParams buttonWidth() { LinearLayout.LayoutParams params = width(); params.topMargin = dp(12); return params; }
    private LinearLayout row() {
        LinearLayout value = new LinearLayout(this);
        value.setOrientation(LinearLayout.HORIZONTAL);
        value.setGravity(Gravity.CENTER_VERTICAL);
        return value;
    }
    private LinearLayout.LayoutParams weighted() { return new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f); }
    private int dp(int value) { return Math.round(value * getResources().getDisplayMetrics().density); }
    private void addSpace(LinearLayout parent, int height) { View gap = new View(this); parent.addView(gap, new LinearLayout.LayoutParams(1, dp(height))); }
    private static String timeNow() { return new SimpleDateFormat("HH:mm:ss", Locale.getDefault()).format(new Date()); }
    private static String describe(Throwable error) { return error.getClass().getSimpleName() + (error.getMessage() == null ? "" : ": " + error.getMessage()); }
    private static boolean validEarfcn(int value) { return value >= 0 && value <= 262143; }
    private static boolean validLockEarfcn(int value) { return value >= 0 && value <= 65535; }
    private static boolean validPci(int value) { return value >= 0 && value <= 503; }
    static boolean validNrArfcn(int value) { return value >= 0 && value <= 3279165; }
    static boolean validNrPci(int value) { return value >= 0 && value <= 1007; }
    static int scsKhz(int code) {
        if (code < 0 || code > 4) throw new IllegalArgumentException("请选择有效的 SCS。");
        return 15 << code;
    }
    static boolean loadNrSa(SharedPreferences preferences) {
        try { return preferences.getBoolean(RADIO_PREFERENCE, false); }
        catch (ClassCastException invalidPreference) { return false; }
    }
    static boolean cellEvidenceUsable(boolean nrSa, boolean saDataRegistered, boolean timestampKnown,
                                      long ageMs, long cellTimeMs, long operationAfterMs) {
        boolean postOperation = operationAfterMs == 0 || (timestampKnown && cellTimeMs >= operationAfterMs);
        return postOperation && (!nrSa || (saDataRegistered && timestampKnown && ageMs >= 0 && ageMs <= 30000));
    }
    static boolean modeChangeAllowed(boolean busy, boolean backendAttempted, boolean recoveryUnknown, boolean restorable) {
        return !busy && (!backendAttempted || (!recoveryUnknown && !restorable));
    }
    static boolean nrObservationCurrent(long cellTimeMs, long nowMs) {
        return cellTimeMs > 0 && cellTimeMs <= nowMs + 1000L && Math.max(0, nowMs - cellTimeMs) <= 30000L;
    }
    private static int parseNrArfcn(String arfcnText) {
        String value = arfcnText == null ? "" : arfcnText.trim();
        if (!value.matches("[0-9]+")) throw new IllegalArgumentException("请输入有效的 NR-ARFCN（0–3279165）。");
        final int arfcn;
        try { arfcn = Integer.parseInt(value); }
        catch (NumberFormatException error) { throw new IllegalArgumentException("请输入有效的 NR-ARFCN（0–3279165）。"); }
        if (!validNrArfcn(arfcn)) throw new IllegalArgumentException("NR-ARFCN 须为 0–3279165。");
        return arfcn;
    }
    static LockTarget parseNrTarget(String arfcnText, int scsCode) {
        return parseNrTarget(MODE_EARFCN, arfcnText, null, scsCode, -1);
    }
    static LockTarget parseNrTarget(int mode, String arfcnText, String pciText, int scsCode, int band) {
        int arfcn = parseNrArfcn(arfcnText);
        scsKhz(scsCode);
        if (normalizeNrLockMode(mode) == MODE_EARFCN) return new LockTarget(arfcn, null, scsCode, -1);
        if (NrBandPolicy.find(arfcn, band) == null) throw new IllegalArgumentException("请选择目标的 NR 频段。");
        String value = pciText == null ? "" : pciText.trim();
        if (!value.matches("[0-9]+")) throw new IllegalArgumentException("请输入有效的 PCI（0–1007）。");
        final int pci;
        try { pci = Integer.parseInt(value); }
        catch (NumberFormatException invalid) { throw new IllegalArgumentException("请输入有效的 PCI（0–1007）。"); }
        if (!validNrPci(pci)) throw new IllegalArgumentException("5G SA 锁小区的 PCI 须为 0–1007。");
        return new LockTarget(arfcn, pci, scsCode, band);
    }
    static LockTarget resolveNrTarget(String arfcnText, int manualCode) {
        return resolveNrTarget(MODE_EARFCN, arfcnText, null, manualCode, -1);
    }
    static LockTarget resolveNrTarget(int mode, String arfcnText, String pciText, int manualCode, int band) {
        int arfcn = parseNrArfcn(arfcnText);
        if (manualCode < -1 || manualCode > 4) throw new IllegalArgumentException("请选择有效的 SCS。");
        int code = manualCode == -1 ? nrScsSelection(mode, arfcn, band).code : manualCode;
        if (code < 0) throw new IllegalArgumentException("此频点的 SCS 待确认，请打开高级设置。");
        return parseNrTarget(mode, arfcnText, pciText, code, band);
    }
    static NrScsPolicy.Selection nrScsSelection(int mode, int arfcn, int band) {
        NrBandPolicy.Band selected = NrBandPolicy.find(arfcn, band);
        if (selected == null && normalizeNrLockMode(mode) == MODE_EARFCN) return NrScsPolicy.resolve(arfcn);
        return selected == null ? new NrScsPolicy.Selection(-1, "请先选择目标的 NR 频段", "")
            : new NrScsPolicy.Selection(selected.scsCode, "按 n" + band + " 频段默认 " + scsKhz(selected.scsCode) + " kHz", "n" + band);
    }
    static int uniqueObservedNrBand(int arfcn, int[] bands) {
        return NrBandPolicy.uniqueObserved(arfcn, bands);
    }
    static int normalizeNrLockMode(int mode) { return mode == MODE_CELL ? MODE_CELL : MODE_EARFCN; }
    static int loadNrLockMode(SharedPreferences preferences) {
        try { return normalizeNrLockMode(preferences.getInt(NR_MODE_PREFERENCE, MODE_EARFCN)); }
        catch (ClassCastException invalidPreference) { return MODE_EARFCN; }
    }
    static void saveNrLockMode(SharedPreferences preferences, int mode) {
        preferences.edit().putInt(NR_MODE_PREFERENCE, normalizeNrLockMode(mode)).apply();
    }
    static int normalizeLockMode(int mode) { return mode == MODE_EARFCN ? MODE_EARFCN : MODE_CELL; }
    static int loadLockMode(SharedPreferences preferences) {
        try { return normalizeLockMode(preferences.getInt(MODE_PREFERENCE, MODE_CELL)); }
        catch (ClassCastException invalidPreference) { return MODE_CELL; }
    }
    static void saveLockMode(SharedPreferences preferences, int mode) {
        preferences.edit().putInt(MODE_PREFERENCE, normalizeLockMode(mode)).apply();
    }
    static boolean canFillTarget(int mode, int currentSub, int selectedSub, int earfcn, int pci) {
        return selectedSub >= 0 && currentSub == selectedSub && validEarfcn(earfcn)
            && (normalizeLockMode(mode) == MODE_EARFCN || validPci(pci));
    }
    static boolean canFillNrTarget(int mode, int currentSub, int selectedSub, int arfcn, int pci,
                                   long cellTimeMs, long nowMs) {
        return selectedSub >= 0 && currentSub == selectedSub && validNrArfcn(arfcn)
            && nrObservationCurrent(cellTimeMs, nowMs)
            && (normalizeNrLockMode(mode) == MODE_EARFCN || validNrPci(pci));
    }
    static LockTarget parseLockTarget(int mode, String earfcnText, String pciText) {
        final int earfcn;
        try { earfcn = Integer.parseInt(earfcnText.trim()); }
        catch (Exception invalid) { throw new IllegalArgumentException("请输入有效的 EARFCN（0–65535）。"); }
        if (!validLockEarfcn(earfcn))
            throw new IllegalArgumentException("锁定写入仅支持 EARFCN 0–65535。当前小区可显示更大 EARFCN，但本后端不能写入。");
        if (normalizeLockMode(mode) == MODE_EARFCN) return new LockTarget(earfcn, null);
        final int pci;
        try { pci = Integer.parseInt(pciText == null ? "" : pciText.trim()); }
        catch (NumberFormatException invalid) { throw new IllegalArgumentException("请输入有效的 PCI（0–503）。"); }
        if (!validPci(pci)) throw new IllegalArgumentException("锁小区模式的 PCI 须为 0–503。");
        return new LockTarget(earfcn, pci);
    }
    static final class LockTarget {
        final int earfcn;
        final Integer pci;
        final boolean nrSa;
        final int scsCode;
        final int nrBand;
        LockTarget(int earfcn, Integer pci) {
            this.earfcn = earfcn; this.pci = pci; this.nrSa = false; this.scsCode = -1; this.nrBand = -1;
        }
        private LockTarget(int arfcn, Integer pci, int scsCode, int band) {
            this.earfcn = arfcn; this.pci = pci; this.nrSa = true; this.scsCode = scsCode; this.nrBand = band;
        }
        String operationLabel() {
            if (nrSa) return (pci == null ? "锁 SA 频点" : "锁 SA 小区") + "并重启基带：NR-ARFCN=" + earfcn
                + (pci == null ? "" : "，PCI=" + pci)
                + (pci == null ? "" : "，频段=n" + nrBand)
                + "，SCS=" + scsKhz(scsCode) + " kHz，slot=0";
            return (pci == null ? "锁频点" : "锁小区") + "并重启基带：EARFCN=" + earfcn
                + (pci == null ? "" : "，PCI=" + pci) + "，slot=0";
        }
        String execute(RootBackend backend) {
            if (nrSa) return pci == null ? backend.lockNrArfcn(earfcn, scsCode) : backend.lockNrCell(earfcn, pci, scsCode, nrBand);
            return pci == null ? backend.lockEarfcn(earfcn, 0) : backend.lock(earfcn, pci, 0);
        }
    }
    private static String available(int value) { return value == CellInfo.UNAVAILABLE || value < 0 ? "未知" : String.valueOf(value); }

    private interface BackendTask { String execute(RootBackend backend) throws Exception; }

    /** Retained across rotation. No Activity is held while the UI is detached. */
    private static final class Worker {
        final ExecutorService executor = Executors.newSingleThreadExecutor();
        final Handler main = new Handler(Looper.getMainLooper());
        final Context context;
        final StringBuilder log = new StringBuilder();
        final UiPresentation.RestoreNotice restoreNotice = new UiPresentation.RestoreNotice();
        RootBackend backend;
        MainActivity owner;
        boolean busy, ready, probed, probedNrSa, backendAttempted, systemInfoRequested;
        boolean failed, hasResult;
        String completedAt = "";
        volatile LockTarget appliedTarget;
        volatile int appliedSub = -1;
        long cellEvidenceAfterMs;
        // Retained across rotation so a recreated screen keeps the same busy and polling timeline.
        long busySinceMs, pollStartMs, pollUntilMs;
        boolean restorable = true, recoveryUnknown = true;
        String result = "尚未执行检测。", operation = "";
        String blockedReason = "请运行 Root / 设备检测。", miuiVersion = "";
        Worker(Context context) { this.context = context; }
        void attach(MainActivity activity) { owner = activity; activity.renderBackendState(); }
        void detach(MainActivity activity) { if (owner == activity) owner = null; }
        void readSystemInfo() {
            if (systemInfoRequested) return;
            systemInfoRequested = true;
            executor.execute(() -> {
                String version = "";
                Process process = null;
                try {
                    process = new ProcessBuilder("/system/bin/getprop", "ro.miui.ui.version.name")
                            .redirectErrorStream(true).start();
                    if (process.waitFor(2, TimeUnit.SECONDS) && process.exitValue() == 0) {
                        try (BufferedReader reader = new BufferedReader(new InputStreamReader(process.getInputStream(), StandardCharsets.UTF_8))) {
                            String line = reader.readLine();
                            if (line != null) version = line.trim();
                        }
                    }
                } catch (InterruptedException error) { Thread.currentThread().interrupt(); }
                catch (Exception ignored) { /* Build.DISPLAY remains available. */ }
                finally { if (process != null) process.destroy(); }
                final String detected = version;
                main.post(() -> { miuiVersion = detected; if (owner != null) owner.renderBackendState(); });
            });
        }
        void addLog(String message) {
            log.append(timeNow()).append("  ").append(message).append('\n');
            if (log.length() > 100000) log.delete(0, log.length() - 80000);
        }
        void run(String label, BackendTask task) {
            run(label, task, false);
        }
        void run(String label, BackendTask task, boolean changesRadio) {
            run(label, task, changesRadio, false);
        }
        void run(String label, BackendTask task, boolean changesRadio, boolean restoreOperation) {
            if (busy) return;
            final boolean updateCapability = !label.equals("导出诊断日志");
            restoreNotice.begin(updateCapability);
            if (updateCapability) backendAttempted = true;
            busy = true; operation = label; busySinceMs = SystemClock.elapsedRealtime(); addLog("开始：" + label);
            if (changesRadio) {
                appliedTarget = null;
                appliedSub = -1;
                pollUntilMs = 0;
                cellEvidenceAfterMs = SystemClock.elapsedRealtime();
                if (owner != null) { owner.stopPolling(); owner.invalidateCellEvidence("操作完成后自动刷新小区"); }
            }
            if (owner != null) owner.renderBackendState();
            final boolean previousReady = ready;
            final boolean previousRestorable = restorable;
            final boolean previousRecoveryUnknown = recoveryUnknown;
            executor.execute(() -> {
                String output; boolean allowed = previousReady; boolean recoverable = previousRestorable; boolean failed = false;
                boolean returnedResult = false;
                boolean recoveryStatusUnknown = previousRecoveryUnknown;
                try {
                    if (backend == null) backend = new RootBackend(context);
                    output = task.execute(backend);
                    returnedResult = output != null && !output.trim().isEmpty();
                    if (output == null || output.trim().isEmpty()) output = "后端未返回结果；不能确认操作成功。";
                } catch (Exception | LinkageError error) {
                    output = "操作未完成：" + describe(error); failed = true;
                }
                if (updateCapability) {
                    // Backend operations refresh recovery; copy their state even when the operation failed.
                    try { allowed = backend != null && backend.canWrite(probedNrSa); }
                    catch (Exception | LinkageError error) { allowed = false; output += "\n锁定能力检查失败：" + describe(error); }
                    boolean recoveryQueryFailed = false;
                    try { recoverable = backend == null || backend.canRestore(); }
                    catch (Exception | LinkageError error) {
                        recoverable = true; recoveryQueryFailed = true;
                        output += "\n恢复能力检查失败：" + describe(error);
                    }
                    try { recoveryStatusUnknown = recoveryQueryFailed || backend == null || backend.isRecoveryUnknown(); }
                    catch (Exception | LinkageError error) {
                        recoveryStatusUnknown = true;
                        output += "\n恢复记录状态未确认：" + describe(error);
                    }
                    if (recoveryStatusUnknown) recoverable = true;
                }
                if (restoreOperation && !failed && (!returnedResult || recoveryStatusUnknown || recoverable)) {
                    output = "恢复完成状态未确认，请查看详情并核对恢复记录。\n" + output;
                    failed = true;
                }
                final String completed = output;
                final boolean writable = allowed, canRecover = recoverable, unknownRecovery = recoveryStatusUnknown, failure = failed;
                final boolean hasReturnedResult = returnedResult;
                main.post(() -> {
                    busy = false; ready = writable; restorable = canRecover; recoveryUnknown = unknownRecovery;
                    restoreNotice.complete(restoreOperation, failure, hasReturnedResult, unknownRecovery, canRecover);
                    this.failed = failure; hasResult = true; completedAt = timeNow();
                    if (!unknownRecovery && !canRecover) { appliedTarget = null; appliedSub = -1; }
                    if (changesRadio) {
                        cellEvidenceAfterMs = SystemClock.elapsedRealtime();
                        pollStartMs = cellEvidenceAfterMs;
                        pollUntilMs = cellEvidenceAfterMs + POLL_WINDOW_MS;
                    }
                    if (label.equals("检测 Root / 设备能力")) probed = true;
                    if (updateCapability && !writable) blockedReason = completed;
                    result = completed; addLog(label + "\n" + completed);
                    if (owner != null) {
                        if (changesRadio) owner.invalidateCellEvidence("等待网络恢复，自动刷新中…");
                        owner.renderBackendState();
                        owner.operationFinished(label, changesRadio);
                        if (failure) owner.showInformation("操作未完成", completed, false);
                    }
                });
            });
        }
    }
}
