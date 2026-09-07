#pragma once
// Serial console. Every line the launcher prints starts with "[launcher]" (or
// "[syscheck]"), and tools/flash.py keys off a few of them:
//   "[launcher] registered"  after `register`
//   "[launcher] booting"     after `boot`
//   "[launcher] menu"        when the menu is on screen
//   "[syscheck] done"        at the end of `syscheck`
//   "[screenshot] end"       after the base64 framebuffer dump of `screenshot`
namespace console {
void poll();
// True once any byte has arrived on the console (vestigial; kept for callers).
bool sawInput();
}
