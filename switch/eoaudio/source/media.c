#include "media.h"

#include "log.h"

static struct {
    bool                init;
    Result              init_rc;
    HiddbgHdlsSessionId session;
    u8                 *work;
} H;

static Result hdls_init(void) {
    if (H.init) return H.init_rc;
    H.init = true;
    H.init_rc = hiddbgInitialize();
    if (R_SUCCEEDED(H.init_rc)) {
        H.work = memalign(0x1000, 0x1000);
        H.init_rc = H.work ? hiddbgAttachHdlsWorkBuffer(&H.session, H.work, 0x1000)
                           : MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    }
    log_printf("media: hid:dbg virtual controller support: %s", rc_str(H.init_rc));
    return H.init_rc;
}

Result media_press(eo_button button) {
    Result rc = hdls_init();
    if (R_FAILED(rc)) return rc;
    HiddbgHdlsDeviceInfo info;
    memset(&info, 0, sizeof(info));
    info.deviceType = HidDeviceType_JoyRight1;
    info.npadInterfaceType = HidNpadInterfaceType_Rail;   // merge with the Joy-Con on the rail
    info.singleColorBody = RGBA8_MAXALPHA(0x40, 0x40, 0x40);
    info.singleColorButtons = RGBA8_MAXALPHA(0xFF, 0xFF, 0xFF);
    info.colorLeftGrip = info.colorRightGrip = info.singleColorBody;
    HiddbgHdlsHandle handle;
    rc = hiddbgAttachHdlsVirtualDevice(&handle, &info);
    if (R_FAILED(rc)) {
        log_printf("media: AttachHdlsVirtualDevice: %s", rc_str(rc));
        return rc;
    }
    HiddbgHdlsState st;
    memset(&st, 0, sizeof(st));
    st.battery_level = 4;
    st.buttons = BIT((u32)button);
    Result rp = hiddbgSetHdlsState(handle, &st);
    svcSleepThread(80000000ULL);
    st.buttons = 0;
    Result rr = hiddbgSetHdlsState(handle, &st);
    svcSleepThread(60000000ULL);
    Result rd = hiddbgDetachHdlsVirtualDevice(handle);
    log_printf("media: pressed %s (press %s, release %s, detach %s)", cfg_button_name(button), rc_str(rp), rc_str(rr),
               rc_str(rd));
    return R_FAILED(rp) ? rp : rr;
}
