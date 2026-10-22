#include "sysinfo.h"

#include "common.h"
#include "log.h"

static const char *target_name(AudioTarget t) {
    switch (t) {
    case AudioTarget_Speaker:         return "Speaker";
    case AudioTarget_Headphone:       return "Headphone jack";
    case AudioTarget_Tv:              return "TV";
    case AudioTarget_UsbOutputDevice: return "USB output device";
    case AudioTarget_Bluetooth:       return "Bluetooth";
    default:                          return "Invalid";
    }
}

static void report_audio(void) {
    Result rc = auddevInitialize();
    if (R_SUCCEEDED(rc)) {
        static AudioDeviceName names[16];
        s32 total = 0;
        rc = auddevListAudioDeviceName(names, 16, &total);
        if (R_SUCCEEDED(rc)) {
            log_both("auddev: %d output name(s):", total);
            for (s32 i = 0; i < total && i < 16; i++) log_both(" %s", names[i].name);
            log_both("\n");
        } else {
            log_both("auddev: ListAudioDeviceName %s\n", rc_str(rc));
        }
        AudioDeviceName active = {0};
        rc = auddevGetActiveAudioDeviceName(&active);
        log_both("auddev: active output = %s (%s)\n", R_SUCCEEDED(rc) ? active.name : "?", rc_str(rc));
        auddevExit();
    } else {
        log_both("auddev: unavailable %s\n", rc_str(rc));
    }

    rc = audctlInitialize();
    if (R_FAILED(rc)) {
        log_both("audctl: unavailable %s\n", rc_str(rc));
        return;
    }
    AudioTarget t = AudioTarget_Invalid;
    rc = audctlGetDefaultTarget(&t);
    log_both("audctl: default target = %s (%s)", R_SUCCEEDED(rc) ? target_name(t) : "?", rc_str(rc));
    t = AudioTarget_Invalid;
    rc = audctlGetActiveOutputTarget(&t);
    log_both(", active = %s (%s)\n", R_SUCCEEDED(rc) ? target_name(t) : "?", rc_str(rc));
    if (hosversionBefore(18, 0, 0)) {
        bool usb = false;
        rc = audctlIsTargetConnected(&usb, AudioTarget_UsbOutputDevice);
        log_both("audctl: USB output device connected = %s (%s)\n", R_SUCCEEDED(rc) ? (usb ? "YES" : "no") : "?",
                 rc_str(rc));
    }
    audctlExit();
}

// usb:pm GetPowerState output (switchbrew "UsbPowerState").
typedef struct {
    u32 role;      // 0 unknown, 1 sink, 2 source
    u32 charger;
    u32 voltage;
    u32 current;
    u32 pdo;
    u32 rdo;
} UsbPowerState;

static void report_usb_port(void) {
    Service pm;
    Result rc = smGetService(&pm, "usb:pm");
    if (R_FAILED(rc)) {
        log_both("usb:pm: unavailable %s\n", rc_str(rc));
        return;
    }
    UsbPowerState st;
    memset(&st, 0, sizeof(st));
    rc = serviceDispatch(&pm, 1,
        .buffer_attrs = { SfBufferAttr_HipcMapAlias | SfBufferAttr_Out },
        .buffers = { { &st, sizeof(st) } },
    );
    static const char *roles[] = {"unknown", "sink (charging)", "SOURCE (powers the device)"};
    if (R_SUCCEEDED(rc))
        log_both("usb:pm: power role %s, charger type %u, %u mV %u mA\n", st.role < 3 ? roles[st.role] : "?",
                 st.charger, st.voltage, st.current);
    else
        log_both("usb:pm: GetPowerState %s\n", rc_str(rc));

    u32 data_role = 0;
    rc = serviceDispatchOut(&pm, 3, data_role);
    static const char *droles[] = {"unknown", "DFP (USB host)", "UFP (USB device)"};
    log_both("usb:pm: data role %s (%s)\n", R_SUCCEEDED(rc) && data_role < 3 ? droles[data_role] : "?", rc_str(rc));
    serviceClose(&pm);
}

void sysinfo_report(void) {
    u32 v = hosversionGet();
    log_both(C_CYAN "== System ==" C_RESET " HOS %u.%u.%u\n", HOSVER_MAJOR(v), HOSVER_MINOR(v), HOSVER_MICRO(v));
    report_audio();
    report_usb_port();
}
