#ifndef DUAL_SCREEN_HUD_H
#define DUAL_SCREEN_HUD_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Returns true when a secondary (physically separate) display is attached and the user has the
// "Dual Screen HUD" enhancement enabled. When false, callers must not redirect any drawing and
// behavior must be identical to a single-screen device.
uint8_t DualScreenHUD_IsActive(void);

// Lazily creates (on first use) and returns the id of the offscreen framebuffer that the core HUD
// (hearts, magic, rupees, minimap, action buttons) should be redirected into via gsSPSetFB/gsSPResetFB
// while DualScreenHUD_IsActive() is true. Returns -1 if the framebuffer could not be created.
int32_t DualScreenHUD_GetFrameBufferId(void);

// Presents the current contents of the HUD framebuffer onto the secondary display, if one is attached.
// Must be called once per rendered frame, after the main frame has finished drawing (so the HUD
// framebuffer's texture is up to date) and while the main render context is current. No-op on
// non-Android platforms and when no secondary display is attached.
void DualScreenHUD_PresentFrame(void);

// Must be called immediately before/after redirecting OVERLAY_DISP into the HUD framebuffer (i.e.
// around the Interface_Draw call). HUD element positions are computed from the renderer's current
// aspect ratio so they stay anchored to the true edges of whatever they're drawn into; without this,
// they'd be positioned for the main (often widescreen) display while actually being drawn into the
// HUD framebuffer's fixed 4:3 space, spreading them out far wider than that buffer actually is.
void DualScreenHUD_BeginHudPass(void);
void DualScreenHUD_EndHudPass(void);

#ifdef __cplusplus
}
#endif

#endif // DUAL_SCREEN_HUD_H
